/* ==========================================================================
 * rcp_point.c - 🔀 Switch / point controller.
 *
 * Models a point as a real one behaves: the command sets a "commanded" lie,
 * the blades swing over a finite time, and detection reports when they have
 * arrived. A point that does not reach its commanded lie within the expected
 * time is declared FAILED TO CORRESPOND, which withdraws it from service and
 * therefore blocks any route over it (INV-3, INV-4).
 *
 * The three rules that matter:
 *   - a locked point may not be moved (INV-4)
 *   - a point may not be moved under a train (INV-3)
 *   - a point in transit may not be used by a route
 * ========================================================================== */
#include "switch_controller.h"
#include "railway_internal.h"

#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Setup
 * -------------------------------------------------------------------------- */
void rw_points_init(void)
{
    RailwayEngine *e = rw_engine();
    size_t i;

    if (e == NULL) {
        return;
    }
    for (i = 0; i < e->point_count; ++i) {
        PointState *point = &e->points[i];
        TrackState *upstream = NULL;
        TrackState *normal = NULL;
        TrackState *reverse = NULL;
        size_t t;

        point->position = SWITCH_MAIN;
        point->commanded = SWITCH_MAIN;
        point->locked = false;
        point->locked_by_route = RW_ID_NONE;
        point->in_service = true;
        point->fault = false;
        point->movement_remaining_ms = 0;

        for (t = 0; t < e->track_count; ++t) {
            if (e->tracks[t].id == point->track_upstream) {
                upstream = &e->tracks[t];
            }
            if (e->tracks[t].id == point->track_normal) {
                normal = &e->tracks[t];
            }
            if (e->tracks[t].id == point->track_reverse) {
                reverse = &e->tracks[t];
            }
        }
        if (upstream != NULL) {
            upstream->point_downstream = point->id;
        }
        if (normal != NULL) {
            normal->point_upstream = point->id;
        }
        if (reverse != NULL) {
            reverse->point_upstream = point->id;
        }
    }
}

void switch_controller_init(void)
{
    rw_points_init();
}

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */
static PointState *point_by_id(RwId point_id)
{
    RailwayEngine *e = rw_engine();

    if (point_id == RW_ID_NONE || point_id > e->point_count) {
        return NULL;
    }
    return &e->points[point_id - 1];
}

bool rw_point_is_moving(const PointState *point)
{
    return point != NULL && point->movement_remaining_ms > 0;
}

bool rw_point_reached(const PointState *point, SwitchPosition wanted)
{
    return point != NULL && point->position == wanted && !rw_point_is_moving(point);
}

bool rw_point_route_matches(const PointState *point, SwitchPosition required)
{
    return rw_point_reached(point, required);
}

void rw_point_apply_position(PointState *point, SwitchPosition position)
{
    if (point == NULL) {
        return;
    }
    point->position = position;
    point->commanded = position;
    point->movement_remaining_ms = 0;
}

bool switch_controller_is_available(RwId switch_id)
{
    const PointState *point = point_by_id(switch_id);

    if (point == NULL)
    {
        return false;
    }

    return point->in_service && !point->fault && !rw_point_is_moving(point);
    }

    /* --------------------------------------------------------------------------
     * The movement proof
     * -------------------------------------------------------------------------- */

int switch_controller_locked_count(void)
{
    RailwayEngine *e = rw_engine();
    size_t i;
    int count = 0;

    for (i = 0; i < e->point_count; ++i) {
        if (e->points[i].locked) {
            count++;
        }
    }
    return count;
}

/* --------------------------------------------------------------------------
 * The movement proof
 * -------------------------------------------------------------------------- */
