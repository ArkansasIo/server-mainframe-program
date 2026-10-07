/* ==========================================================================
 * rcp_layout.c - The demonstration railway.
 *
 * Scope: a two-platform station with a goods loop and two sidings, on a
 * double-track section. It is small enough to reason about but contains every
 * element the interlocking must handle: a flat crossing, a trailing and a
 * facing point, permissive and non-permissive sections, and a level crossing.
 *
 *                      UP MAIN (eastbound)
 *     west  ── T1 ── P1 ──┬── T2 (UP PLATFORM 1) ──┬── T4 ── P3 ── T7 ──  east
 *                          │                        │
 *                          └── T3 (UP PLATFORM 2) ──┘
 *
 *                      DOWN MAIN (westbound)
 *         west ── T8 ── P4 ── T9 (DOWN PLATFORM) ── T10 ── T11 ── east
 *                            │
 *                            └── T12 (GOODS LOOP) ── P5 ── T13 (SIDING 1)
 *                                                       └──── T14 (SIDING 2)
 *
 * Geographic coordinates are normalised 0..1 for the diagram renderer; the
 * GUI scales them into its drawing area. North (latitude) increases upwards
 * on the panel, so a larger latitude draws higher.
 * ========================================================================== */
#include "railway_internal.h"

#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Small builders that keep rw_layout_build_default() readable.
 * -------------------------------------------------------------------------- */
static TrackState *add_track(RailwayEngine *e, const char *name,
                             float lat0, float lon0, float lat1, float lon1,
                             int speed_limit, float length_m)
{
    TrackState *track;

    if (e->track_count >= RW_MAX_TRACKS)
    {
        return NULL;
    }
    track = &e->tracks[e->track_count++];
    memset(track, 0, sizeof(*track));
    track->id = (RwId)e->track_count;
    snprintf(track->name, sizeof(track->name), "%s", name);
    track->start_lat = lat0;
    track->start_lon = lon0;
    track->end_lat = lat1;
    track->end_lon = lon1;
    track->speed_limit_kph = speed_limit;
    track->length_m = length_m;
    track->occupancy = TRACK_CLEAR;
    track->in_service = true;
    return track;
}

static SignalStateInternal *add_signal(RailwayEngine *e, const char *name, RwId track,
                                       bool at_exit)
{
    SignalStateInternal *signal;

    if (e->signal_count >= RW_MAX_SIGNALS)
    {
        return NULL;
    }
    signal = &e->signals[e->signal_count++];
    memset(signal, 0, sizeof(*signal));
    signal->id = (RwId)e->signal_count;
    snprintf(signal->name, sizeof(signal->name), "%s", name);
    signal->track = track;
    signal->at_exit = at_exit;
    signal->aspect = SIGNAL_RED;
    signal->commanded = SIGNAL_RED;
    signal->in_service = true;
    signal->manual = false;
    return signal;
}

static PointState *add_point(RailwayEngine *e, const char *name,
                             RwId upstream, RwId normal, RwId reverse,
                             float lat, float lon)
{
    PointState *point;

    if (e->point_count >= RW_MAX_SWITCHES)
    {
        return NULL;
    }
    point = &e->points[e->point_count++];
    memset(point, 0, sizeof(*point));
    point->id = (RwId)e->point_count;
    snprintf(point->name, sizeof(point->name), "%s", name);
    point->track_upstream = upstream;
    point->track_normal = normal;
    point->track_reverse = reverse;
    point->position = SWITCH_MAIN;
    point->commanded = SWITCH_MAIN;
    point->in_service = true;
    point->latitude = lat;
    point->longitude = lon;
    return point;
}

static SensorState *add_sensor(RailwayEngine *e, const char *name, RwSensorKind kind, RwId track)
{
    SensorState *sensor;

    if (e->sensor_count >= RW_MAX_SENSORS)
    {
        return NULL;
    }
    sensor = &e->sensors[e->sensor_count++];
    memset(sensor, 0, sizeof(*sensor));
    sensor->id = (RwId)e->sensor_count;
    snprintf(sensor->name, sizeof(sensor->name), "%s", name);
    sensor->kind = kind;
    sensor->state = SENSOR_OK;
    sensor->track = track;
    sensor->in_service = true;
    return sensor;
}

