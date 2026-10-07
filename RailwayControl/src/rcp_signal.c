/* ==========================================================================
 * rcp_signal.c - 🚦 Signal controller.
 *
 * Invariant INV-1 is enforced here: a signal may only move off danger when a
 * route is set for it AND every section the route covers is proven clear.
 * Moving a signal TO danger is always permitted - it is the safe direction -
 * and is never refused, not even during an emergency.
 * ========================================================================== */
#include "signal_controller.h"
#include "railway_internal.h"
#include "railcontrol.h"
#include "interlocking.h"

#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Setup
 * -------------------------------------------------------------------------- */
void rw_signals_init(void)
{
    RailwayEngine *e = rw_engine();
    size_t i;

    if (e == NULL) {
        return;
    }
    for (i = 0; i < e->signal_count; ++i) {
        SignalStateInternal *signal = &e->signals[i];
        TrackState *track = NULL;
        size_t t;

        signal->aspect = SIGNAL_RED;
        signal->commanded = SIGNAL_RED;
        signal->manual = false;
        signal->in_service = true;
        signal->approach_locked = false;
        signal->held_by_route = RW_ID_NONE;

        /* Bind the signal to the section it stands at. */
        for (t = 0; t < e->track_count; ++t) {
            if (e->tracks[t].id == signal->track) {
                track = &e->tracks[t];
                break;
            }
        }
        if (track != NULL) {
            if (signal->at_exit) {
                track->exit_signal = signal->id;
            } else {
                track->entry_signal = signal->id;
            }
        }
    }
}

void signal_controller_init(void)
{
    rw_signals_init();
}

/* --------------------------------------------------------------------------
 * Queries
 * -------------------------------------------------------------------------- */
bool signal_controller_is_proceed(SignalState aspect)
{
    return aspect == SIGNAL_GREEN || aspect == SIGNAL_YELLOW || aspect == SIGNAL_CALLON;
}

bool rw_signal_is_proceed(SignalState aspect)
{
    return signal_controller_is_proceed(aspect);
}

static SignalStateInternal *signal_by_id(RwId signal_id)
{
    RailwayEngine *e = rw_engine();

    if (signal_id == RW_ID_NONE || signal_id > e->signal_count) {
        return NULL;
    }
    return &e->signals[signal_id - 1];
}

RwResult signal_controller_get_aspect(RwId signal_id, SignalState *aspect)
{
    const SignalStateInternal *signal = signal_by_id(signal_id);

    if (aspect == NULL) {
        return RW_ERR_INVALID_ARG;
    }
    if (signal == NULL) {
        return RW_ERR_INVALID_ID;
    }
    *aspect = signal->aspect;
    return RW_OK;
}

int signal_controller_count(void)
{
    return (int)rw_engine()->signal_count;
}

RwResult signal_controller_get_info(RwId signal_id, RwSignalInfo *info)
{
    const SignalStateInternal *signal = signal_by_id(signal_id);
    const RailwayEngine *e = rw_engine();

    if (info == NULL) {
        return RW_ERR_INVALID_ARG;
    }
    if (signal == NULL) {
        return RW_ERR_INVALID_ID;
    }

    memset(info, 0, sizeof(*info));
    info->id = signal->id;
    snprintf(info->name, sizeof(info->name), "%s", signal->name);
    info->aspect = signal->aspect;
    info->commanded = signal->commanded;
    info->manual = signal->manual;
    info->in_service = signal->in_service;
    info->track = signal->track;
    info->holding_route = signal->held_by_route;

    {
        const TrackState *track = NULL;
        size_t i;
        for (i = 0; i < e->track_count; ++i) {
            if (e->tracks[i].id == signal->track) {
                track = &e->tracks[i];
                break;
            }
        }
        if (track != NULL) {
            const float along = signal->at_exit ? 0.92f : 0.08f;
            info->latitude = track->start_lat + (track->end_lat - track->start_lat) * along;
            info->longitude = track->start_lon + (track->end_lon - track->start_lon) * along;
        }
    }
    return RW_OK;
}

RwResult signal_controller_get_at(int index, RwSignalInfo *info)
{
    if (info == NULL) {
        return RW_ERR_INVALID_ARG;
    }
    if (index < 0 || (size_t)index >= rw_engine()->signal_count) {
        return RW_ERR_INVALID_ID;
    }
    return signal_controller_get_info(rw_engine()->signals[index].id, info);
}

/* --------------------------------------------------------------------------
 * The clearing proof  (INV-1)
 *
 * Before a signal may show proceed we require:
 *   a) the emergency stop is not active
 *   b) the signal is in service
 *   c) a route is set with this signal as its entry signal
 *   d) every track on that route is CLEAR (or UNKNOWN is tolerated only when
 *      the configuration explicitly allows it - it does not by default)
 * -------------------------------------------------------------------------- */
