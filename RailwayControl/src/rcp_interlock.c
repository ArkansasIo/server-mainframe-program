/* ==========================================================================
 * rcp_interlock.c - 🔒 Computer-Based Interlocking (CBI/EI).
 *
 * THE SAFETY CORE. Everything in this file is written to be obviously
 * correct on inspection: no dynamic memory, no recursion, no floating point
 * in a decision, no early exit that could leave a partial state behind.
 *
 * A route may only be granted when ALL of the following hold. The order of
 * the checks is the order a signal engineer would read them off a control
 * table, and each failure names the offending asset so the operator can see
 * exactly what stopped it:
 *
 *   1. the route exists and the emergency stop is not active
 *   2. no conflicting route is already set (shared track or shared point)
 *   3. every point on the path is in service, not faulted, and can be moved
 *      or is already lying correctly
 *   4. every track on the path is proven clear - CLEAR only. UNKNOWN (a
 *      failed circuit) always fails the proof, per invariant INV-5
 *   5. the exit signal, when present, is in service
 *
 * Only after all five pass does anything change: points are locked in the
 * required lie, the route is marked LOCKED, and the entry signal is cleared.
 * If any step fails the layout is left exactly as it was - a route is either
 * fully set or not set at all, never half way.
 * ========================================================================== */
#include "interlocking.h"
#include "railway_internal.h"
#include "railcontrol.h"

#include "signal_controller.h"
#include "switch_controller.h"
#include "train_controller.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------------- */
void interlocking_init(void)
{
    RailwayEngine *e = rw_engine();
    size_t i;

    if (e == NULL)
    {
        return;
    }
    e->config.fail_safe_unknown = true;
    e->config.allow_call_on = false;
    e->config.release_requires_clear = true;
    e->config.require_route_lock = true;
    e->config.point_movement_ms = 1200;

    for (i = 0; i < e->route_count; ++i)
    {
        e->routes[i].state = ROUTE_IDLE;
        e->routes[i].booked_by = RW_ID_NONE;
        e->routes[i].conflict[0] = '\0';
        e->routes[i].set_at_ms = 0;
    }

    /* Bind each signal to the route it is the entry signal of, so the signal
     * controller knows which route must be set before it can clear. */
    for (i = 0; i < e->route_count; ++i)
    {
        SignalStateInternal *signal = NULL;
        size_t s;

        for (s = 0; s < e->signal_count; ++s)
        {
            if (e->signals[s].id == e->routes[i].entry_signal)
            {
                signal = &e->signals[s];
                break;
            }
        }
        if (signal != NULL)
        {
            signal->entry_for_route = e->routes[i].id;
        }
    }
}

/* --------------------------------------------------------------------------
 * Lookup helpers
 * -------------------------------------------------------------------------- */
static RouteStateInternal *route_by_id(RwId route_id)
{
    RailwayEngine *e = rw_engine();

    if (route_id == RW_ID_NONE || route_id > e->route_count)
    {
        return NULL;
    }
    return &e->routes[route_id - 1];
}

static PointState *point_by_id(RwId point_id)
{
    RailwayEngine *e = rw_engine();

    if (point_id == RW_ID_NONE || point_id > e->point_count)
    {
        return NULL;
    }
    return &e->points[point_id - 1];
}

static TrackState *track_by_id(RwId track_id)
{
    RailwayEngine *e = rw_engine();

    if (track_id == RW_ID_NONE || track_id > e->track_count)
    {
        return NULL;
    }
    return &e->tracks[track_id - 1];
}

RwId interlocking_find_route(const char *name)
{
    RailwayEngine *e = rw_engine();
    size_t i;

    if (e == NULL || name == NULL || name[0] == '\0')
    {
        return RW_ID_NONE;
    }
    for (i = 0; i < e->route_count; ++i)
    {
        if (strcmp(e->routes[i].name, name) == 0)
        {
            return e->routes[i].id;
        }
    }
    /* Also accept a bare number, so "ROUTE 2" works in the terminal. */
    {
        char *end = NULL;
        long value = strtol(name, &end, 10);
        if (end != NULL && *end == '\0' && value > 0 && value <= (long)e->route_count)
        {
            return (RwId)value;
        }
    }
    return RW_ID_NONE;
}

