/* ==========================================================================
 * railway_api.c - Implementation of the public control API.
 *
 * This file owns:
 *   - the single engine instance
 *   - the global mutex that makes every API call thread safe
 *   - the mapping between the public Rw*Info views and the internal state
 *   - the tick loop that advances signals, points, trains, routes, sensors
 *     and the CONJOB registry in a fixed, auditable order
 *
 * Order inside one tick matters and is deliberate:
 *
 *   1. points    - they must be settled before a route can be proved
 *   2. routes    - the interlocking releases cleared routes and re-proves
 *   3. signals   - aspects follow from the routes just set
 *   4. trains    - movement uses the aspects from step 3
 *   5. sensors   - field inputs update the track circuits
 *   6. occupancy - recomputed from train positions and circuits
 *   7. jobs      - CONJOB rules react to the settled state
 *   8. scripts   - Lua timers run last, against a fully consistent world
 * ========================================================================== */
#include "railway_api.h"
#include "railway_internal.h"

#include "signal_controller.h"
#include "switch_controller.h"
#include "train_controller.h"
#include "interlocking.h"
#include "conjob.h"
#if defined(MF_WITH_LUA)
#include "lua_script.h"
#endif

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

/* ==========================================================================
 * Engine instance and locking
 * ========================================================================== */
static RailwayEngine g_engine;

#if defined(_WIN32)
static CRITICAL_SECTION g_lock;
static bool g_lock_ready = false;

static void lock_init(void)
{
    if (!g_lock_ready)
    {
        InitializeCriticalSection(&g_lock);
        g_lock_ready = true;
    }
}
static void lock_enter(void)
{
    if (g_lock_ready)
    {
        EnterCriticalSection(&g_lock);
    }
}
static void lock_leave(void)
{
    if (g_lock_ready)
    {
        LeaveCriticalSection(&g_lock);
    }
}
#else
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void lock_init(void) {}
static void lock_enter(void) { pthread_mutex_lock(&g_lock); }
static void lock_leave(void) { pthread_mutex_unlock(&g_lock); }
#endif

RailwayEngine *rw_engine(void)
{
    return &g_engine;
}

bool rw_engine_ready(void)
{
    return g_engine.initialized;
}

void rw_set_error(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(g_engine.last_error, sizeof(g_engine.last_error), format, args);
    va_end(args);
}

void rw_bump_revision(void)
{
    g_engine.revision++;
}

/* ==========================================================================
 * Lifecycle
 * ========================================================================== */
RwResult railway_init(void)
{
    lock_init();
    lock_enter();

    if (g_engine.initialized)
    {
        lock_leave();
        return RW_OK;
    }

    memset(&g_engine, 0, sizeof(g_engine));
    g_engine.system_state = SYSTEM_INITIALISING;
    snprintf(g_engine.name, sizeof(g_engine.name), "%s", "SKEEFFIELD CONTROL AREA");
    snprintf(g_engine.signal_box, sizeof(g_engine.signal_box), "%s", "SKEFFIELD PSB");
    g_engine.next_event_id = 1;
    g_engine.initialized = true;

    rw_event_log_init();

    /* Configuration defaults. Fail safe everywhere: an unknown occupancy
     * blocks the route, and a route is required before a signal clears. */
    g_engine.config.fail_safe_unknown = true;
    g_engine.config.allow_call_on = false;
    g_engine.config.release_requires_clear = true;
    g_engine.config.require_route_lock = true;
    g_engine.config.point_movement_ms = 1200;
    g_engine.config.default_speed_limit_kph = 80.0f;

    /* Build the demonstration layout, then bring up the subsystems. */
    if (rw_layout_build_default(&g_engine) != RW_OK)
    {
        g_engine.initialized = false;
        lock_leave();
        return RW_ERR_INTERNAL;
    }

    rw_tracks_init();
    signal_controller_init();
    switch_controller_init();
    train_controller_init();
    rw_sensors_init();
    rw_interlock_init();
    conjob_init();
    conjob_install_defaults();

#if defined(MF_WITH_LUA)
    (void)lua_script_init();
#endif

    g_engine.system_state = SYSTEM_ONLINE;
    rw_event(SEVERITY_INFO, "SYSTEM", "Railway Control Center initialised: %s, %s",
             g_engine.name, g_engine.signal_box);
    rw_bump_revision();

    lock_leave();
    return RW_OK;
}

