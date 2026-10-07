/* ==========================================================================
 * rcp_car.c - 🚃 Rolling stock: the individual cars that make up a train.
 *
 * Before this file a train was atomic: it carried a `carriages` count and the
 * controller derived a length from it (carriages * 23 + 20). That is enough
 * to move a rectangle around a diagram, but it cannot answer the questions a
 * signaller actually asks about a formation:
 *
 *     - what is on this train, and in what order?
 *     - how long is it, and therefore which track circuits does it straddle?
 *     - is any vehicle defective, overloaded, or out of service?
 *     - where would I detach a portion?
 *
 * So a train now owns an ordered list of cars. The train's length is the sum
 * of its cars - the derived value, not an independent one - so the two can
 * never disagree.
 *
 * Cars live in a single engine-wide pool (RailwayEngine.cars) rather than as
 * fixed arrays inside each train. A pool keeps the memory layout flat and
 * deterministic (no allocation on the tick path), gives every car a globally
 * unique id for the event log, and lets a car be detached and re-attached
 * without moving structs around.
 *
 * SAFETY NOTICE
 * -------------
 * Simulator only. Not certified for real signalling. See railcontrol.h.
 * ========================================================================== */
#include "railway_internal.h"

#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Per-type defaults
 *
 * Real stock dimensions, rounded to a sensible metre. These are the values a
 * formation gets when the operator adds a car without specifying numbers.
 * -------------------------------------------------------------------------- */
typedef struct
{
    const char *designation;
    const char *code;
    float length_m;
    float tare_tonnes;
    int capacity; /* seats for passenger stock, tonnes for freight */
} CarTypeProfile;

static const CarTypeProfile k_profiles[] = {
    /* CAR_CLASS_LOCOMOTIVE   */ {"Traction unit", "LOCO", 20.0f, 82.0f, 0},
    /* CAR_CLASS_PASSENGER    */ {"Passenger saloon", "PSGR", 23.0f, 36.5f, 72},
    /* CAR_CLASS_FREIGHT_BOX  */ {"Covered box wagon", "BOX", 18.0f, 24.0f, 60},
    /* CAR_CLASS_FREIGHT_FLAT */ {"Flat wagon", "FLAT", 18.5f, 20.0f, 65},
    /* CAR_CLASS_TANK         */ {"Tank wagon", "TANK", 15.0f, 26.0f, 45},
    /* CAR_CLASS_HOPPER       */ {"Bulk hopper", "HOP", 16.0f, 24.5f, 70},
    /* CAR_CLASS_BRAKE_VAN    */ {"Brake van", "BV", 12.0f, 22.0f, 0},
    /* CAR_CLASS_DINING       */ {"Catering vehicle", "DIN", 23.0f, 38.0f, 42},
    /* CAR_CLASS_SLEEPER      */ {"Sleeping car", "SLP", 23.0f, 39.5f, 30},
    /* CAR_CLASS_ENGINEERING  */ {"Engineering vehicle", "ENG", 19.0f, 30.0f, 0},
};

#define CAR_PROFILE_COUNT ((int)(sizeof(k_profiles) / sizeof(k_profiles[0])))

/* Clamp a car type into the profile table. */
static const CarTypeProfile *profile_of(RcpCarType type)
{
    int index = (int)type;

    if (index < 0 || index >= CAR_PROFILE_COUNT)
    {
        return &k_profiles[CAR_CLASS_FREIGHT_BOX];
    }
    return &k_profiles[index];
}

const char *rw_car_default_designation(RcpCarType type)
{
    return profile_of(type)->designation;
}

float rw_car_default_length(RcpCarType type)
{
    return profile_of(type)->length_m;
}

float rw_car_default_tare(RcpCarType type)
{
    return profile_of(type)->tare_tonnes;
}

int rw_car_default_capacity(RcpCarType type)
{
    return profile_of(type)->capacity;
}

/* --------------------------------------------------------------------------
 * Labels - every front end renders cars the same way.
 * -------------------------------------------------------------------------- */