static TrainStateInternal *add_train(RailwayEngine *e, const char *headcode,
                                     const char *description, TrainClass cls,
                                     RwId track, float max_speed, int carriages)
{
    TrainStateInternal *train;

    if (e->train_count >= RW_MAX_TRAINS)
    {
        return NULL;
    }
    train = &e->trains[e->train_count++];
    memset(train, 0, sizeof(*train));
    train->id = (RwId)e->train_count;
    snprintf(train->headcode, sizeof(train->headcode), "%s", headcode);
    snprintf(train->description, sizeof(train->description), "%s", description);
    train->cls = cls;
    train->state = TRAIN_STOPPED;
    train->track = track;
    train->max_speed_kph = max_speed;
    train->target_speed_kph = 0.0f;
    train->speed_kph = 0.0f;
    train->carriages = carriages;
    train->length_m = (unsigned)(carriages * 23 + 20);
    return train;
}

static RouteStateInternal *add_route(RailwayEngine *e, const char *name,
                                     RwId entry_signal, RwId exit_signal)
{
    RouteStateInternal *route;

    if (e->route_count >= RW_MAX_ROUTES)
    {
        return NULL;
    }
    route = &e->routes[e->route_count++];
    memset(route, 0, sizeof(*route));
    route->id = (RwId)e->route_count;
    snprintf(route->name, sizeof(route->name), "%s", name);
    route->entry_signal = entry_signal;
    route->exit_signal = exit_signal;
    route->state = ROUTE_IDLE;
    return route;
}

static void route_step(RouteStateInternal *route, RwId track, RwId point, SwitchPosition required)
{
    if (route->step_count >= RW_MAX_STEPS)
    {
        return;
    }
    route->steps[route->step_count].track = track;
    route->steps[route->step_count].point = point;
    route->steps[route->step_count].point_required = required;
    route->step_count++;
}

/* --------------------------------------------------------------------------
 * Rolling stock builders
 *
 * Every train in the demonstration fleet gets its known formation: a traction
 * unit at the head, the vehicles behind it, and (on the freight) a brake van
 * at the tail. This is the data the operator sees on the train detail panel,
 * and it is what determines each train's length.
 * -------------------------------------------------------------------------- */

/* Append a single car to a train, letting rcp_car.c supply the per-type
 * defaults for anything not given here. */
static void add_car(RailwayEngine *e, RwId train_id, RcpCarType type,
                    const char *number, const char *designation)
{
    (void)e;
    rw_car_add(train_id, type, number, designation, 0.0f, 0.0f, -1);
}

/* Give a car a load, so the fleet is not uniformly empty. */
static void load_car(const char *number, float percent, bool occupied)
{
    RwId car_id = rw_car_find(number);
    CarStateInternal *car = rw_car_by_id(car_id);

    if (car != NULL)
    {
        car->load_percent = percent;
        car->occupied = occupied;
    }
}

/*
 * The known formations.
 *
 *   1A34  Up express       - 1 loco + 7 passenger + 1 dining  (9 vehicles)
 *   2C58  Down stopping    - 1 loco + 5 passenger             (6 vehicles)
 *   4E71  Coal empties     - 2 loco (banking) + 14 hopper + brake van
 *   6T22  Track machine    - 1 loco + 2 engineering vehicles
 *   0Z99  Light engine     - 1 loco only
 */