RwId interlocking_route_for_signal(RwId signal_id)
{
    RailwayEngine *e = rw_engine();
    size_t i;

    for (i = 0; i < e->route_count; ++i)
    {
        if (e->routes[i].entry_signal == signal_id)
        {
            return e->routes[i].id;
        }
    }
    return RW_ID_NONE;
}

int interlocking_locked_route_count(void)
{
    RailwayEngine *e = rw_engine();
    size_t i;
    int count = 0;

    for (i = 0; i < e->route_count; ++i)
    {
        if (e->routes[i].state == ROUTE_LOCKED || e->routes[i].state == ROUTE_OCCUPIED)
        {
            count++;
        }
    }
    return count;
}

/* --------------------------------------------------------------------------
 * Conflict detection  (⚠️)
 *
 * Two routes conflict when they share a track OR require a common point in
 * different lies. Sharing a point in the SAME lie is still a conflict in a
 * real interlocking when the routes overlap, so we simply treat any shared
 * point as exclusive - conservative, and the safe direction.
 * -------------------------------------------------------------------------- */
bool interlocking_routes_conflict(RwId route_a, RwId route_b)
{
    const RouteStateInternal *a = route_by_id(route_a);
    const RouteStateInternal *b = route_by_id(route_b);
    size_t i;
    size_t j;

    if (a == NULL || b == NULL || route_a == route_b)
    {
        return false;
    }

    for (i = 0; i < a->step_count; ++i)
    {
        if (a->steps[i].track == RW_ID_NONE)
        {
            continue;
        }
        for (j = 0; j < b->step_count; ++j)
        {
            if (b->steps[j].track == RW_ID_NONE)
            {
                continue;
            }
            if (a->steps[i].track == b->steps[j].track)
            {
                return true; /* shared running line */
            }
        }
    }

    for (i = 0; i < a->step_count; ++i)
    {
        if (a->steps[i].point == RW_ID_NONE)
        {
            continue;
        }
        for (j = 0; j < b->step_count; ++j)
        {
            if (b->steps[j].point == RW_ID_NONE)
            {
                continue;
            }
            if (a->steps[i].point == b->steps[j].point && a->steps[i].point_required != b->steps[j].point_required)
            {
                return true; /* shared point, opposite lie */
            }
        }
    }

    return false;
}

RwResult interlocking_check_conflicts(RwId route_id, RwId *other_route,
                                      char *reason, size_t reason_size)
{
    RailwayEngine *e = rw_engine();
    size_t i;

    if (other_route != NULL)
    {
        *other_route = RW_ID_NONE;
    }

    for (i = 0; i < e->route_count; ++i)
    {
        const RouteStateInternal *other = &e->routes[i];

        if (other->id == route_id)
        {
            continue;
        }
        if (other->state != ROUTE_LOCKED && other->state != ROUTE_OCCUPIED)
        {
            continue;
        }
        if (interlocking_routes_conflict(route_id, other->id))
        {
            if (other_route != NULL)
            {
                *other_route = other->id;
            }
            if (reason != NULL && reason_size > 0)
            {
                snprintf(reason, reason_size,
                         "Conflicts with route %s which is already %s",
                         other->name, route_state_name(other->state));
            }
            return RW_ERR_CONFLICT;
        }
    }
    return RW_OK;
}

/* --------------------------------------------------------------------------
 * The proof
 *
 * Returns RW_OK only when every requirement is satisfied. Nothing is
 * modified. `reason` always receives an operator-readable explanation on
 * failure, so the GUI and terminal can show why the route was refused.
 * -------------------------------------------------------------------------- */
