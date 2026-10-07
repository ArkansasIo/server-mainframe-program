/* ==========================================================================
 * rcp_train.c - 🚆 Train controller (movement authority and speed supervision).
 *
 * This is where the ATC-style speed supervision lives. A train only moves
 * when it holds a movement authority, and even then its speed is capped by
 * the lowest of:
 *
 *     - the traction/stock limit
 *     - the line speed of the section it is on
 *     - the approach speed for a signal at danger ahead
 *     - the target speed the signaller set
 *
 * Braking is modelled as a rate, not a step: a train decelerates at a fixed
 * service rate, and at a much higher rate under emergency braking. That
 * matters because "can I stop before the signal" is the question the whole
 * system exists to answer.
 * ========================================================================== */
#include "train_controller.h"
#include "railway_internal.h"
#include "signal_controller.h"
#include "interlocking.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Service and emergency braking rates, in km/h per second. */
#define RW_SERVICE_BRAKE_RATE    3.0f
#define RW_EMERGENCY_BRAKE_RATE 12.0f
#define RW_ACCELERATION_RATE     1.2f
#define RW_WALKING_PACE_KPH       1.0f
#define RW_APPROACH_DISTANCE_M   350.0f

/* --------------------------------------------------------------------------
 * Setup
 * -------------------------------------------------------------------------- */
void rw_trains_init(void)
{
    RailwayEngine *e = rw_engine();
    size_t i;

    if (e == NULL) {
        return;
    }
    for (i = 0; i < e->train_count; ++i) {
        TrainStateInternal *train = &e->trains[i];
        const TrackState *track = NULL;
        size_t t;

        train->state = TRAIN_STOPPED;
        train->speed_kph = 0.0f;
        train->target_speed_kph = 0.0f;
        train->emergency_brake = false;
        train->held = false;
        train->route = RW_ID_NONE;
        train->blocked_by_signal = RW_ID_NONE;
        train->stopped_since_ms = (unsigned)e->clock_ms;
        train->delay_seconds = 0;
        train->last_reporting_sensor = RW_ID_NONE;

        if (train->max_speed_kph <= 0.0f) {
            train->max_speed_kph = 100.0f;
        }
        if (train->length_m == 0) {
            train->length_m = (unsigned)(train->carriages * 23 + 20);
        }

        for (t = 0; t < e->track_count; ++t) {
            if (e->tracks[t].id == train->track) {
                track = &e->tracks[t];
                break;
            }
        }
        if (track != NULL) {
            train->destination_track = track->id;
            train->latitude = track->start_lat;
            train->longitude = track->start_lon;
            rw_track_set_occupancy(track->id, TRACK_OCCUPIED, train->id);
        }
    }
}

void train_controller_init(void)
{
    rw_trains_init();
}

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */
static TrainStateInternal *train_by_id(RwId train_id)
{
    RailwayEngine *e = rw_engine();

    if (train_id == RW_ID_NONE || train_id > e->train_count) {
        return NULL;
    }
    return &e->trains[train_id - 1];
}

static TrackState *track_of(const TrainStateInternal *train)
{
    RailwayEngine *e = rw_engine();
    size_t i;

    if (train == NULL || train->track == RW_ID_NONE) {
        return NULL;
    }
    for (i = 0; i < e->track_count; ++i) {
        if (e->tracks[i].id == train->track) {
            return &e->tracks[i];
        }
    }
    return NULL;
}