static void build_fleet_cars(RailwayEngine *e)
{
    RwId train;

    if (e->train_count < 5)
    {
        return;
    }

    /* --- 1A34: the up express, a 9-car passenger formation. --------------- */
    train = 1;
    add_car(e, train, CAR_CLASS_LOCOMOTIVE,  "43021", "Class 43 power car");
    add_car(e, train, CAR_CLASS_PASSENGER,   "41102", "Trailer first");
    add_car(e, train, CAR_CLASS_PASSENGER,   "41103", "Trailer first");
    add_car(e, train, CAR_CLASS_DINING,      "40701", "Buffet restaurant");
    add_car(e, train, CAR_CLASS_PASSENGER,   "42110", "Trailer standard");
    add_car(e, train, CAR_CLASS_PASSENGER,   "42111", "Trailer standard");
    add_car(e, train, CAR_CLASS_PASSENGER,   "42112", "Trailer standard");
    add_car(e, train, CAR_CLASS_PASSENGER,   "42113", "Trailer standard");
    add_car(e, train, CAR_CLASS_LOCOMOTIVE,  "43022", "Class 43 power car (rear)");
    load_car("41102", 62.0f, true);
    load_car("41103", 48.0f, true);
    load_car("40701", 35.0f, true);
    load_car("42110", 71.0f, true);
    load_car("42111", 66.0f, true);
    load_car("42112", 54.0f, true);
    load_car("42113", 29.0f, true);

    /* --- 2C58: the down stopping service, 6 cars. ------------------------- */
    train = 2;
    add_car(e, train, CAR_CLASS_LOCOMOTIVE,  "15801", "Class 158 power car");
    add_car(e, train, CAR_CLASS_PASSENGER,   "15811", "Trailer standard");
    add_car(e, train, CAR_CLASS_PASSENGER,   "15812", "Trailer standard");
    add_car(e, train, CAR_CLASS_PASSENGER,   "15813", "Trailer standard");
    add_car(e, train, CAR_CLASS_PASSENGER,   "15814", "Trailer standard");
    add_car(e, train, CAR_CLASS_LOCOMOTIVE,  "15802", "Class 158 power car (rear)");
    load_car("15811", 44.0f, true);
    load_car("15812", 58.0f, true);
    load_car("15813", 33.0f, true);
    load_car("15814", 19.0f, true);

    /* --- 4E71: coal empties - running empty back to the colliery. -------- */
    train = 3;
    add_car(e, train, CAR_CLASS_LOCOMOTIVE,  "66018", "Class 66 traction unit");
    add_car(e, train, CAR_CLASS_LOCOMOTIVE,  "66019", "Class 66 banking unit");
    add_car(e, train, CAR_CLASS_HOPPER,      "HOP3101", "HAA coal hopper");
    add_car(e, train, CAR_CLASS_HOPPER,      "HOP3102", "HAA coal hopper");
    add_car(e, train, CAR_CLASS_HOPPER,      "HOP3103", "HAA coal hopper");
    add_car(e, train, CAR_CLASS_HOPPER,      "HOP3104", "HAA coal hopper");
    add_car(e, train, CAR_CLASS_HOPPER,      "HOP3105", "HAA coal hopper");
    add_car(e, train, CAR_CLASS_HOPPER,      "HOP3106", "HAA coal hopper");
    add_car(e, train, CAR_CLASS_HOPPER,      "HOP3107", "HAA coal hopper");
    add_car(e, train, CAR_CLASS_HOPPER,      "HOP3108", "HAA coal hopper");
    add_car(e, train, CAR_CLASS_HOPPER,      "HOP3109", "HAA coal hopper");
    add_car(e, train, CAR_CLASS_HOPPER,      "HOP3110", "HAA coal hopper");
    add_car(e, train, CAR_CLASS_HOPPER,      "HOP3111", "HAA coal hopper");
    add_car(e, train, CAR_CLASS_HOPPER,      "HOP3112", "HAA coal hopper");
    add_car(e, train, CAR_CLASS_HOPPER,      "HOP3113", "HAA coal hopper");
    add_car(e, train, CAR_CLASS_HOPPER,      "HOP3114", "HAA coal hopper");
    add_car(e, train, CAR_CLASS_BRAKE_VAN,   "BV9001", "Guards brake van");

    /* --- 6T22: track machine on the siding. ------------------------------- */
    train = 4;
    add_car(e, train, CAR_CLASS_LOCOMOTIVE,  "31601", "Class 31 traction unit");
    add_car(e, train, CAR_CLASS_ENGINEERING, "DR77001", "Ballast tamper");
    add_car(e, train, CAR_CLASS_ENGINEERING, "DR77002", "Stoneblower");

    /* --- 0Z99: light engine, a single traction unit. ---------------------- */
    train = 5;
    add_car(e, train, CAR_CLASS_LOCOMOTIVE,  "57009", "Class 57 light engine");

    /* A spare vehicle in the yard, not attached to any train. */
    add_car(e, RW_ID_NONE, CAR_CLASS_FREIGHT_FLAT, "FLT4401", "Spare flat wagon");

    /* Recompute every train's derived length from the formations just built. */
    {
        size_t t;
        for (t = 0; t < e->train_count; ++t)
        {
            rw_train_recompute_length(e->trains[t].id);
        }
    }
}

/* --------------------------------------------------------------------------
 * Track ids, named for readability below. These match the order in which the
 * tracks are added.
 * -------------------------------------------------------------------------- */