void railway_shutdown(void)
{
    lock_enter();

#if defined(MF_WITH_LUA)
    lua_script_shutdown();
#endif
    conjob_shutdown();

    rw_event(SEVERITY_INFO, "SYSTEM", "Railway Control Center shutting down");
    g_engine.system_state = SYSTEM_SHUTDOWN;
    g_engine.initialized = false;

    lock_leave();
}

bool railway_is_initialized(void)
{
    return g_engine.initialized;
}

/* ==========================================================================
 * Tick loop  (🎛️)
 * ========================================================================== */
RwResult railway_tick(unsigned delta_ms)
{
    if (!g_engine.initialized)
    {
        return RW_ERR_NOT_INITIALIZED;
    }
    if (delta_ms > 5000)
    {
        delta_ms = 5000; /* clamp to keep integration stable after a stall */
    }

    lock_enter();

    g_engine.clock_ms += delta_ms;
    g_engine.tick_count++;

    /* 1. points settle */
    switch_controller_tick(delta_ms);
    rw_points_tick(delta_ms);

    /* 2. routes: release cleared ones, re-prove the rest */
    interlocking_tick(delta_ms);

    /* 3. signals follow the routes */
    signal_controller_tick(delta_ms);
    signal_controller_apply_route_aspects();

    /* 4. trains move under the aspects just applied */
    train_controller_tick(delta_ms);

    /* 5. field inputs */
    rw_sensors_tick(delta_ms);

    /* 6. occupancy from trains + circuits */
    rw_track_recompute_from_circuits();

    /* 7. automated control jobs */
    conjob_tick(delta_ms);

    /* 8. scripts run against a consistent world */
#if defined(MF_WITH_LUA)
    lua_script_tick(delta_ms);
#endif

    /* Housekeeping: raise a standing alarm while the emergency is active. */
    if (g_engine.emergency)
    {
        g_engine.system_state = SYSTEM_EMERGENCY;
    }
    else if (g_engine.system_state == SYSTEM_EMERGENCY)
    {
        g_engine.system_state = SYSTEM_ONLINE;
    }

    rw_bump_revision();
    lock_leave();
    return RW_OK;
}

/* ==========================================================================
 * Status  (🎛️)
 * ========================================================================== */
RwResult railway_get_status(RwSystemStatus *status)
{
    RailwayEngine *e = &g_engine;
    size_t i;
    int alarms = 0;

    if (status == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }
    if (!e->initialized)
    {
        return RW_ERR_NOT_INITIALIZED;
    }

    lock_enter();
    memset(status, 0, sizeof(*status));

    status->state = e->system_state;
    status->ticks = e->tick_count;
    status->uptime_seconds = (unsigned long)(e->clock_ms / 1000UL);
    status->conflicts_blocked = e->conflicts_blocked;

    status->signals_total = (int)e->signal_count;
    for (i = 0; i < e->signal_count; ++i)
    {
        if (e->signals[i].aspect == SIGNAL_GREEN)
        {
            status->signals_green++;
        }
        else if (e->signals[i].aspect == SIGNAL_RED)
        {
            status->signals_red++;
        }
    }

    status->switches_total = (int)e->point_count;
    status->switches_locked = switch_controller_locked_count();

    status->tracks_total = (int)e->track_count;
    for (i = 0; i < e->track_count; ++i)
    {
        switch (e->tracks[i].occupancy)
        {
        case TRACK_CLEAR:
            status->tracks_clear++;
            break;
        case TRACK_OCCUPIED:
            status->tracks_occupied++;
            break;
        case TRACK_UNKNOWN:
            status->tracks_unknown++;
            break;
        }
    }

    status->trains_total = (int)e->train_count;
    for (i = 0; i < e->train_count; ++i)
    {
        if (e->trains[i].state == TRAIN_RUNNING)
        {
            status->trains_running++;
        }
        else
        {
            status->trains_stopped++;
        }
    }

    status->routes_total = (int)e->route_count;
    status->routes_locked = interlocking_locked_route_count();

    for (i = 0; i < e->event_count; ++i)
    {
        if (!e->events[i].acknowledged && (e->events[i].severity >= SEVERITY_WARNING))
        {
            alarms++;
        }
    }
    status->alarms = alarms;

    lock_leave();
    return RW_OK;
}