const char *rcp_car_type_code(RcpCarType type)
{
    return profile_of(type)->code;
}

const char *rcp_car_type_name(RcpCarType type)
{
    return profile_of(type)->designation;
}

const char *rcp_car_status_name(RcpCarStatus status)
{
    switch (status)
    {
    case CAR_STATUS_OK:
        return "OK";
    case CAR_STATUS_DEFECTIVE:
        return "DEFECTIVE";
    case CAR_STATUS_SHUNTING:
        return "SHUNTING";
    case CAR_STATUS_OUT_OF_SERVICE:
        return "OUT OF SERVICE";
    case CAR_STATUS_LOCKED:
        return "LOCKED";
    }
    return "UNKNOWN";
}

const char *rcp_car_coupling_name(RcpCarCoupling coupling)
{
    switch (coupling)
    {
    case CAR_COUPLING_INTERNAL:
        return "COUPLED";
    case CAR_COUPLING_LEAD:
        return "LEAD";
    case CAR_COUPLING_TAIL:
        return "TAIL";
    case CAR_COUPLING_LOOSE:
        return "LOOSE";
    }
    return "UNKNOWN";
}

/* --------------------------------------------------------------------------
 * Pool access
 * -------------------------------------------------------------------------- */
CarStateInternal *rw_car_by_id(RwId car_id)
{
    RailwayEngine *e = rw_engine();
    size_t i;

    if (e == NULL || car_id == RW_ID_NONE)
    {
        return NULL;
    }
    for (i = 0; i < e->car_count; ++i)
    {
        if (e->cars[i].id == car_id)
        {
            return &e->cars[i];
        }
    }
    return NULL;
}

RwId rw_car_find(const char *number)
{
    RailwayEngine *e = rw_engine();
    size_t i;

    if (e == NULL || number == NULL || number[0] == '\0')
    {
        return RW_ID_NONE;
    }
    for (i = 0; i < e->car_count; ++i)
    {
        if (strcmp(e->cars[i].number, number) == 0)
        {
            return e->cars[i].id;
        }
    }
    return RW_ID_NONE;
}

RwId rw_car_find_on_train(RwId train_id, int position)
{
    RailwayEngine *e = rw_engine();
    size_t i;

    if (e == NULL || position < 0)
    {
        return RW_ID_NONE;
    }
    for (i = 0; i < e->car_count; ++i)
    {
        if (e->cars[i].train == train_id && e->cars[i].position == position)
        {
            return e->cars[i].id;
        }
    }
    return RW_ID_NONE;
}

/* --------------------------------------------------------------------------
 * Formation management
 *
 * Coupling is derived from position, never set by hand: the head car is LEAD,
 * the last car is TAIL, and everything between is INTERNAL. A car with no
 * train is LOOSE. That keeps the labels consistent with the ordering.
 * -------------------------------------------------------------------------- */
static void assign_position(CarStateInternal *car, RwId train_id, int position,
                            bool is_head, bool is_tail)
{
    car->train = train_id;
    car->position = position;

    if (train_id == RW_ID_NONE)
    {
        car->coupling = CAR_COUPLING_LOOSE;
    }
    else if (is_head && is_tail)
    {
        car->coupling = CAR_COUPLING_LEAD; /* single-car unit */
    }
    else if (is_head)
    {
        car->coupling = CAR_COUPLING_LEAD;
    }
    else if (is_tail)
    {
        car->coupling = CAR_COUPLING_TAIL;
    }
    else
    {
        car->coupling = CAR_COUPLING_INTERNAL;
    }
}

/**
 * Re-number a train's cars so positions run 0..n-1 in pool order, and refresh
 * the LEAD/TAIL/INTERNAL labels. Called after any add, remove or detach.
 */