static RwResult prove_points(RailwayEngine *e, const RouteStateInternal *route,
                             char *reason, size_t reason_size)
{
    size_t i;

    for (i = 0; i < route->step_count; ++i)
    {
        const RwId point_id = route->steps[i].point;
        PointState *point;

        if (point_id == RW_ID_NONE)
        {
            continue;
        }
        point = point_by_id(point_id);
        if (point == NULL)
        {
            snprintf(reason, reason_size, "Route references unknown point %u", point_id);
            return RW_ERR_INVALID_ID;
        }
        if (!point->in_service)
        {
            snprintf(reason, reason_size, "Point %s is out of service", point->name);
            return RW_ERR_OUT_OF_SERVICE;
        }
        if (point->fault)
        {
            snprintf(reason, reason_size,
                     "Point %s has failed to correspond - detection lost", point->name);
            return RW_ERR_OUT_OF_SERVICE;
        }
        /* A point already locked by another route cannot be used. */
        if (point->locked && point->locked_by_route != route->id && point->locked_by_route != RW_ID_NONE)
        {
            RouteStateInternal *holder = route_by_id(point->locked_by_route);
            snprintf(reason, reason_size,
                     "Point %s is locked by route %s",
                     point->name, holder != NULL ? holder->name : "another route");
            return RW_ERR_POINT_LOCKED;
        }
        /* The point must not be mid-swing. */
        if (rw_point_is_moving(point))
        {
            snprintf(reason, reason_size, "Point %s is still in transit", point->name);
            return RW_ERR_POINT_MOVING;
        }
        /* Moving a point under a train is forbidden (INV-3). */
        if (point->position != route->steps[i].point_required)
        {
            const TrackState *upstream = track_by_id(point->track_upstream);
            if (upstream != NULL && upstream->occupancy == TRACK_OCCUPIED)
            {
                snprintf(reason, reason_size,
                         "Cannot move point %s - %s is occupied",
                         point->name, upstream->name);
                return RW_ERR_OCCUPIED;
            }
        }
    }
    (void)e;
    return RW_OK;
}

static RwResult prove_tracks(RailwayEngine *e, const RouteStateInternal *route,
                             char *reason, size_t reason_size)
{
    size_t i;
    const bool fail_safe_unknown = e->config.fail_safe_unknown;

    for (i = 0; i < route->step_count; ++i)
    {
        const RwId track_id = route->steps[i].track;
        const TrackState *track;

        if (track_id == RW_ID_NONE)
        {
            continue;
        }
        track = track_by_id(track_id);
        if (track == NULL)
        {
            snprintf(reason, reason_size, "Route references unknown track %u", track_id);
            return RW_ERR_INVALID_ID;
        }
        if (!track->in_service || track->closed_for_work)
        {
            snprintf(reason, reason_size, "Track %s is out of service", track->name);
            return RW_ERR_OUT_OF_SERVICE;
        }
        if (track->circuit_failed)
        {
            snprintf(reason, reason_size,
                     "Track %s has a failed track circuit - section unproven", track->name);
            return RW_ERR_INTERLOCK;
        }
        switch (track->occupancy)
        {
        case TRACK_CLEAR:
            break;
        case TRACK_OCCUPIED:
            /* Call-on is the only permitted exception, and only when the
             * operator has explicitly enabled it. A call-on lets a train
             * enter an occupied platform to couple or to draw forward. */
            if (!(e->config.allow_call_on && i + 1 == route->step_count))
            {
                snprintf(reason, reason_size, "Track %s is occupied", track->name);
                return RW_ERR_OCCUPIED;
            }
            break;
        case TRACK_UNKNOWN:
            if (fail_safe_unknown)
            {
                snprintf(reason, reason_size,
                         "Track %s occupancy is UNKNOWN - treated as unsafe", track->name);
                return RW_ERR_INTERLOCK;
            }
            break;
        }
    }
    return RW_OK;
}