SystemState railway_system_state(void)
{
    return g_engine.system_state;
}

const char *railway_last_error(void)
{
    return g_engine.last_error[0] ? g_engine.last_error : "No error";
}

const char *railway_system_name(void)
{
    return g_engine.name;
}

const char *railway_signal_box_name(void)
{
    return g_engine.signal_box;
}

/* ==========================================================================
 * Internal view builders. The API hands out copies only, so a front end can
 * never mutate engine state by writing through a pointer.
 * ========================================================================== */
static void signal_to_info(const SignalStateInternal *src, RwSignalInfo *info)
{
    memset(info, 0, sizeof(*info));
    info->id = src->id;
    snprintf(info->name, sizeof(info->name), "%s", src->name);
    info->aspect = src->aspect;
    info->commanded = src->commanded;
    info->manual = src->manual;
    info->in_service = src->in_service;
    info->track = src->track;
    info->holding_route = src->held_by_route;

    /* Diagram position: signals sit at the exit end of their track, offset a
     * little so an entry and an exit signal on the same track do not overlap. */
    {
        const TrackState *track = NULL;
        size_t i;
        for (i = 0; i < g_engine.track_count; ++i)
        {
            if (g_engine.tracks[i].id == src->track)
            {
                track = &g_engine.tracks[i];
                break;
            }
        }
        if (track != NULL)
        {
            const float along = src->at_exit ? 0.92f : 0.08f;
            info->latitude = track->start_lat + (track->end_lat - track->start_lat) * along;
            info->longitude = track->start_lon + (track->end_lon - track->start_lon) * along;
        }
    }
}

static void point_to_info(const PointState *src, RwSwitchInfo *info)
{
    memset(info, 0, sizeof(*info));
    info->id = src->id;
    snprintf(info->name, sizeof(info->name), "%s", src->name);
    info->position = src->position;
    info->commanded = src->commanded;
    info->locked = src->locked;
    info->in_service = src->in_service;
    info->moving = rw_point_is_moving(src);
    info->locked_by_route = src->locked_by_route;
    info->latitude = src->latitude;
    info->longitude = src->longitude;
}

static void track_to_info(const TrackState *src, RwTrackInfo *info)
{
    memset(info, 0, sizeof(*info));
    info->id = src->id;
    snprintf(info->name, sizeof(info->name), "%s", src->name);
    info->occupancy = src->occupancy;
    info->occupied_by = src->occupied_by;
    info->speed_limit_kph = src->speed_limit_kph;
    info->in_service = src->in_service;
    info->length_m = src->length_m;
    info->start_lat = src->start_lat;
    info->start_lon = src->start_lon;
    info->end_lat = src->end_lat;
    info->end_lon = src->end_lon;
}

static void train_to_info(const TrainStateInternal *src, RwTrainInfo *info)
{
    memset(info, 0, sizeof(*info));
    info->id = src->id;
    snprintf(info->name, sizeof(info->name), "%s", src->headcode);
    info->state = src->state;
    info->speed_kph = src->speed_kph;
    info->max_speed_kph = src->max_speed_kph;
    info->track = src->track;
    info->route = src->route;
    info->carriages = src->carriages;
    info->emergency_brake = src->emergency_brake;
    info->latitude = src->latitude;
    info->longitude = src->longitude;
    info->delay_seconds = src->delay_seconds;
}

static void route_to_info(const RouteStateInternal *src, RwRouteInfo *info)
{
    memset(info, 0, sizeof(*info));
    info->id = src->id;
    snprintf(info->name, sizeof(info->name), "%s", src->name);
    info->state = src->state;
    info->entry_signal = src->entry_signal;
    info->exit_signal = src->exit_signal;
    info->booked_by = src->booked_by;
    info->step_count = src->step_count;
    snprintf(info->conflict, sizeof(info->conflict), "%s", src->conflict);
}