enum
{
    TRK_UP_WEST = 1,  /* T1  up main, west of the station */
    TRK_UP_P1,        /* T2  up platform 1 */
    TRK_UP_P2,        /* T3  up platform 2 */
    TRK_UP_EAST,      /* T4  up main, east of the station */
    TRK_UP_EAST2,     /* T5  up main, eastern approach */
    TRK_SIDING_A,     /* T6  up siding */
    TRK_SIDING_B,     /* T7  up siding, second road */
    TRK_DOWN_WEST,    /* T8  down main, west */
    TRK_DOWN_PLAT,    /* T9  down platform */
    TRK_DOWN_EAST,    /* T10 down main, east */
    TRK_DOWN_EAST2,   /* T11 down main, eastern approach */
    TRK_GOODS_LOOP,   /* T12 goods loop */
    TRK_GOODS_SIDING, /* T13 goods siding 1 */
    TRK_GOODS_SIDING2 /* T14 goods siding 2 */
};

RwResult rw_layout_build_default(RailwayEngine *e)
{
    PointState *point;
    RouteStateInternal *route;

    if (e == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }

    /* The rolling stock pool is engine-wide; start it empty before any
     * formation is built. */
    rw_cars_init();

    /* -------- UP MAIN (eastbound, top half of the panel) ---------------- */
    add_track(e, "T1 UP MAIN W", 0.72f, 0.02f, 0.72f, 0.22f, 100, 480.0f);
    add_track(e, "T2 UP PLAT 1", 0.72f, 0.22f, 0.72f, 0.48f, 80, 520.0f);
    add_track(e, "T3 UP PLAT 2", 0.62f, 0.22f, 0.62f, 0.48f, 80, 520.0f);
    add_track(e, "T4 UP MAIN E", 0.72f, 0.48f, 0.72f, 0.72f, 100, 480.0f);
    add_track(e, "T5 UP APPROACH", 0.72f, 0.72f, 0.72f, 0.95f, 100, 620.0f);
    add_track(e, "T6 UP SDG A", 0.84f, 0.48f, 0.84f, 0.70f, 20, 240.0f);
    add_track(e, "T7 UP SDG B", 0.90f, 0.48f, 0.90f, 0.70f, 20, 240.0f);

    /* -------- DOWN MAIN (westbound, bottom half) ------------------------ */
    add_track(e, "T8 DN MAIN W", 0.28f, 0.05f, 0.28f, 0.30f, 100, 480.0f);
    add_track(e, "T9 DN PLAT", 0.28f, 0.30f, 0.28f, 0.58f, 80, 520.0f);
    add_track(e, "T10 DN MAIN E", 0.28f, 0.58f, 0.28f, 0.78f, 100, 480.0f);
    add_track(e, "T11 DN APPROACH", 0.28f, 0.78f, 0.28f, 0.98f, 100, 620.0f);
    add_track(e, "T12 GOODS LOOP", 0.14f, 0.30f, 0.14f, 0.62f, 40, 460.0f);
    add_track(e, "T13 GD SDG 1", 0.06f, 0.62f, 0.06f, 0.86f, 15, 260.0f);
    add_track(e, "T14 GD SDG 2", 0.00f, 0.62f, 0.00f, 0.86f, 15, 260.0f);

    /* -------- POINTS ---------------------------------------------------- */
    /* P1: facing point at the west end of the up platforms. Normal runs into
     * platform 1, reverse into platform 2. */
    point = add_point(e, "P1", TRK_UP_WEST, TRK_UP_P1, TRK_UP_P2, 0.72f, 0.22f);
    if (point != NULL) {
        point->lock_type = 0;   /* triggered - locks when a route is set */
    }

    /* P2: trailing point at the east end of platform 2, rejoining the main. */
    point = add_point(e, "P2", TRK_UP_P2, TRK_UP_EAST, TRK_UP_P2, 0.62f, 0.48f);

    /* P3: facing point off the up main into the up sidings. */
    point = add_point(e, "P3", TRK_UP_EAST, TRK_UP_EAST2, TRK_SIDING_A, 0.72f, 0.72f);

    /* P4: facing point off the down main into the goods loop. */
    point = add_point(e, "P4", TRK_DOWN_WEST, TRK_DOWN_PLAT, TRK_GOODS_LOOP, 0.28f, 0.30f);

    /* P5: trailing point in the goods yard splitting the two sidings. */
    point = add_point(e, "P5", TRK_GOODS_LOOP, TRK_GOODS_SIDING, TRK_GOODS_SIDING2, 0.10f, 0.62f);

    /* -------- SIGNALS ---------------------------------------------------
     * Each signal is bound to the track it stands at; add_signal() returns the
     * new signal so the binding could be extended here if a layout needed it. */
    (void)add_signal(e, "S1", TRK_UP_WEST, true);        /* up home */
    (void)add_signal(e, "S3", TRK_UP_P1, true);          /* platform 1 starter */
    (void)add_signal(e, "S4", TRK_UP_P2, true);          /* platform 2 starter */
    (void)add_signal(e, "S6", TRK_UP_EAST, true);        /* up section signal */
    (void)add_signal(e, "S8", TRK_UP_EAST2, true);       /* up distant */

    /* Down direction. */
    (void)add_signal(e, "S10", TRK_DOWN_EAST, true);     /* down home */
    (void)add_signal(e, "S12", TRK_DOWN_PLAT, true);     /* down platform starter */
    (void)add_signal(e, "S14", TRK_DOWN_WEST, true);     /* down section signal */
    (void)add_signal(e, "S16", TRK_GOODS_LOOP, true);    /* goods loop starter */

    /* Signals reading into the station (entry signals of the routes below). */
    (void)add_signal(e, "S2", TRK_UP_WEST, false);       /* up inner home */
    (void)add_signal(e, "S11", TRK_DOWN_EAST, false);    /* down inner home */

    /* -------- SENSORS: one track circuit per section, plus extras -------- */
    {
        size_t i;
        static const char *tc_names[] = {
            "TC1", "TC2", "TC3", "TC4", "TC5", "TC6", "TC7",
            "TC8", "TC9", "TC10", "TC11", "TC12", "TC13", "TC14"};
        for (i = 0; i < e->track_count && i < 14; ++i)
        {
            add_sensor(e, tc_names[i], SENSOR_TRACK_CIRCUIT, (RwId)(i + 1));
        }
    }
    /* Axle counter at the station throat, a hot box detector on the up main,
     * a level crossing on the down approach, and a treadle in the goods yard. */
    add_sensor(e, "AC1", SENSOR_AXLE_COUNTER, TRK_UP_WEST);
    add_sensor(e, "HB1", SENSOR_HOT_BOX, TRK_UP_EAST2);
    add_sensor(e, "LX1", SENSOR_LEVEL_CROSSING, TRK_DOWN_EAST2 > 0 ? TRK_DOWN_EAST2 : TRK_DOWN_EAST);
    add_sensor(e, "TR1", SENSOR_TREADLE, TRK_GOODS_LOOP);

    /* -------- TRAINS: a representative mix ------------------------------ */
    add_train(e, "1A34", "Up express to Skeffield", TRAIN_CLASS_PASSENGER, TRK_UP_WEST, 160.0f, 8);
    add_train(e, "2C58", "Down stopping service", TRAIN_CLASS_PASSENGER, TRK_DOWN_EAST, 145.0f, 6);
    add_train(e, "4E71", "Up coal empties", TRAIN_CLASS_FREIGHT, TRK_GOODS_LOOP, 100.0f, 18);
    add_train(e, "6T22", "Track machine", TRAIN_CLASS_ENGINEERING, TRK_SIDING_B, 60.0f, 3);
    add_train(e, "0Z99", "Light engine to yard", TRAIN_CLASS_LIGHT_ENGINE, TRK_DOWN_WEST, 120.0f, 0);

    /* Give a couple of trains positions along their tracks so the diagram
     * shows them in different places. */
    if (e->train_count >= 5)
    {
        e->trains[0].position_m = 120.0f;
        e->trains[1].position_m = 60.0f;
        e->trains[2].position_m = 200.0f;
        e->trains[3].position_m = 40.0f;
        e->trains[4].position_m = 300.0f;
    }

    /* -------- ROLLING STOCK: give every train its known formation --------- */
    build_fleet_cars(e);

    /* -------- ROUTES ----------------------------------------------------
     * Each route names its entry and exit signal and lists the tracks and
     * the required point lies. The interlocking proves all of this before
     * granting the route.
     * ------------------------------------------------------------------- */

    /* R1: up main into platform 1 and on to the section. */
    route = add_route(e, "R1 UP-P1-EAST", 1 /*S1*/, 3 /*S3*/);
    if (route != NULL)
    {
        route_step(route, TRK_UP_WEST, RW_ID_NONE, SWITCH_MAIN);
        route_step(route, TRK_UP_P1, 1 /*P1 requires NORMAL*/, SWITCH_MAIN);
    }

    /* R2: up main into platform 2 (reverse P1). */
    route = add_route(e, "R2 UP-P2", 1, 4 /*S4*/);
    if (route != NULL)
    {
        route_step(route, TRK_UP_WEST, RW_ID_NONE, SWITCH_MAIN);
        route_step(route, TRK_UP_P1, RW_ID_NONE, SWITCH_MAIN); /* not used */
        route_step(route, TRK_UP_P2, 1, SWITCH_DIVERGING);
    }

    /* R3: platform 1 to the east section over the trailing point P2. */
    route = add_route(e, "R3 P1-EAST", 3 /*S3*/, 5 /*S6*/);
    if (route != NULL)
    {
        route_step(route, TRK_UP_P1, RW_ID_NONE, SWITCH_MAIN);
        route_step(route, TRK_UP_EAST, 2 /*P2*/, SWITCH_MAIN);
    }

    /* R4: platform 2 to the east section trailing through P2. */
    route = add_route(e, "R4 P2-EAST", 4 /*S4*/, 5 /*S6*/);
    if (route != NULL)
    {
        route_step(route, TRK_UP_P2, RW_ID_NONE, SWITCH_MAIN);
        route_step(route, TRK_UP_EAST, 2, SWITCH_MAIN);
    }

    /* R5: up main into the sidings (reverse P3). */
    route = add_route(e, "R5 UP-SDGS", 5 /*S6*/, 10 /*S2*/);
    if (route != NULL)
    {
        route_step(route, TRK_UP_EAST, RW_ID_NONE, SWITCH_MAIN);
        route_step(route, TRK_SIDING_A, 3 /*P3*/, SWITCH_DIVERGING);
    }

    /* R6: down main into the down platform. */
    route = add_route(e, "R6 DN-PLAT", 6 /*S10*/, 7 /*S12*/);
    if (route != NULL)
    {
        route_step(route, TRK_DOWN_EAST, RW_ID_NONE, SWITCH_MAIN);
        route_step(route, TRK_DOWN_PLAT, 4 /*P4 - trailing from this direction*/, SWITCH_MAIN);
    }

    /* R7: down main into the goods loop (reverse P4). */
    route = add_route(e, "R7 DN-GOODS", 6, 9 /*S16*/);
    if (route != NULL)
    {
        route_step(route, TRK_DOWN_EAST, RW_ID_NONE, SWITCH_MAIN);
        route_step(route, TRK_GOODS_LOOP, 4, SWITCH_DIVERGING);
    }

    /* R8: down platform to the west section. */
    route = add_route(e, "R8 PLAT-WEST", 7 /*S12*/, 8 /*S14*/);
    if (route != NULL)
    {
        route_step(route, TRK_DOWN_PLAT, RW_ID_NONE, SWITCH_MAIN);
        route_step(route, TRK_DOWN_WEST, RW_ID_NONE, SWITCH_MAIN);
    }

    /* R9: goods loop onto the two sidings (reverse P5). */
    route = add_route(e, "R9 GOODS-SDG", 9 /*S16*/, 11 /*from P5*/);
    if (route != NULL)
    {
        route_step(route, TRK_GOODS_LOOP, RW_ID_NONE, SWITCH_MAIN);
        route_step(route, TRK_GOODS_SIDING, 5 /*P5*/, SWITCH_DIVERGING);
    }

    /* R10: down main straight through to the west (the classic conflicting
     * route against R7, both use T10 and P4). */
    route = add_route(e, "R10 DN-THROUGH", 6 /*S10*/, 8 /*S14*/);
    if (route != NULL)
    {
        route_step(route, TRK_DOWN_EAST, RW_ID_NONE, SWITCH_MAIN);
        route_step(route, TRK_DOWN_PLAT, 4, SWITCH_MAIN);
        route_step(route, TRK_DOWN_WEST, RW_ID_NONE, SWITCH_MAIN);
    }

    return RW_OK;
}