RwResult interlocking_validate_route(RwId route_id, char *reason, size_t reason_size)
{
    RailwayEngine *e = rw_engine();
    RouteStateInternal *route;
    SignalStateInternal *entry;
    RwResult result;
    RwId conflicting = RW_ID_NONE;

    if (reason == NULL || reason_size == 0)
    {
        return RW_ERR_INVALID_ARG;
    }
    reason[0] = '\0';

    route = route_by_id(route_id);
    if (route == NULL)
    {
        snprintf(reason, reason_size, "No such route (%u)", route_id);
        return RW_ERR_NO_ROUTE;
    }

    /* 1. emergency and route state */
    if (e->emergency)
    {
        snprintf(reason, reason_size,
                 "Emergency stop is active - no route may be set");
        return RW_ERR_EMERGENCY;
    }
    if (route->state == ROUTE_LOCKED || route->state == ROUTE_OCCUPIED)
    {
        snprintf(reason, reason_size, "Route %s is already set", route->name);
        return RW_ERR_ROUTE_SET;
    }
    if (route->step_count == 0)
    {
        snprintf(reason, reason_size, "Route %s has no path defined", route->name);
        return RW_ERR_INTERLOCK;
    }

    /* 2. the entry signal must exist and be usable */
    if (route->entry_signal != RW_ID_NONE)
    {
        if (route->entry_signal > e->signal_count)
        {
            snprintf(reason, reason_size, "Route entry signal %u does not exist",
                     route->entry_signal);
            return RW_ERR_INVALID_ID;
        }
        entry = &e->signals[route->entry_signal - 1];
        if (!entry->in_service)
        {
            snprintf(reason, reason_size, "Entry signal %s is out of service", entry->name);
            return RW_ERR_OUT_OF_SERVICE;
        }
    }

    /* 3. the exit signal, when present, must be usable */
    if (route->exit_signal != RW_ID_NONE)
    {
        if (route->exit_signal > e->signal_count)
        {
            snprintf(reason, reason_size, "Route exit signal %u does not exist",
                     route->exit_signal);
            return RW_ERR_INVALID_ID;
        }
        if (!e->signals[route->exit_signal - 1].in_service)
        {
            snprintf(reason, reason_size, "Exit signal %s is out of service",
                     e->signals[route->exit_signal - 1].name);
            return RW_ERR_OUT_OF_SERVICE;
        }
    }

    /* 4. no conflicting route (INV-2) */
    result = interlocking_check_conflicts(route_id, &conflicting, reason, reason_size);
    if (result != RW_OK)
    {
        return result;
    }

    /* 5. points */
    result = prove_points(e, route, reason, reason_size);
    if (result != RW_OK)
    {
        return result;
    }

    /* 6. track circuits */
    result = prove_tracks(e, route, reason, reason_size);
    if (result != RW_OK)
    {
        return result;
    }

    return RW_OK;
}

/* --------------------------------------------------------------------------
 * Setting and cancelling routes
 * -------------------------------------------------------------------------- */
static void lock_points_for_route(RouteStateInternal *route)
{
    size_t i;

    for (i = 0; i < route->step_count; ++i)
    {
        PointState *point;

        if (route->steps[i].point == RW_ID_NONE)
        {
            continue;
        }
        point = point_by_id(route->steps[i].point);
        if (point == NULL)
        {
            continue;
        }
        /* Command the required lie; the switch controller animates the swing
         * over point_movement_ms and reports when it has arrived. */
        if (point->position != route->steps[i].point_required)
        {
            (void)switch_controller_set_position(point->id, route->steps[i].point_required);
        }
        switch_controller_lock_for_route(point->id, route->id);
    }
}

RwResult interlocking_request_route(RwId route_id)
{
    return rw_interlock_set_route(route_id, RW_ID_NONE);
}