static void sensor_to_info(const SensorState *src, RwSensorInfo *info)
{
    memset(info, 0, sizeof(*info));
    info->id = src->id;
    snprintf(info->name, sizeof(info->name), "%s", src->name);
    info->kind = (int)src->kind;
    info->state = (int)src->state;
    info->track = src->track;
    info->value = src->value;
    info->in_service = src->in_service;
}

/* ==========================================================================
 * Signals  (🚦)
 * ========================================================================== */
RwResult railway_set_signal(int signal_id, SignalState state)
{
    RwResult result;
    if (!g_engine.initialized)
    {
        return RW_ERR_NOT_INITIALIZED;
    }
    lock_enter();
    result = signal_controller_set_aspect((RwId)signal_id, state);
    if (result != RW_OK)
    {
        rw_set_error("Signal %d refused: %s", signal_id, rw_result_string(result));
    }
    rw_bump_revision();
    lock_leave();
    return result;
}

RwResult railway_set_signal_mode(int signal_id, bool manual)
{
    RwResult result;
    if (!g_engine.initialized)
    {
        return RW_ERR_NOT_INITIALIZED;
    }
    lock_enter();
    result = signal_controller_set_manual((RwId)signal_id, manual);
    rw_bump_revision();
    lock_leave();
    return result;
}

RwResult railway_set_signal_in_service(int signal_id, bool in_service)
{
    RwResult result;
    if (!g_engine.initialized)
    {
        return RW_ERR_NOT_INITIALIZED;
    }
    lock_enter();
    result = signal_controller_set_in_service((RwId)signal_id, in_service);
    rw_bump_revision();
    lock_leave();
    return result;
}

RwResult railway_get_signal(int signal_id, SignalState *state)
{
    RwResult result;
    if (state == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }
    lock_enter();
    result = signal_controller_get_aspect((RwId)signal_id, state);
    lock_leave();
    return result;
}

RwResult railway_get_signal_info(int signal_id, RwSignalInfo *info)
{
    RwResult result;
    if (info == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }
    lock_enter();
    result = signal_controller_get_info((RwId)signal_id, info);
    lock_leave();
    return result;
}

int railway_signal_count(void)
{
    return (int)g_engine.signal_count;
}

RwResult railway_get_signal_at(int index, RwSignalInfo *info)
{
    if (info == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }
    if (index < 0 || (size_t)index >= g_engine.signal_count)
    {
        return RW_ERR_INVALID_ID;
    }
    lock_enter();
    signal_to_info(&g_engine.signals[index], info);
    lock_leave();
    return RW_OK;
}

/* ==========================================================================
 * Switches  (🔀)
 * ========================================================================== */
RwResult railway_set_switch(int switch_id, SwitchPosition position)
{
    RwResult result;
    if (!g_engine.initialized)
    {
        return RW_ERR_NOT_INITIALIZED;
    }
    lock_enter();
    result = switch_controller_set_position((RwId)switch_id, position);
    if (result != RW_OK)
    {
        rw_set_error("Point %d refused: %s", switch_id, rw_result_string(result));
    }
    rw_bump_revision();
    lock_leave();
    return result;
}

RwResult railway_release_switch(int switch_id)
{
    RwResult result;
    if (!g_engine.initialized)
    {
        return RW_ERR_NOT_INITIALIZED;
    }
    lock_enter();
    result = switch_controller_release((RwId)switch_id);
    rw_bump_revision();
    lock_leave();
    return result;
}

RwResult railway_set_switch_in_service(int switch_id, bool in_service)
{
    RwResult result;
    if (!g_engine.initialized)
    {
        return RW_ERR_NOT_INITIALIZED;
    }
    lock_enter();
    result = switch_controller_set_in_service((RwId)switch_id, in_service);
    rw_bump_revision();
    lock_leave();
    return result;
}

RwResult railway_get_switch(int switch_id, SwitchPosition *position)
{
    RwResult result;
    if (position == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }
    lock_enter();
    result = switch_controller_get_position((RwId)switch_id, position);
    lock_leave();
    return result;
}

RwResult railway_get_switch_info(int switch_id, RwSwitchInfo *info)
{
    RwResult result;
    if (info == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }
    lock_enter();
    result = switch_controller_get_info((RwId)switch_id, info);
    lock_leave();
    return result;
}