static void renumber_train(RwId train_id)
{
    RailwayEngine *e = rw_engine();
    size_t i;
    int position = 0;
    int count = 0;

    if (e == NULL || train_id == RW_ID_NONE)
    {
        return;
    }

    for (i = 0; i < e->car_count; ++i)
    {
        if (e->cars[i].train == train_id)
        {
            count++;
        }
    }

    for (i = 0; i < e->car_count; ++i)
    {
        CarStateInternal *car = &e->cars[i];

        if (car->train != train_id)
        {
            continue;
        }
        assign_position(car, train_id, position,
                        position == 0, position == count - 1);
        position++;
    }
}

float rw_train_formation_length(RwId train_id)
{
    RailwayEngine *e = rw_engine();
    size_t i;
    float total = 0.0f;

    if (e == NULL || train_id == RW_ID_NONE)
    {
        return 0.0f;
    }
    for (i = 0; i < e->car_count; ++i)
    {
        if (e->cars[i].train == train_id)
        {
            total += e->cars[i].length_m;
        }
    }
    return total;
}

/**
 * Recompute a train's derived fields from its formation:
 *   length_m  - the sum of the car lengths, plus a coupling allowance
 *   carriages - the number of cars
 * The controller reads length_m for section occupancy, so this is the one
 * place those two views are brought back into agreement.
 */
RwResult rw_train_recompute_length(RwId train_id)
{
    RailwayEngine *e = rw_engine();
    size_t i;
    int cars = 0;
    float sum = 0.0f;
    TrainStateInternal *train = NULL;

    if (e == NULL || train_id == RW_ID_NONE || train_id > e->train_count)
    {
        return RW_ERR_INVALID_ID;
    }
    train = &e->trains[train_id - 1];

    for (i = 0; i < e->car_count; ++i)
    {
        if (e->cars[i].train == train_id)
        {
            cars++;
            sum += e->cars[i].length_m;
        }
    }

    train->carriages = cars;

    if (cars > 0)
    {
        /* 1 m per coupling between vehicles, plus 2 m of drawgear at each end. */
        const float couplers = (float)(cars - 1) * 1.0f;
        const unsigned derived = (unsigned)(sum + couplers + 4.0f);
        train->length_m = derived;
    }
    else
    {
        /* A train with no formation keeps a nominal 20 m so occupancy maths
         * still works; this is the pre-formation default. */
        train->length_m = 20u;
    }
    return RW_OK;
}

int rw_cars_of_train_count(RwId train_id)
{
    RailwayEngine *e = rw_engine();
    size_t i;
    int count = 0;

    if (e == NULL)
    {
        return 0;
    }
    for (i = 0; i < e->car_count; ++i)
    {
        if (e->cars[i].train == train_id && train_id != RW_ID_NONE)
        {
            count++;
        }
    }
    return count;
}

/* --------------------------------------------------------------------------
 * Add / remove
 * -------------------------------------------------------------------------- */
RwId rw_car_add(RwId train_id, RcpCarType type, const char *number,
                const char *designation, float length_m,
                float tare_tonnes, int capacity)
{
    RailwayEngine *e = rw_engine();
    CarStateInternal *car;
    int position;

    if (e == NULL)
    {
        return RW_ID_NONE;
    }
    if (e->car_count >= RW_MAX_CARS)
    {
        rw_event(SEVERITY_WARNING, "STOCK",
                 "Cannot add car - the rolling stock pool is full (%d cars)",
                 RW_MAX_CARS);
        return RW_ID_NONE;
    }
    if (train_id != RW_ID_NONE && train_id > e->train_count)
    {
        return RW_ID_NONE;
    }

    position = (train_id == RW_ID_NONE) ? 0 : rw_cars_of_train_count(train_id);
    if (position >= RW_MAX_CARS_PER_TRAIN)
    {
        rw_event(SEVERITY_WARNING, "STOCK",
                 "Cannot add car - train %u already has the maximum %d vehicles",
                 (unsigned)train_id, RW_MAX_CARS_PER_TRAIN);
        return RW_ID_NONE;
    }

    car = &e->cars[e->car_count++];
    memset(car, 0, sizeof(*car));

    car->id = (e->next_car_id == RW_ID_NONE) ? 1u : e->next_car_id++;
    car->type = type;
    car->status = CAR_STATUS_OK;
    car->in_service = true;
    car->length_m = (length_m > 0.0f) ? length_m : rw_car_default_length(type);
    car->tare_tonnes = (tare_tonnes > 0.0f) ? tare_tonnes : rw_car_default_tare(type);
    car->capacity = (capacity >= 0) ? capacity : rw_car_default_capacity(type);
    car->load_percent = 0.0f;
    car->occupied = false;

    if (number != NULL && number[0] != '\0')
    {
        snprintf(car->number, sizeof(car->number), "%s", number);
    }
    else
    {
        snprintf(car->number, sizeof(car->number), "%05u", (unsigned)car->id);
    }
    if (designation != NULL && designation[0] != '\0')
    {
        snprintf(car->designation, sizeof(car->designation), "%s", designation);
    }
    else
    {
        snprintf(car->designation, sizeof(car->designation), "%s",
                 rw_car_default_designation(type));
    }

    assign_position(car, train_id, position, position == 0, false);

    if (train_id != RW_ID_NONE)
    {
        renumber_train(train_id);
        rw_train_recompute_length(train_id);
        rw_bump_revision();
    }
    return car->id;
}