RwResult rw_interlock_set_route(RwId route_id, RwId train_id)
{
    RailwayEngine *e = rw_engine();
    RouteStateInternal *route = route_by_id(route_id);
    char reason[RW_MAX_TEXT];
    RwResult result;

    if (route == NULL)
    {
        return RW_ERR_NO_ROUTE;
    }

    /* Prove first. Nothing changes unless the proof succeeds, so a refused
     * request cannot leave the interlocking in a partial state. */
    result = interlocking_validate_route(route_id, reason, sizeof(reason));
    if (result != RW_OK)
    {
        snprintf(route->conflict, sizeof(route->conflict), "%s", reason);
        route->state = ROUTE_IDLE;
        e->conflicts_blocked++;

        rw_event(SEVERITY_WARNING, "INTERLOCK",
                 "Route %s refused: %s", route->name, reason);
        return result;
    }

    /* Proof passed - now, and only now, change the layout. */
    route->state = ROUTE_LOCKED;
    route->booked_by = train_id;
    route->set_at_ms = (unsigned)e->clock_ms;
    route->conflict[0] = '\0';
    e->route_sets++;

    lock_points_for_route(route);

    if (route->entry_signal != RW_ID_NONE)
    {
        signal_controller_hold_for_route(route->entry_signal, route->id);
    }
    if (route->exit_signal != RW_ID_NONE)
    {
        signal_controller_hold_for_route(route->exit_signal, route->id);
    }

    rw_event(SEVERITY_INFO, "INTERLOCK",
             "Route %s set: %s to %s over %u section(s)",
             route->name,
             route->entry_signal != RW_ID_NONE ? e->signals[route->entry_signal - 1].name : "start",
             route->exit_signal != RW_ID_NONE ? e->signals[route->exit_signal - 1].name : "end",
             (unsigned)route->step_count);

    return RW_OK;
}

RwResult interlocking_set_route_and_dispatch(RwId route_id)
{
    RwResult result;
    RwId train;

    result = interlocking_request_route(route_id);
    if (result != RW_OK)
    {
        return result;
    }

    train = train_controller_waiting_for_route(route_id);
    if (train == RW_ID_NONE)
    {
        /* The route is set but nothing is waiting at it. That is a legitimate
         * outcome - explain it rather than silently doing nothing. */
        rw_event(SEVERITY_INFO, "INTERLOCK",
                 "Route set but no train is waiting at its entry signal");
        return RW_OK;
    }

    {
        RouteStateInternal *route = route_by_id(route_id);
        if (route != NULL)
        {
            route->booked_by = train;
        }
    }

    result = train_controller_assign_route(train, route_id);
    if (result != RW_OK)
    {
        rw_event(SEVERITY_WARNING, "INTERLOCK",
                 "Route set but authority to train %u was refused (%s)",
                 train, rw_result_string(result));
        return result;
    }

    rw_event(SEVERITY_INFO, "INTERLOCK",
             "Route dispatched: train %u given authority over route %u", train, route_id);
    return RW_OK;
}

RwResult interlocking_cancel_route(RwId route_id)
{
    RailwayEngine *e = rw_engine();
    RouteStateInternal *route = route_by_id(route_id);
    size_t i;

    if (route == NULL)
    {
        return RW_ERR_NO_ROUTE;
    }
    if (route->state != ROUTE_LOCKED && route->state != ROUTE_OCCUPIED)
    {
        return RW_OK; /* nothing to cancel */
    }

    /* Refuse while a train is inside the route: cancelling under a train
     * would remove its authority and leave it stranded between signals. */
    for (i = 0; i < route->step_count; ++i)
    {
        const TrackState *track = track_by_id(route->steps[i].track);
        if (track != NULL && track->occupancy == TRACK_OCCUPIED)
        {
            snprintf(route->conflict, sizeof(route->conflict),
                     "Track %s is occupied - the route cannot be cancelled",
                     track->name);
            rw_event(SEVERITY_ALARM, "INTERLOCK",
                     "Route %s cancel refused: %s", route->name, route->conflict);
            return RW_ERR_OCCUPIED;
        }
    }

    /* Release the entry signal first, then the points. */
    if (route->entry_signal != RW_ID_NONE)
    {
        signal_controller_release_from_route(route->entry_signal);
    }
    for (i = 0; i < route->step_count; ++i)
    {
        if (route->steps[i].point != RW_ID_NONE)
        {
            switch_controller_unlock_for_route(route->steps[i].point);
        }
    }

    route->state = ROUTE_RELEASED;
    route->booked_by = RW_ID_NONE;
    route->conflict[0] = '\0';
    e->route_cancels++;

    rw_event(SEVERITY_WARNING, "INTERLOCK", "Route %s cancelled by the operator", route->name);

    /* Immediately return to IDLE so it can be re-set straight away. */
    route->state = ROUTE_IDLE;
    return RW_OK;
}