int railway_switch_count(void)
{
    return (int)g_engine.point_count;
}

RwResult railway_get_switch_at(int index, RwSwitchInfo *info)
{
    if (info == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }
    if (index < 0 || (size_t)index >= g_engine.point_count)
    {
        return RW_ERR_INVALID_ID;
    }
    lock_enter();
    point_to_info(&g_engine.points[index], info);
    lock_leave();
    return RW_OK;
}

/* ==========================================================================
 * Trains  (🚆)
 * ========================================================================== */
RwResult railway_set_train_state(int train_id, TrainState state)
{
    RwResult result;
    if (!g_engine.initialized)
    {
        return RW_ERR_NOT_INITIALIZED;
    }
    lock_enter();
    result = train_controller_set_state((RwId)train_id, state);
    if (result != RW_OK)
    {
        rw_set_error("Train %d refused: %s", train_id, rw_result_string(result));
    }
    rw_bump_revision();
    lock_leave();
    return result;
}

RwResult railway_get_train_state(int train_id, TrainState *state)
{
    RwResult result;
    if (state == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }
    lock_enter();
    result = train_controller_get_state((RwId)train_id, state);
    lock_leave();
    return result;
}

RwResult railway_get_train_info(int train_id, RwTrainInfo *info)
{
    RwResult result;
    if (info == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }
    lock_enter();
    result = train_controller_get_info((RwId)train_id, info);
    lock_leave();
    return result;
}

RwResult railway_assign_train_route(int train_id, int route_id)
{
    RwResult result;
    if (!g_engine.initialized)
    {
        return RW_ERR_NOT_INITIALIZED;
    }
    lock_enter();
    result = train_controller_assign_route((RwId)train_id, (RwId)route_id);
    rw_bump_revision();
    lock_leave();
    return result;
}

RwResult railway_set_train_speed(int train_id, float speed_kph)
{
    RwResult result;
    if (!g_engine.initialized)
    {
        return RW_ERR_NOT_INITIALIZED;
    }
    lock_enter();
    result = train_controller_set_speed((RwId)train_id, speed_kph);
    rw_bump_revision();
    lock_leave();
    return result;
}

RwResult railway_hold_train(int train_id, bool hold)
{
    RwResult result;
    if (!g_engine.initialized)
    {
        return RW_ERR_NOT_INITIALIZED;
    }
    lock_enter();
    result = train_controller_hold((RwId)train_id, hold);
    rw_bump_revision();
    lock_leave();
    return result;
}

int railway_train_count(void)
{
    return (int)g_engine.train_count;
}

RwResult railway_get_train_at(int index, RwTrainInfo *info)
{
    if (info == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }
    if (index < 0 || (size_t)index >= g_engine.train_count)
    {
        return RW_ERR_INVALID_ID;
    }
    lock_enter();
    train_to_info(&g_engine.trains[index], info);
    lock_leave();
    return RW_OK;
}

/* ==========================================================================
 * Rolling stock  (🚃)
 * ========================================================================== */
int railway_car_count(void)
{
    return (int)g_engine.car_count;
}

int railway_train_car_count(int train_id)
{
    return rw_cars_of_train_count((RwId)train_id);
}

RwResult railway_get_car_info(int car_id, RwCarInfo *info)
{
    if (info == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }
    if (rw_car_by_id((RwId)car_id) == NULL)
    {
        return RW_ERR_INVALID_ID;
    }
    lock_enter();
    rw_car_to_info((RwId)car_id, info);
    lock_leave();
    return RW_OK;
}

RwResult railway_get_car_at(int index, RwCarInfo *info)
{
    if (info == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }
    if (index < 0 || (size_t)index >= g_engine.car_count)
    {
        return RW_ERR_INVALID_ID;
    }
    lock_enter();
    rw_car_to_info(g_engine.cars[index].id, info);
    lock_leave();
    return RW_OK;
}

RwResult railway_get_train_car(int train_id, int position, RwCarInfo *info)
{
    RwId car_id;
    RwResult result;

    if (info == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }
    car_id = rw_car_find_on_train((RwId)train_id, position);
    if (car_id == RW_ID_NONE)
    {
        return RW_ERR_INVALID_ID;
    }
    lock_enter();
    rw_car_to_info(car_id, info);
    lock_leave();
    result = RW_OK;
    return result;
}