static float clampf(float value, float low, float high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

/* Interpolate the train's diagram position from its position along the track. */
static void update_diagram_position(TrainStateInternal *train, const TrackState *track)
{
    float fraction;

    if (train == NULL || track == NULL) {
        return;
    }
    fraction = (track->length_m > 0.0f)
        ? clampf(train->position_m / track->length_m, 0.0f, 1.0f)
        : 0.0f;

    train->latitude = track->start_lat + (track->end_lat - track->start_lat) * fraction;
    train->longitude = track->start_lon + (track->end_lon - track->start_lon) * fraction;
}

/* --------------------------------------------------------------------------
 * Speed supervision
 *
 * Returns the speed the train is permitted to run at right now. The minimum
 * of every applicable limit wins, which is the safe composition.
 * -------------------------------------------------------------------------- */
static float permitted_speed(const TrainStateInternal *train, RwId *blocking_signal)
{
    RailwayEngine *e = rw_engine();
    const TrackState *track = track_of(train);
    float limit = train->max_speed_kph;

    if (blocking_signal != NULL) {
        *blocking_signal = RW_ID_NONE;
    }
    if (track == NULL) {
        return 0.0f;    /* no track reference: do not move */
    }

    /* Line speed. */
    if (track->speed_limit_kph > 0 && (float)track->speed_limit_kph < limit) {
        limit = (float)track->speed_limit_kph;
    }

    /* The signaller's target. */
    if (train->target_speed_kph >= 0.0f && train->target_speed_kph < limit) {
        limit = train->target_speed_kph;
    }

    /* Held trains do not move. */
    if (train->held) {
        return 0.0f;
    }

    /* A route must be set, and its entry signal must be off, before anything
     * moves. This is the ATC equivalent of "movement authority". */
    if (train->route == RW_ID_NONE) {
        return 0.0f;
    }

    {
        const RouteStateInternal *route = NULL;
        size_t r;

        for (r = 0; r < e->route_count; ++r) {
            if (e->routes[r].id == train->route) {
                route = &e->routes[r];
                break;
            }
        }
        if (route == NULL) {
            return 0.0f;
        }
        if (route->state != ROUTE_LOCKED && route->state != ROUTE_OCCUPIED) {
            return 0.0f;
        }

        /* The signal governing the section ahead. */
        if (route->exit_signal != RW_ID_NONE && route->exit_signal <= e->signal_count) {
            const SignalStateInternal *exit =
                &e->signals[route->exit_signal - 1];
            if (exit->aspect == SIGNAL_RED) {
                /* Prepare to stop at the exit signal. Where the train is far
                 * enough away it may still run at caution; close to the signal
                 * it must be slowing to a stand. */
                const float remaining = track->length_m - train->position_m;
                if (remaining < RW_APPROACH_DISTANCE_M) {
                    const float approach = remaining / RW_APPROACH_DISTANCE_M;
                    const float caution = 25.0f * approach;
                    if (caution < limit) {
                        limit = caution;
                    }
                    if (blocking_signal != NULL) {
                        *blocking_signal = exit->id;
                    }
                } else {
                    if (limit > 40.0f) limit = 40.0f;   /* caution */
                }
            }
        }
    }

    return limit < 0.0f ? 0.0f : limit;
}

/* --------------------------------------------------------------------------
 * Occupancy handover between sections
 *
 * Called when a train reaches the end of its current track. Looks at the
 * points and the route to decide which section it enters next, and refuses
 * the move if that section is not clear - the train then stops short rather
 * than entering an occupied block.
 * -------------------------------------------------------------------------- */
static bool advance_to_next_track(TrainStateInternal *train, float overshoot_m)
{
    RailwayEngine *e = rw_engine();
    const TrackState *current = track_of(train);
    RwId next_id = RW_ID_NONE;
    TrackState *next = NULL;
    size_t i;

    if (current == NULL) {
        return false;
    }

    /* A point downstream selects the next section by its lie. */
    if (current->point_downstream != RW_ID_NONE
        && current->point_downstream <= e->point_count) {
        const PointState *point = &e->points[current->point_downstream - 1];

        if (rw_point_is_moving(point)) {
            return false;   /* never enter a point in transit */
        }
        next_id = (point->position == SWITCH_MAIN) ? point->track_normal : point->track_reverse;
    }

    /* Otherwise follow the route the train holds. */
    if (next_id == RW_ID_NONE && train->route != RW_ID_NONE
        && train->route <= e->route_count) {
        const RouteStateInternal *route = &e->routes[train->route - 1];
        size_t s;
        bool found_current = false;

        for (s = 0; s < route->step_count; ++s) {
            if (found_current) {
                next_id = route->steps[s].track;
                break;
            }
            if (route->steps[s].track == current->id) {
                found_current = true;
            }
        }
    }

    if (next_id == RW_ID_NONE || next_id == current->id) {
        return false;   /* nowhere to go: hold position */
    }

    for (i = 0; i < e->track_count; ++i) {
        if (e->tracks[i].id == next_id) {
            next = &e->tracks[i];
            break;
        }
    }
    if (next == NULL) {
        return false;
    }

    /* Separation: never enter an occupied or unproven section. */
    if (next->occupancy != TRACK_CLEAR || next->circuit_failed) {
        rw_event(SEVERITY_WARNING, "TRAIN",
                 "Train %s held at the boundary of %s - section is %s",
                 train->headcode, next->name,
                 next->circuit_failed ? "UNPROVEN" : track_occupancy_name(next->occupancy));
        return false;
    }

    /* Hand the occupancy over. The old section clears, the new one occupies. */
    rw_track_set_occupancy(current->id, TRACK_CLEAR, RW_ID_NONE);
    rw_track_set_occupancy(next->id, TRACK_OCCUPIED, train->id);

    train->track = next->id;
    train->position_m = overshoot_m;
    update_diagram_position(train, next);

    rw_event(SEVERITY_INFO, "TRAIN", "Train %s entered %s", train->headcode, next->name);
    return true;
}

/* --------------------------------------------------------------------------
 * Commands
 * -------------------------------------------------------------------------- */
RwResult train_controller_set_state(RwId train_id, TrainState state)
{
    RailwayEngine *e = rw_engine();
    TrainStateInternal *train = train_by_id(train_id);

    if (train == NULL) {
        return RW_ERR_INVALID_ID;
    }

    switch (state) {
    case TRAIN_EMERGENCY_STOP:
        train->emergency_brake = true;
        train->state = TRAIN_EMERGENCY_STOP;
        train->target_speed_kph = 0.0f;
        rw_event(SEVERITY_ALARM, "TRAIN",
                 "Train %s EMERGENCY BRAKE applied by the operator", train->headcode);
        return RW_OK;

    case TRAIN_STOPPED:
        train->emergency_brake = false;
        train->target_speed_kph = 0.0f;
        train->state = TRAIN_STOPPED;
        rw_event(SEVERITY_INFO, "TRAIN", "Train %s stopping", train->headcode);
        return RW_OK;

    case TRAIN_RUNNING:
        if (e->emergency) {
            rw_event(SEVERITY_WARNING, "TRAIN",
                     "Train %s cannot run - emergency stop is active", train->headcode);
            return RW_ERR_EMERGENCY;
        }
        if (train->route == RW_ID_NONE) {
            rw_event(SEVERITY_WARNING, "TRAIN",
                     "Train %s cannot run - no movement authority (set a route first)",
                     train->headcode);
            return RW_ERR_NO_ROUTE;
        }
        train->emergency_brake = false;
        train->held = false;
        train->state = TRAIN_RUNNING;
        rw_event(SEVERITY_INFO, "TRAIN", "Train %s running", train->headcode);
        return RW_OK;
    }

    return RW_ERR_INVALID_ARG;
}

RwResult train_controller_set_speed(RwId train_id, float speed_kph)
{
    TrainStateInternal *train = train_by_id(train_id);

    if (train == NULL) {
        return RW_ERR_INVALID_ID;
    }
    if (speed_kph < 0.0f) {
        return RW_ERR_RANGE;
    }
    if (speed_kph > train->max_speed_kph) {
        speed_kph = train->max_speed_kph;   /* clamp to the stock limit */
    }
    train->target_speed_kph = speed_kph;
    if (speed_kph > 0.0f && train->state != TRAIN_RUNNING && !train->emergency_brake) {
        train->state = TRAIN_RUNNING;
    }
    return RW_OK;
}

RwResult train_controller_assign_route(RwId train_id, RwId route_id)
{
    RailwayEngine *e = rw_engine();
    TrainStateInternal *train = train_by_id(train_id);
    const RouteStateInternal *route = NULL;
    size_t r;

    if (train == NULL) {
        return RW_ERR_INVALID_ID;
    }
    if (route_id == RW_ID_NONE || route_id > e->route_count) {
        return RW_ERR_NO_ROUTE;
    }

    for (r = 0; r < e->route_count; ++r) {
        if (e->routes[r].id == route_id) {
            route = &e->routes[r];
            break;
        }
    }
    if (route == NULL) {
        return RW_ERR_NO_ROUTE;
    }

    /* Authority is only ever granted for a route the interlocking has set.
     * That is INV-8 and it is checked here, not assumed. */
    if (route->state != ROUTE_LOCKED && route->state != ROUTE_OCCUPIED) {
        rw_event(SEVERITY_WARNING, "TRAIN",
                 "Train %s was refused authority over route %s - route is not set",
                 train->headcode, route->name);
        return RW_ERR_INTERLOCK;
    }
    if (e->emergency) {
        return RW_ERR_EMERGENCY;
    }

    train->route = route_id;
    train->destination_track = route->steps[route->step_count - 1].track;
    train->held = false;
    if (train->state != TRAIN_RUNNING) {
        train->state = TRAIN_RUNNING;
    }

    rw_event(SEVERITY_INFO, "TRAIN",
             "Train %s given movement authority over route %s",
             train->headcode, route->name);
    return RW_OK;
}

RwResult train_controller_hold(RwId train_id, bool hold)
{
    TrainStateInternal *train = train_by_id(train_id);

    if (train == NULL) {
        return RW_ERR_INVALID_ID;
    }
    train->held = hold;
    if (hold) {
        train->target_speed_kph = 0.0f;
        rw_event(SEVERITY_INFO, "TRAIN", "Train %s held", train->headcode);
    } else {
        rw_event(SEVERITY_INFO, "TRAIN", "Train %s released from hold", train->headcode);
    }
    return RW_OK;
}

RwResult train_controller_get_state(RwId train_id, TrainState *state)
{
    const TrainStateInternal *train = train_by_id(train_id);

    if (state == NULL) {
        return RW_ERR_INVALID_ARG;
    }
    if (train == NULL) {
        return RW_ERR_INVALID_ID;
    }
    *state = train->state;
    return RW_OK;
}

int train_controller_count(void)
{
    return (int)rw_engine()->train_count;
}

RwResult train_controller_get_info(RwId train_id, RwTrainInfo *info)
{
    const TrainStateInternal *train = train_by_id(train_id);

    if (info == NULL) {
        return RW_ERR_INVALID_ARG;
    }
    if (train == NULL) {
        return RW_ERR_INVALID_ID;
    }
    memset(info, 0, sizeof(*info));
    info->id = train->id;
    snprintf(info->name, sizeof(info->name), "%s", train->headcode);
    info->state = train->state;
    info->speed_kph = train->speed_kph;
    info->max_speed_kph = train->max_speed_kph;
    info->track = train->track;
    info->route = train->route;
    info->carriages = train->carriages;
    info->emergency_brake = train->emergency_brake;
    info->latitude = train->latitude;
    info->longitude = train->longitude;
    info->delay_seconds = train->delay_seconds;
    return RW_OK;
}

RwResult train_controller_get_at(int index, RwTrainInfo *info)
{
    if (info == NULL) {
        return RW_ERR_INVALID_ARG;
    }
    if (index < 0 || (size_t)index >= rw_engine()->train_count) {
        return RW_ERR_INVALID_ID;
    }
    return train_controller_get_info(rw_engine()->trains[index].id, info);
}

RwId train_controller_waiting_for_route(RwId route_id)
{
    RailwayEngine *e = rw_engine();
    RouteStateInternal *route = NULL;
    size_t r;
    size_t i;

    for (r = 0; r < e->route_count; ++r) {
        if (e->routes[r].id == route_id) {
            route = &e->routes[r];
            break;
        }
    }
    if (route == NULL || route->step_count == 0) {
        return RW_ID_NONE;
    }

    /* The first section of a route is where a train must be waiting. */
    for (i = 0; i < e->train_count; ++i) {
        TrainStateInternal *train = &e->trains[i];

        if (train->track != route->steps[0].track) {
            continue;
        }
        if (train->emergency_brake) {
            continue;
        }
        return train->id;
    }
    return RW_ID_NONE;
}

/* --------------------------------------------------------------------------
 * Emergency
 * -------------------------------------------------------------------------- */
void rw_train_apply_emergency(TrainStateInternal *train)
{
    if (train == NULL) {
        return;
    }
    train->emergency_brake = true;
    train->state = TRAIN_EMERGENCY_STOP;
    train->target_speed_kph = 0.0f;
}

int train_controller_emergency_all(const char *reason)
{
    RailwayEngine *e = rw_engine();
    size_t i;
    int affected = 0;

    for (i = 0; i < e->train_count; ++i) {
        TrainStateInternal *train = &e->trains[i];

        if (train->speed_kph > 0.1f || train->state == TRAIN_RUNNING) {
            affected++;
        }
        rw_train_apply_emergency(train);
    }

    rw_event(SEVERITY_CRITICAL, "TRAIN",
             "Emergency brake applied to all %u train(s): %s",
             (unsigned)e->train_count, reason != NULL ? reason : "emergency stop");
    return affected;
}

bool train_controller_all_stopped(void)
{
    RailwayEngine *e = rw_engine();
    size_t i;

    for (i = 0; i < e->train_count; ++i) {
        if (e->trains[i].speed_kph > RW_WALKING_PACE_KPH) {
            return false;
        }
    }
    return true;
}

int train_controller_occupancy_count(void)
{
    RailwayEngine *e = rw_engine();
    size_t i;
    int count = 0;

    for (i = 0; i < e->track_count; ++i) {
        if (e->tracks[i].occupied_by != RW_ID_NONE) {
            count++;
        }
    }
    return count;
}

/* --------------------------------------------------------------------------
 * Per-tick integration
 * -------------------------------------------------------------------------- */
void train_controller_tick(unsigned delta_ms)
{
    RailwayEngine *e = rw_engine();
    const float seconds = (float)delta_ms / 1000.0f;
    size_t i;

    for (i = 0; i < e->train_count; ++i) {
        TrainStateInternal *train = &e->trains[i];
        TrackState *track = track_of(train);
        RwId blocking = RW_ID_NONE;
        float permitted;
        float delta;

        if (track == NULL) {
            continue;
        }

        /* A train anywhere on the network during an emergency is emergency
         * braked every tick, so a script or terminal command cannot restart
         * one by accident (INV-6). */
        if (e->emergency) {
            train->emergency_brake = true;
            train->target_speed_kph = 0.0f;
        }

        permitted = permitted_speed(train, &blocking);
        train->blocked_by_signal = blocking;

        if (train->emergency_brake) {
            /* Emergency braking: the fastest available deceleration. */
            delta = RW_EMERGENCY_BRAKE_RATE * seconds;
            train->speed_kph -= delta;
            if (train->speed_kph <= 0.0f) {
                train->speed_kph = 0.0f;
                if (train->state != TRAIN_EMERGENCY_STOP) {
                    train->state = TRAIN_STOPPED;
                }
            } else {
                train->state = TRAIN_EMERGENCY_STOP;
            }
        } else if (train->speed_kph > permitted) {
            /* Service braking down to the permitted speed. */
            delta = RW_SERVICE_BRAKE_RATE * seconds;
            train->speed_kph -= delta;
            if (train->speed_kph < permitted) {
                train->speed_kph = permitted;
            }
        } else if (train->speed_kph < permitted) {
            /* Accelerating up to the permitted speed. */
            delta = RW_ACCELERATION_RATE * seconds;
            train->speed_kph += delta;
            if (train->speed_kph > permitted) {
                train->speed_kph = permitted;
            }
        }

        /* State bookkeeping. */
        if (train->speed_kph <= RW_WALKING_PACE_KPH && permitted <= 0.0f) {
            /* Stopped, and nothing is asking it to move. */
            if (train->state == TRAIN_RUNNING) {
                train->state = train->held ? TRAIN_STOPPED : TRAIN_STOPPED;
            }
            if (train->stopped_since_ms == 0) {
                train->stopped_since_ms = (unsigned)e->clock_ms;
            } else {
                train->delay_seconds = ((unsigned)e->clock_ms - train->stopped_since_ms) / 1000u;
            }
            continue;
        }

        if (train->speed_kph > RW_WALKING_PACE_KPH) {
            train->stopped_since_ms = 0;
            if (!train->emergency_brake) {
                train->state = TRAIN_RUNNING;
            }
        }

        /* Integrate position. Speed is km/h, time is seconds, so metres per
         * second is speed / 3.6. */
        {
            const float metres = (train->speed_kph / 3.6f) * seconds;
            const float previous = train->position_m;
            train->position_m += metres;

            if (train->position_m >= track->length_m) {
                const float overshoot = train->position_m - track->length_m;
                if (!advance_to_next_track(train, overshoot)) {
                    /* No onward movement: stop at the section boundary. */
                    train->position_m = track->length_m - 0.5f;
                    if (train->position_m < 0.0f) {
                        train->position_m = 0.0f;
                    }
                    train->speed_kph = 0.0f;
                    if (train->state == TRAIN_RUNNING) {
                        train->state = TRAIN_STOPPED;
                    }
                }
            }
            (void)previous;
            track = track_of(train);
            update_diagram_position(train, track);
        }

        /* Occupancy follows the train every tick, so a train that appears by
         * any path (script, terminal, restore) is always accounted for. */
        rw_track_set_occupancy(train->track, TRACK_OCCUPIED, train->id);
    }
}

void rw_trains_tick(unsigned delta_ms)
{
    train_controller_tick(delta_ms);
}