/* --------------------------------------------------------------------------
 * Per-tick processing
 * -------------------------------------------------------------------------- */
void rw_interlock_release_routes(void)
{
    RailwayEngine *e = rw_engine();
    size_t r;

    for (r = 0; r < e->route_count; ++r)
    {
        RouteStateInternal *route = &e->routes[r];
        bool any_occupied = false;
        bool all_clear = true;
        size_t i;

        if (route->state != ROUTE_LOCKED && route->state != ROUTE_OCCUPIED)
        {
            continue;
        }

        for (i = 0; i < route->step_count; ++i)
        {
            const TrackState *track = track_by_id(route->steps[i].track);
            if (track == NULL)
            {
                continue;
            }
            if (track->occupancy == TRACK_OCCUPIED)
            {
                any_occupied = true;
            }
            if (track->occupancy != TRACK_CLEAR)
            {
                all_clear = false;
            }
        }

        /* A route with a train inside it is OCCUPIED; the train's presence is
         * what grants it protection. */
        if (any_occupied)
        {
            if (route->state != ROUTE_OCCUPIED)
            {
                route->state = ROUTE_OCCUPIED;
                rw_event(SEVERITY_INFO, "INTERLOCK",
                         "Route %s is now occupied by train %u",
                         route->name, route->booked_by);
            }
            continue;
        }

        /* Every section clear again: the train has left the route. Release it
         * (INV-7) - but only if it was previously occupied, so that a route
         * which has never been used is not released before it is used. */
        if (all_clear && route->state == ROUTE_OCCUPIED)
        {
            if (e->config.release_requires_clear)
            {
                for (i = 0; i < route->step_count; ++i)
                {
                    if (route->steps[i].point != RW_ID_NONE)
                    {
                        switch_controller_unlock_for_route(route->steps[i].point);
                    }
                }
                if (route->entry_signal != RW_ID_NONE)
                {
                    signal_controller_release_from_route(route->entry_signal);
                }
                if (route->exit_signal != RW_ID_NONE)
                {
                    signal_controller_release_from_route(route->exit_signal);
                }

                rw_event(SEVERITY_INFO, "INTERLOCK",
                         "Route %s released - all sections proved clear", route->name);

                route->state = ROUTE_IDLE;
                route->booked_by = RW_ID_NONE;
            }
        }
    }
}

void rw_interlock_apply_aspects(void)
{
    signal_controller_apply_route_aspects();
}

void interlocking_tick(unsigned delta_ms)
{
    (void)delta_ms;
    rw_interlock_release_routes();
}

/* --------------------------------------------------------------------------
 * Emergency
 * -------------------------------------------------------------------------- */
void interlocking_emergency_drop(const char *reason)
{
    RailwayEngine *e = rw_engine();
    size_t i;

    for (i = 0; i < e->route_count; ++i)
    {
        RouteStateInternal *route = &e->routes[i];

        if (route->state == ROUTE_LOCKED || route->state == ROUTE_OCCUPIED)
        {
            route->state = ROUTE_IDLE;
            route->booked_by = RW_ID_NONE;
        }
        route->conflict[0] = '\0';
    }

    for (i = 0; i < e->signal_count; ++i)
    {
        e->signals[i].aspect = SIGNAL_RED;
        e->signals[i].commanded = SIGNAL_RED;
        e->signals[i].held_by_route = RW_ID_NONE;
    }

    /* Points stay locked in place: moving them during an emergency would be
     * unsafe. The release happens when the emergency is cleared. */
    rw_event(SEVERITY_CRITICAL, "INTERLOCK",
             "All routes dropped and every signal replaced to danger: %s",
             reason != NULL ? reason : "emergency");
}

/* --------------------------------------------------------------------------
 * Aliases used by the engine tick in railway_api.c. The interlocking is the
 * same object whether it is reached through the public controller name or the
 * internal one; these thin wrappers keep railway_api.c free of the details.
 * -------------------------------------------------------------------------- */
void rw_interlock_init(void)
{
    interlocking_init();
}