int railway_list_train_cars(int train_id, RwCarInfo *out, int max)
{
    int written;

    if (out == NULL || max <= 0)
    {
        return 0;
    }
    lock_enter();
    written = rw_cars_of_train((RwId)train_id, out, max);
    lock_leave();
    return written;
}

float railway_train_length_m(int train_id)
{
    float length;

    lock_enter();
    length = rw_train_formation_length((RwId)train_id);
    lock_leave();
    return length;
}

int railway_add_train_car(int train_id, RcpCarType type,
                          const char *number, const char *designation,
                          float length_m, float tare_tonnes, int capacity)
{
    RwId car_id;

    if (!g_engine.initialized)
    {
        return (int)RW_ID_NONE;
    }
    lock_enter();
    car_id = rw_car_add((RwId)train_id, type, number, designation,
                        length_m, tare_tonnes, capacity);
    if (car_id != RW_ID_NONE)
    {
        rw_event(SEVERITY_INFO, "STOCK",
                 "Car %s (%s) attached to train %u",
                 number != NULL ? number : "(new)",
                 rcp_car_type_code(type), (unsigned)train_id);
    }
    rw_bump_revision();
    lock_leave();
    return (int)car_id;
}

RwResult railway_remove_train_car(int car_id)
{
    RwResult result;

    lock_enter();
    result = rw_car_remove((RwId)car_id);
    rw_bump_revision();
    lock_leave();
    return result;
}

RwResult railway_set_car_status(int car_id, RcpCarStatus status)
{
    CarStateInternal *car = rw_car_by_id((RwId)car_id);

    if (car == NULL)
    {
        return RW_ERR_INVALID_ID;
    }
    lock_enter();
    car->status = status;
    if (status == CAR_STATUS_OUT_OF_SERVICE || status == CAR_STATUS_DEFECTIVE)
    {
        car->in_service = false;
    }
    rw_event(status == CAR_STATUS_DEFECTIVE ? SEVERITY_WARNING : SEVERITY_INFO,
             "STOCK", "Car %s status set to %s",
             car->number, rcp_car_status_name(status));
    rw_bump_revision();
    lock_leave();
    return RW_OK;
}

RwResult railway_set_car_in_service(int car_id, bool in_service)
{
    CarStateInternal *car = rw_car_by_id((RwId)car_id);

    if (car == NULL)
    {
        return RW_ERR_INVALID_ID;
    }
    lock_enter();
    car->in_service = in_service;
    if (!in_service && car->status == CAR_STATUS_OK)
    {
        car->status = CAR_STATUS_OUT_OF_SERVICE;
    }
    rw_event(SEVERITY_INFO, "STOCK", "Car %s %s service",
             car->number, in_service ? "returned to" : "withdrawn from");
    rw_bump_revision();
    lock_leave();
    return RW_OK;
}

int railway_find_car(const char *number)
{
    return (int)rw_car_find(number);
}

/* ==========================================================================
 * Routes  (🔒)
 * ========================================================================== */
RwResult railway_request_route(int route_id)
{
    RwResult result;
    if (!g_engine.initialized)
    {
        return RW_ERR_NOT_INITIALIZED;
    }
    lock_enter();
    result = interlocking_request_route((RwId)route_id);
    if (result != RW_OK)
    {
        rw_set_error("Route %d refused: %s", route_id, rw_result_string(result));
    }
    rw_bump_revision();
    lock_leave();
    return result;
}

RwResult railway_cancel_route(int route_id)
{
    RwResult result;
    if (!g_engine.initialized)
    {
        return RW_ERR_NOT_INITIALIZED;
    }
    lock_enter();
    result = interlocking_cancel_route((RwId)route_id);
    rw_bump_revision();
    lock_leave();
    return result;
}

RwResult railway_get_route_info(int route_id, RwRouteInfo *info)
{
    const RouteStateInternal *route;
    if (info == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }
    if (route_id <= 0 || (size_t)route_id > g_engine.route_count)
    {
        return RW_ERR_INVALID_ID;
    }
    lock_enter();
    route = &g_engine.routes[route_id - 1];
    route_to_info(route, info);
    lock_leave();
    return RW_OK;
}