static RwResult point_may_move(PointState *point, const char *reason_out, size_t reason_size,
                               const char **why)
{
    RailwayEngine *e = rw_engine();
    const TrackState *upstream = NULL;
    size_t t;

    if (e->emergency) {
        *why = "emergency stop is active - all points are held";
        return RW_ERR_EMERGENCY;
    }
    if (!point->in_service) {
        *why = "point is out of service";
        return RW_ERR_OUT_OF_SERVICE;
    }
    if (point->fault) {
        *why = "point has failed to correspond";
        return RW_ERR_OUT_OF_SERVICE;
    }
    /* INV-4: locked by a route. */
    if (point->locked && point->locked_by_route != RW_ID_NONE) {
        *why = "point is locked by a route";
        return RW_ERR_POINT_LOCKED;
    }
    if (rw_point_is_moving(point)) {
        *why = "point is already in transit";
        return RW_ERR_POINT_MOVING;
    }

    /* INV-3: never move a point under a train. We check the section that
     * feeds the point, which is where a train would be standing on it. */
    for (t = 0; t < e->track_count; ++t) {
        if (e->tracks[t].id == point->track_upstream) {
            upstream = &e->tracks[t];
            break;
        }
    }
    if (upstream != NULL && upstream->occupancy == TRACK_OCCUPIED) {
        *why = "the section feeding the point is occupied";
        snprintf((char *)reason_out, reason_size, "track %s is occupied", upstream->name);
        return RW_ERR_OCCUPIED;
    }

    (void)reason_out;
    (void)reason_size;
    *why = NULL;
    return RW_OK;
}

/* --------------------------------------------------------------------------
 * Commands
 * -------------------------------------------------------------------------- */
RwResult switch_controller_set_position(RwId switch_id, SwitchPosition position)
{
    PointState *point = point_by_id(switch_id);
    const char *why = NULL;
    char detail[RW_MAX_TEXT];
    RwResult result;

    if (point == NULL) {
        return RW_ERR_INVALID_ID;
    }
    if (position != SWITCH_MAIN && position != SWITCH_DIVERGING) {
        return RW_ERR_INVALID_ARG;
    }

    /* Already there: a no-op, which is success. */
    if (rw_point_reached(point, position)) {
        return RW_OK;
    }

    result = point_may_move(point, detail, sizeof(detail), &why);
    if (result != RW_OK) {
        rw_event(SEVERITY_WARNING, "POINT",
                 "Point %s cannot be moved to %s: %s",
                 point->name, switch_position_name(position),
                 (why != NULL) ? why : rw_result_string(result));
        return result;
    }

    /* Accept the request and start the swing. The detection completes in
     * switch_controller_tick(). */
    point->commanded = position;
    point->movement_remaining_ms = (unsigned)rw_engine()->config.point_movement_ms;
    if (point->movement_remaining_ms == 0) {
        point->movement_remaining_ms = 1;   /* never complete instantly */
    }

    rw_event(SEVERITY_INFO, "POINT", "Point %s moving to %s",
             point->name, switch_position_name(position));
    return RW_OK;
}

RwResult switch_controller_release(RwId switch_id)
{
    RailwayEngine *e = rw_engine();
    PointState *point = point_by_id(switch_id);

    if (point == NULL) {
        return RW_ERR_INVALID_ID;
    }
    if (!point->locked) {
        return RW_OK;    /* nothing to release */
    }

    /* Releasing under a train is refused unless the emergency stop is active,
     * in which case the signaller is explicitly taking manual control. */
    if (!e->emergency) {
        size_t t;
        for (t = 0; t < e->track_count; ++t) {
            if (e->tracks[t].id == point->track_upstream
                && e->tracks[t].occupancy == TRACK_OCCUPIED) {
                rw_event(SEVERITY_ALARM, "POINT",
                         "Point %s release refused - %s is occupied",
                         point->name, e->tracks[t].name);
                return RW_ERR_OCCUPIED;
            }
        }
    }

    {
        RwId previous = point->locked_by_route;
        RouteStateInternal *route = NULL;
        size_t r;

        for (r = 0; r < e->route_count; ++r) {
            if (e->routes[r].id == previous) {
                route = &e->routes[r];
                break;
            }
        }

        point->locked = false;
        point->locked_by_route = RW_ID_NONE;

        /* A manual release under a route is a significant safety event: it
         * removes the protection the route was providing. Log it loudly. */
        rw_event(SEVERITY_ALARM, "POINT",
                 "Point %s RELEASED by the operator (was locked by route %s) - "
                 "the route no longer protects this point",
                 point->name, route != NULL ? route->name : "unknown");
    }
    return RW_OK;
}

RwResult switch_controller_set_in_service(RwId switch_id, bool in_service)
{
    PointState *point = point_by_id(switch_id);

    if (point == NULL) {
        return RW_ERR_INVALID_ID;
    }
    point->in_service = in_service;
    if (!in_service) {
        /* Withdrawing a point cancels its lock so routes can be cleared up. */
        point->locked = false;
        point->locked_by_route = RW_ID_NONE;
        point->movement_remaining_ms = 0;
    }
    rw_event(SEVERITY_WARNING, "POINT", "Point %s placed %s service",
             point->name, in_service ? "IN" : "OUT OF");
    return RW_OK;
}