static RwResult signal_may_clear(SignalStateInternal *signal, char *reason, size_t reason_size)
{
    RailwayEngine *e = rw_engine();
    RouteStateInternal *route = NULL;
    size_t i;

    if (e->emergency) {
        snprintf(reason, reason_size, "emergency stop is active");
        return RW_ERR_EMERGENCY;
    }
    if (!signal->in_service) {
        snprintf(reason, reason_size, "signal %s is out of service", signal->name);
        return RW_ERR_OUT_OF_SERVICE;
    }

    /* Find the route this signal reads over. A signal may also be an exit
     * signal, in which case it is held by the route that ends at it. */
    for (i = 0; i < e->route_count; ++i) {
        RouteStateInternal *candidate = &e->routes[i];
        if (candidate->entry_signal != signal->id) {
            continue;
        }
        if (candidate->state == ROUTE_LOCKED || candidate->state == ROUTE_OCCUPIED) {
            route = candidate;
            break;
        }
    }

    if (route == NULL) {
        snprintf(reason, reason_size,
                 "no route is set for signal %s - INV-1", signal->name);
        return RW_ERR_INTERLOCK;
    }

    /* The section immediately beyond the signal must be proven. */
    for (i = 0; i < route->step_count; ++i) {
        const TrackState *track = NULL;
        size_t t;

        for (t = 0; t < e->track_count; ++t) {
            if (e->tracks[t].id == route->steps[i].track) {
                track = &e->tracks[t];
                break;
            }
        }
        if (track == NULL) {
            continue;
        }
        if (track->circuit_failed) {
            snprintf(reason, reason_size,
                     "track circuit on %s has failed - INV-5", track->name);
            return RW_ERR_INTERLOCK;
        }
        if (track->occupancy == TRACK_OCCUPIED && i > 0) {
            /* The first section may legitimately be occupied by the train the
             * route is being set for; anything beyond it must be clear. */
            snprintf(reason, reason_size, "section %s is occupied", track->name);
            return RW_ERR_OCCUPIED;
        }
        if (track->occupancy == TRACK_UNKNOWN) {
            snprintf(reason, reason_size,
                     "section %s occupancy is UNKNOWN - treated as unsafe", track->name);
            return RW_ERR_INTERLOCK;
        }
    }

    /* An intermediate aspect: look one signal ahead. If the next signal is at
     * danger, show caution rather than clear. This is the approach release. */
    return RW_OK;
}

/* --------------------------------------------------------------------------
 * Aspect control
 * -------------------------------------------------------------------------- */
void rw_signal_force(SignalStateInternal *signal, SignalState aspect)
{
    if (signal == NULL) {
        return;
    }
    signal->aspect = aspect;
    signal->commanded = aspect;
}

RwResult signal_controller_set_aspect(RwId signal_id, SignalState aspect)
{
    RailwayEngine *e = rw_engine();
    SignalStateInternal *signal = signal_by_id(signal_id);
    char reason[RW_MAX_TEXT];

    (void)e;
    if (signal == NULL) {
        return RW_ERR_INVALID_ID;
    }
    if (!signal->in_service && aspect != SIGNAL_RED) {
        return RW_ERR_OUT_OF_SERVICE;
    }

    /* Restoring to danger is always allowed, in any state, for any caller.
     * This is what makes the emergency stop work and is the fail-safe
     * direction of every signalling system. */
    if (!signal_controller_is_proceed(aspect)) {
        signal->aspect = SIGNAL_RED;
        signal->commanded = SIGNAL_RED;
        signal->held_by_route = RW_ID_NONE;
        rw_event(SEVERITY_INFO, "SIGNAL", "Signal %s replaced to danger", signal->name);
        return RW_OK;
    }

    /* Clearing (or caution) requires the interlocking proof. */
    {
        RwResult result = signal_may_clear(signal, reason, sizeof(reason));
        if (result != RW_OK) {
            /* Replacing to danger on refusal keeps the layout consistent. */
            signal->aspect = SIGNAL_RED;
            signal->commanded = SIGNAL_RED;
            rw_event(SEVERITY_WARNING, "SIGNAL",
                     "Signal %s cannot be cleared: %s", signal->name, reason);
            return result;
        }
    }

    /* Approval logic: GREEN when the caller asked for it and the route is
     * proved out, YELLOW otherwise. The interlocking's automatic working
     * uses YELLOW as the intermediate aspect. */
    signal->aspect = (aspect == SIGNAL_GREEN) ? SIGNAL_GREEN : SIGNAL_YELLOW;
    signal->commanded = signal->aspect;
    signal->manual = true;

    rw_event(SEVERITY_INFO, "SIGNAL", "Signal %s cleared to %s",
             signal->name, signal_state_name(signal->aspect));
    return RW_OK;
}