RwResult rw_car_remove(RwId car_id)
{
    RailwayEngine *e = rw_engine();
    CarStateInternal *car = rw_car_by_id(car_id);
    size_t index;
    RwId train_id;

    if (e == NULL)
    {
        return RW_ERR_NOT_INITIALIZED;
    }
    if (car == NULL)
    {
        return RW_ERR_INVALID_ID;
    }

    train_id = car->train;
    index = (size_t)(car - e->cars);

    /* Compact the pool so the array stays contiguous. */
    if (index + 1 < e->car_count)
    {
        memmove(&e->cars[index], &e->cars[index + 1],
                (e->car_count - index - 1) * sizeof(e->cars[0]));
    }
    e->car_count--;

    if (train_id != RW_ID_NONE)
    {
        renumber_train(train_id);
        rw_train_recompute_length(train_id);
        rw_bump_revision();
    }
    return RW_OK;
}

/* --------------------------------------------------------------------------
 * Read-out
 * -------------------------------------------------------------------------- */
static void to_info(const CarStateInternal *car, RwCarInfo *info)
{
    memset(info, 0, sizeof(*info));
    info->id = car->id;
    info->train = car->train;
    info->position = car->position;
    info->type = car->type;
    info->status = car->status;
    info->coupling = car->coupling;
    info->length_m = car->length_m;
    info->tare_tonnes = car->tare_tonnes;
    info->capacity = car->capacity;
    info->load_percent = car->load_percent;
    info->occupied = car->occupied;
    info->in_service = car->in_service;
    snprintf(info->number, sizeof(info->number), "%s", car->number);
    snprintf(info->designation, sizeof(info->designation), "%s", car->designation);
}

int rw_cars_of_train(RwId train_id, RwCarInfo *out, int max)
{
    RailwayEngine *e = rw_engine();
    size_t i;
    int written = 0;

    if (e == NULL || out == NULL || max <= 0 || train_id == RW_ID_NONE)
    {
        return 0;
    }
    for (i = 0; i < e->car_count && written < max; ++i)
    {
        if (e->cars[i].train == train_id)
        {
            to_info(&e->cars[i], &out[written]);
            written++;
        }
    }
    return written;
}

/* --------------------------------------------------------------------------
 * Init
 * -------------------------------------------------------------------------- */
void rw_cars_init(void)
{
    RailwayEngine *e = rw_engine();

    if (e == NULL)
    {
        return;
    }
    memset(e->cars, 0, sizeof(e->cars));
    e->car_count = 0;
    e->next_car_id = 1;
}

/* Exposed for the API layer, which needs the same conversion. */
void rw_car_to_info(RwId car_id, RwCarInfo *info)
{
    CarStateInternal *car = rw_car_by_id(car_id);

    if (car == NULL || info == NULL)
    {
        return;
    }
    to_info(car, info);
}