int railway_route_count(void)
{
    return (int)g_engine.route_count;
}

RwResult railway_get_route_at(int index, RwRouteInfo *info)
{
    if (info == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }
    if (index < 0 || (size_t)index >= g_engine.route_count)
    {
        return RW_ERR_INVALID_ID;
    }
    lock_enter();
    route_to_info(&g_engine.routes[index], info);
    lock_leave();
    return RW_OK;
}

int railway_find_route(const char *name)
{
    int id;
    lock_enter();
    id = (int)interlocking_find_route(name);
    lock_leave();
    return (id == (int)RW_ID_NONE) ? -1 : id;
}

RwResult railway_set_route_and_dispatch(int route_id)
{
    RwResult result;
    if (!g_engine.initialized)
    {
        return RW_ERR_NOT_INITIALIZED;
    }
    lock_enter();
    result = interlocking_set_route_and_dispatch((RwId)route_id);
    rw_bump_revision();
    lock_leave();
    return result;
}

/* ==========================================================================
 * Emergency  (🛑)
 * ========================================================================== */
int railway_emergency_stop(void)
{
    if (!g_engine.initialized)
    {
        return 0;
    }
    return (int)railway_emergency_stop_reason("Operator emergency stop");
}

RwResult railway_emergency_stop_reason(const char *reason)
{
    if (!g_engine.initialized)
    {
        return RW_ERR_NOT_INITIALIZED;
    }
    lock_enter();
    rw_emergency_engage(reason);
    rw_bump_revision();
    lock_leave();
    return RW_OK;
}

RwResult railway_clear_emergency(void)
{
    if (!g_engine.initialized)
    {
        return RW_ERR_NOT_INITIALIZED;
    }
    lock_enter();

    if (!g_engine.emergency)
    {
        lock_leave();
        return RW_OK;
    }

    if (!rw_emergency_can_clear())
    {
        rw_set_error("Cannot clear the emergency while trains are still moving "
                     "or a section remains occupied by an emergency-braked train");
        lock_leave();
        return RW_ERR_BUSY;
    }

    g_engine.emergency = false;
    g_engine.emergency_reason[0] = '\0';
    g_engine.system_state = SYSTEM_ONLINE;

    rw_event(SEVERITY_INFO, "EMERGENCY", "Emergency stop cleared - normal working restored");
    switch_controller_release_all();
    rw_bump_revision();
    lock_leave();
    return RW_OK;
}

bool railway_emergency_active(void)
{
    return g_engine.emergency;
}

/* ==========================================================================
 * Tracks  (🛤️) and sensors  (📡)
 * ========================================================================== */
RwResult railway_get_track(int track_id, TrackOccupancy *occupancy)
{
    const TrackState *track;
    if (occupancy == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }
    if (track_id <= 0 || (size_t)track_id > g_engine.track_count)
    {
        return RW_ERR_INVALID_ID;
    }
    lock_enter();
    track = &g_engine.tracks[track_id - 1];
    *occupancy = track->occupancy;
    lock_leave();
    return RW_OK;
}

RwResult railway_get_track_info(int track_id, RwTrackInfo *info)
{
    if (info == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }
    if (track_id <= 0 || (size_t)track_id > g_engine.track_count)
    {
        return RW_ERR_INVALID_ID;
    }
    lock_enter();
    track_to_info(&g_engine.tracks[track_id - 1], info);
    lock_leave();
    return RW_OK;
}

int railway_track_count(void)
{
    return (int)g_engine.track_count;
}

RwResult railway_get_track_at(int index, RwTrackInfo *info)
{
    if (info == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }
    if (index < 0 || (size_t)index >= g_engine.track_count)
    {
        return RW_ERR_INVALID_ID;
    }
    lock_enter();
    track_to_info(&g_engine.tracks[index], info);
    lock_leave();
    return RW_OK;
}