RwResult signal_controller_set_manual(RwId signal_id, bool manual)
{
    SignalStateInternal *signal = signal_by_id(signal_id);

    if (signal == NULL) {
        return RW_ERR_INVALID_ID;
    }
    signal->manual = manual;
    rw_event(SEVERITY_INFO, "SIGNAL", "Signal %s placed under %s control",
             signal->name, manual ? "MANUAL" : "AUTOMATIC");
    return RW_OK;
}

RwResult signal_controller_set_in_service(RwId signal_id, bool in_service)
{
    SignalStateInternal *signal = signal_by_id(signal_id);

    if (signal == NULL) {
        return RW_ERR_INVALID_ID;
    }
    signal->in_service = in_service;
    if (!in_service) {
        /* Withdrawing a signal must force it to danger at once. */
        signal->aspect = SIGNAL_RED;
        signal->commanded = SIGNAL_RED;
        signal->held_by_route = RW_ID_NONE;
    }
    rw_event(SEVERITY_WARNING, "SIGNAL", "Signal %s placed %s service",
             signal->name, in_service ? "IN" : "OUT OF");
    return RW_OK;
}

/* --------------------------------------------------------------------------
 * Interlocking interface
 * -------------------------------------------------------------------------- */
void signal_controller_hold_for_route(RwId signal_id, RwId route_id)
{
    SignalStateInternal *signal = signal_by_id(signal_id);

    if (signal == NULL) {
        return;
    }
    signal->held_by_route = route_id;
    signal->approach_locked = true;
}

void signal_controller_release_from_route(RwId signal_id)
{
    SignalStateInternal *signal = signal_by_id(signal_id);

    if (signal == NULL) {
        return;
    }
    signal->held_by_route = RW_ID_NONE;
    signal->approach_locked = false;
    /* Replacing to danger on release is the safe behaviour: the route is no
     * longer proved, so the signal must not invite a train forward. */
    signal->aspect = SIGNAL_RED;
    signal->commanded = SIGNAL_RED;
}

void signal_controller_all_to_danger(const char *reason)
{
    RailwayEngine *e = rw_engine();
    size_t i;

    for (i = 0; i < e->signal_count; ++i) {
        e->signals[i].aspect = SIGNAL_RED;
        e->signals[i].commanded = SIGNAL_RED;
        e->signals[i].held_by_route = RW_ID_NONE;
    }
    rw_event(SEVERITY_CRITICAL, "SIGNAL", "All signals replaced to danger: %s",
             reason != NULL ? reason : "unspecified");
}

/* --------------------------------------------------------------------------
 * Automatic working: the interlocking drives aspects for set routes.
 * -------------------------------------------------------------------------- */
void signal_controller_apply_route_aspects(void)
{
    RailwayEngine *e = rw_engine();
    RcSettings *settings = rc_settings();
    size_t r;

    if (!settings->auto_signal_routes) {
        return;
    }

    for (r = 0; r < e->route_count; ++r) {
        RouteStateInternal *route = &e->routes[r];
        SignalStateInternal *entry;
        char reason[RW_MAX_TEXT];

        if (route->state != ROUTE_LOCKED && route->state != ROUTE_OCCUPIED) {
            continue;
        }
        if (route->entry_signal == RW_ID_NONE) {
            continue;
        }
        entry = signal_by_id(route->entry_signal);
        if (entry == NULL) {
            continue;
        }
        /* A manually worked signal is left alone. */
        if (entry->manual) {
            continue;
        }
        if (entry->aspect != SIGNAL_RED) {
            continue;    /* already off */
        }

        if (signal_may_clear(entry, reason, sizeof(reason)) == RW_OK) {
            entry->aspect = SIGNAL_YELLOW;
            entry->commanded = SIGNAL_YELLOW;
        }
    }
}

/* --------------------------------------------------------------------------
 * Per-tick work: expire approach locking, honour the emergency.
 * -------------------------------------------------------------------------- */
void signal_controller_tick(unsigned delta_ms)
{
    RailwayEngine *e = rw_engine();
    size_t i;

    (void)delta_ms;

    if (e->emergency) {
        /* INV-6: every signal must show danger during an emergency. Checked
         * each tick rather than only on entry, so a signal that somehow got
         * cleared is corrected immediately. */
        for (i = 0; i < e->signal_count; ++i) {
            if (e->signals[i].aspect != SIGNAL_RED) {
                e->signals[i].aspect = SIGNAL_RED;
                e->signals[i].commanded = SIGNAL_RED;
                rw_event(SEVERITY_ALARM, "SIGNAL",
                         "Signal %s forced to danger by the emergency stop (INV-6)",
                         e->signals[i].name);
            }
        }
    }
}

void rw_signals_tick(unsigned delta_ms)
{
    signal_controller_tick(delta_ms);
}