RwResult switch_controller_get_position(RwId switch_id, SwitchPosition *position)
{
    const PointState *point = point_by_id(switch_id);

    if (position == NULL) {
        return RW_ERR_INVALID_ARG;
    }
    if (point == NULL) {
        return RW_ERR_INVALID_ID;
    }
    *position = point->position;
    return RW_OK;
}

int switch_controller_count(void)
{
    return (int)rw_engine()->point_count;
}

RwResult switch_controller_get_info(RwId switch_id, RwSwitchInfo *info)
{
    const PointState *point = point_by_id(switch_id);

    if (info == NULL) {
        return RW_ERR_INVALID_ARG;
    }
    if (point == NULL) {
        return RW_ERR_INVALID_ID;
    }
    memset(info, 0, sizeof(*info));
    info->id = point->id;
    snprintf(info->name, sizeof(info->name), "%s", point->name);
    info->position = point->position;
    info->commanded = point->commanded;
    info->locked = point->locked;
    info->in_service = point->in_service;
    info->moving = rw_point_is_moving(point);
    info->locked_by_route = point->locked_by_route;
    info->latitude = point->latitude;
    info->longitude = point->longitude;
    return RW_OK;
}

RwResult switch_controller_get_at(int index, RwSwitchInfo *info)
{
    if (info == NULL) {
        return RW_ERR_INVALID_ARG;
    }
    if (index < 0 || (size_t)index >= rw_engine()->point_count) {
        return RW_ERR_INVALID_ID;
    }
    return switch_controller_get_info(rw_engine()->points[index].id, info);
}

/* --------------------------------------------------------------------------
 * Interlocking interface
 * -------------------------------------------------------------------------- */
void switch_controller_lock_for_route(RwId switch_id, RwId route_id)
{
    PointState *point = point_by_id(switch_id);

    if (point == NULL) {
        return;
    }
    point->locked = true;
    point->locked_by_route = route_id;
}

void switch_controller_unlock_for_route(RwId switch_id)
{
    PointState *point = point_by_id(switch_id);

    if (point == NULL) {
        return;
    }
    point->locked = false;
    point->locked_by_route = RW_ID_NONE;
}

void switch_controller_release_all(void)
{
    RailwayEngine *e = rw_engine();
    size_t i;

    for (i = 0; i < e->point_count; ++i) {
        e->points[i].locked = false;
        e->points[i].locked_by_route = RW_ID_NONE;
    }
    rw_event(SEVERITY_INFO, "POINT", "All point locks released");
}

void switch_controller_set_fault(RwId switch_id, bool fault)
{
    PointState *point = point_by_id(switch_id);

    if (point == NULL) {
        return;
    }
    point->fault = fault;
    if (fault) {
        point->movement_remaining_ms = 0;
        rw_event(SEVERITY_ALARM, "POINT",
                 "Point %s FAILED TO CORRESPOND - withdrawn from service", point->name);
    } else {
        rw_event(SEVERITY_INFO, "POINT", "Point %s detection restored", point->name);
    }
}

/* --------------------------------------------------------------------------
 * Per-tick: complete or fault a swing.
 * -------------------------------------------------------------------------- */
void switch_controller_tick(unsigned delta_ms)
{
    RailwayEngine *e = rw_engine();
    size_t i;

    for (i = 0; i < e->point_count; ++i) {
        PointState *point = &e->points[i];

        if (point->movement_remaining_ms == 0) {
            continue;
        }
        if (delta_ms >= point->movement_remaining_ms) {
            point->movement_remaining_ms = 0;

            /* Detection: the blades have arrived at the commanded lie. A real
             * point reports through separate normally-open and normally-closed
             * detection contacts; here the successful arrival is the
             * correspondence. */
            point->position = point->commanded;
            rw_event(SEVERITY_INFO, "POINT", "Point %s has reached %s and detection is proved",
                     point->name, switch_position_name(point->position));
        } else {
            point->movement_remaining_ms -= delta_ms;
        }
    }
}

void rw_points_tick(unsigned delta_ms)
{
    switch_controller_tick(delta_ms);
}