RwResult railway_inject_sensor(int sensor_id, int value, int state)
{
    RwResult result;
    if (!g_engine.initialized)
    {
        return RW_ERR_NOT_INITIALIZED;
    }
    lock_enter();
    result = rw_sensor_apply((RwId)sensor_id, value, state);
    rw_bump_revision();
    lock_leave();
    return result;
}

RwResult railway_fail_track_circuit(int track_id, bool failed)
{
    if (!g_engine.initialized)
    {
        return RW_ERR_NOT_INITIALIZED;
    }
    if (track_id <= 0 || (size_t)track_id > g_engine.track_count)
    {
        return RW_ERR_INVALID_ID;
    }

    lock_enter();
    {
        TrackState *track = &g_engine.tracks[track_id - 1];
        track->circuit_failed = failed;

        if (failed)
        {
            /* Fail safe: an undetected section must present as UNKNOWN, which
             * the interlocking treats as unsafe. */
            track->occupancy = TRACK_UNKNOWN;
            rw_event(SEVERITY_ALARM, "SENSOR",
                     "Track circuit failure on %s - section treated as UNSAFE", track->name);
        }
        else
        {
            rw_track_recompute_from_circuits();
            rw_event(SEVERITY_INFO, "SENSOR", "Track circuit restored on %s", track->name);
        }
    }
    rw_bump_revision();
    lock_leave();
    return RW_OK;
}

int railway_sensor_count(void)
{
    return (int)g_engine.sensor_count;
}

RwResult railway_get_sensor_at(int index, RwSensorInfo *info)
{
    if (info == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }
    if (index < 0 || (size_t)index >= g_engine.sensor_count)
    {
        return RW_ERR_INVALID_ID;
    }
    lock_enter();
    sensor_to_info(&g_engine.sensors[index], info);
    lock_leave();
    return RW_OK;
}

/* ==========================================================================
 * Events  (📝)
 * ========================================================================== */
int railway_event_count(void)
{
    return (int)g_engine.event_count;
}

RwResult railway_get_event_at(int index, RwEventInfo *info)
{
    const EventRecord *record;
    if (info == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }
    if (index < 0 || (size_t)index >= g_engine.event_count)
    {
        return RW_ERR_INVALID_ID;
    }
    lock_enter();
    record = &g_engine.events[index];
    memset(info, 0, sizeof(*info));
    info->id = record->id;
    info->severity = record->severity;
    snprintf(info->category, sizeof(info->category), "%s", record->category);
    snprintf(info->message, sizeof(info->message), "%s", record->message);
    snprintf(info->timestamp, sizeof(info->timestamp), "%s", record->timestamp);
    info->acknowledged = record->acknowledged;
    lock_leave();
    return RW_OK;
}

int railway_unacknowledged_count(void)
{
    int count = 0;
    size_t i;
    lock_enter();
    for (i = 0; i < g_engine.event_count; ++i)
    {
        if (!g_engine.events[i].acknowledged && g_engine.events[i].severity >= SEVERITY_WARNING)
        {
            count++;
        }
    }
    lock_leave();
    return count;
}

RwResult railway_acknowledge_event(int event_id)
{
    size_t i;
    lock_enter();
    for (i = 0; i < g_engine.event_count; ++i)
    {
        if ((int)g_engine.events[i].id == event_id)
        {
            g_engine.events[i].acknowledged = true;
            lock_leave();
            return RW_OK;
        }
    }
    lock_leave();
    return RW_ERR_INVALID_ID;
}

int railway_acknowledge_all(void)
{
    size_t i;
    int count = 0;
    lock_enter();
    for (i = 0; i < g_engine.event_count; ++i)
    {
        if (!g_engine.events[i].acknowledged)
        {
            g_engine.events[i].acknowledged = true;
            count++;
        }
    }
    lock_leave();
    return count;
}

RwResult railway_log_message(RwSeverity severity, const char *category, const char *message)
{
    if (message == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }
    lock_enter();
    rw_event(severity, category != NULL ? category : "OPERATOR", "%s", message);
    lock_leave();
    return RW_OK;
}

/* ==========================================================================
 * Engine support functions
 *
 * rw_point_apply_position() and rw_point_reached() live in rcp_point.c next
 * to the point controller they belong to. Only the revision bump helper is
 * needed here.
 * ========================================================================== */
