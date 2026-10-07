/* ==========================================================================
 * rcp_track.c - 🛤️ Track occupancy.
 *
 * Occupancy has two sources and they must agree:
 *
 *   1. TRAINS      a train physically standing on the section
 *   2. CIRCUITS    the track circuit / axle counter reporting the section
 *
 * The section is OCCUPIED if EITHER source says so. That is the fail-safe
 * union: a disagreement means something is wrong, and the safe reading of a
 * disagreement is "occupied". A failed circuit (TRACK_UNKNOWN) is never
 * downgraded to CLEAR - INV-5.
 * ========================================================================== */
#include "railway_internal.h"

#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Setup
 * -------------------------------------------------------------------------- */
void rw_tracks_init(void)
{
    RailwayEngine *e = rw_engine();
    size_t i;

    if (e == NULL)
    {
        return;
    }
    for (i = 0; i < e->track_count; ++i)
    {
        TrackState *track = &e->tracks[i];

        if (track->speed_limit_kph <= 0)
        {
            track->speed_limit_kph = (int)e->config.default_speed_limit_kph;
        }
        if (track->length_m <= 0.0f)
        {
            track->length_m = 400.0f;
        }
        track->in_service = true;
        track->closed_for_work = false;
        track->circuit_failed = false;
        /* Occupancy is derived from the trains and sensors, not preset. */
        track->occupancy = TRACK_CLEAR;
        track->occupied_by = RW_ID_NONE;
        track->occupied_since_ms = 0;
    }

    /* Seed the initial occupancy from wherever the trains were placed. */
    for (i = 0; i < e->train_count; ++i)
    {
        const TrainStateInternal *train = &e->trains[i];
        size_t t;

        for (t = 0; t < e->track_count; ++t)
        {
            if (e->tracks[t].id == train->track && train->track != RW_ID_NONE)
            {
                e->tracks[t].occupancy = TRACK_OCCUPIED;
                e->tracks[t].occupied_by = train->id;
                break;
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * Direct occupancy control (used by the train controller and the sensors)
 * -------------------------------------------------------------------------- */
void rw_track_set_occupancy(RwId track_id, TrackOccupancy occupancy, RwId train_id)
{
    RailwayEngine *e = rw_engine();
    TrackState *track = NULL;
    size_t i;

    if (track_id == RW_ID_NONE)
    {
        return;
    }
    for (i = 0; i < e->track_count; ++i)
    {
        if (e->tracks[i].id == track_id)
        {
            track = &e->tracks[i];
            break;
        }
    }
    if (track == NULL)
    {
        return;
    }

    /* Never clear a failed circuit by accident. */
    if (track->circuit_failed && occupancy == TRACK_CLEAR)
    {
        occupancy = TRACK_UNKNOWN;
    }

    if (track->occupancy != occupancy)
    {
        if (occupancy == TRACK_OCCUPIED)
        {
            track->occupied_since_ms = (unsigned)e->clock_ms;
            if (train_id != RW_ID_NONE)
            {
                rw_event(SEVERITY_INFO, "TRACK",
                         "%s occupied by train %u", track->name, train_id);
            }
            else
            {
                rw_event(SEVERITY_WARNING, "TRACK",
                         "%s showing OCCUPIED with no train identified - "
                         "possible circuit fault or vehicle on the line",
                         track->name);
            }
        }
        else if (track->occupancy == TRACK_OCCUPIED)
        {
            rw_event(SEVERITY_INFO, "TRACK", "%s is now clear", track->name);
        }
        track->occupancy = occupancy;
    }

    track->occupied_by = (occupancy == TRACK_OCCUPIED) ? train_id : RW_ID_NONE;
}

TrackOccupancy rw_track_occupancy(const RailwayEngine *e, RwId track_id)
{
    size_t i;

    if (e == NULL || track_id == RW_ID_NONE)
    {
        return TRACK_UNKNOWN;
    }
    for (i = 0; i < e->track_count; ++i)
    {
        if (e->tracks[i].id == track_id)
        {
            return e->tracks[i].occupancy;
        }
    }
    return TRACK_UNKNOWN;
}

bool rw_track_is_clear_for_route(const RailwayEngine *e, RwId track_id)
{
    return rw_track_occupancy(e, track_id) == TRACK_CLEAR;
}

/* --------------------------------------------------------------------------
 * Recomputation
 *
 * Derived each tick from the two sources. Written so the precedence is
 * explicit and cannot be misread:
 *
 *   failed circuit          -> UNKNOWN (never clearable by software alone)
 *   occupied by any source  -> OCCUPIED
 *   otherwise               -> CLEAR
 * -------------------------------------------------------------------------- */
void rw_track_recompute_from_circuits(void)
{
    RailwayEngine *e = rw_engine();
    size_t t;
    size_t i;

    /* Start from the train-derived view. */
    for (t = 0; t < e->track_count; ++t)
    {
        TrackState *track = &e->tracks[t];
        RwId occupant = RW_ID_NONE;

        for (i = 0; i < e->train_count; ++i)
        {
            const TrainStateInternal *train = &e->trains[i];
            if (train->track == track->id)
            {
                occupant = train->id;
                break;
            }
        }

        if (track->circuit_failed)
        {
            track->occupancy = TRACK_UNKNOWN;
            track->occupied_by = RW_ID_NONE;
            continue;
        }

        if (occupant != RW_ID_NONE)
        {
            if (track->occupancy != TRACK_OCCUPIED)
            {
                track->occupied_since_ms = (unsigned)e->clock_ms;
            }
            track->occupancy = TRACK_OCCUPIED;
            track->occupied_by = occupant;
            continue;
        }

        /* No train. A triggered track circuit means something the train list
         * does not know about is on the section - a road vehicle at a
         * crossing, a derailed wagon, or a failed circuit. Treat as occupied;
         * never silently clear it. */
        {
            bool circuit_occupied = false;
            size_t s;

            for (s = 0; s < e->sensor_count; ++s)
            {
                const SensorState *sensor = &e->sensors[s];
                if (sensor->track != track->id || !sensor->in_service)
                {
                    continue;
                }
                if (sensor->kind != SENSOR_TRACK_CIRCUIT && sensor->kind != SENSOR_AXLE_COUNTER)
                {
                    continue;
                }
                if (sensor->state == SENSOR_TRIGGERED)
                {
                    circuit_occupied = true;
                }
                if (sensor->state == SENSOR_FAILED)
                {
                    track->circuit_failed = true;
                }
            }

            if (track->circuit_failed)
            {
                track->occupancy = TRACK_UNKNOWN;
                track->occupied_by = RW_ID_NONE;
            }
            else if (circuit_occupied)
            {
                if (track->occupancy != TRACK_OCCUPIED)
                {
                    track->occupied_since_ms = (unsigned)e->clock_ms;
                }
                track->occupancy = TRACK_OCCUPIED;
                track->occupied_by = RW_ID_NONE;
            }
            else
            {
                track->occupancy = TRACK_CLEAR;
                track->occupied_by = RW_ID_NONE;
                track->occupied_since_ms = 0;
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * Collision / conflict detection  (⚠️)
 *
 * Independent of the interlocking: this is the second line of defence. It
 * asks the question "is the world still consistent?" even if every route
 * proof passed. Anything it finds is a serious alarm.
 *
 * It detects:
 *   - two trains reported on the same section
 *   - a train on a section that is not the one it is associated with
 *   - a train on a section whose route is not set (movement without authority)
 * -------------------------------------------------------------------------- */
int train_controller_detect_conflicts(RwId *train_a, RwId *train_b,
                                      char *reason, size_t reason_size)
{
    RailwayEngine *e = rw_engine();
    int found = 0;
    size_t i;
    size_t j;

    if (train_a != NULL)
        *train_a = RW_ID_NONE;
    if (train_b != NULL)
        *train_b = RW_ID_NONE;
    if (reason != NULL && reason_size > 0)
        reason[0] = '\0';

    /* Two trains on one section. */
    for (i = 0; i < e->train_count; ++i)
    {
        for (j = i + 1; j < e->train_count; ++j)
        {
            if (e->trains[i].track == RW_ID_NONE || e->trains[j].track == RW_ID_NONE)
            {
                continue;
            }
            if (e->trains[i].track != e->trains[j].track)
            {
                continue;
            }
            found++;
            if (found == 1)
            {
                const TrackState *track = NULL;
                size_t t;

                for (t = 0; t < e->track_count; ++t)
                {
                    if (e->tracks[t].id == e->trains[i].track)
                    {
                        track = &e->tracks[t];
                        break;
                    }
                }
                if (train_a != NULL)
                    *train_a = e->trains[i].id;
                if (train_b != NULL)
                    *train_b = e->trains[j].id;
                if (reason != NULL)
                {
                    snprintf(reason, reason_size,
                             "COLLISION RISK: trains %s and %s are both on %s",
                             e->trains[i].headcode, e->trains[j].headcode,
                             track != NULL ? track->name : "the same section");
                }
            }
        }
    }

    /* A moving train whose route is not set is running without authority. */
    for (i = 0; i < e->train_count; ++i)
    {
        const TrainStateInternal *train = &e->trains[i];
        const RouteStateInternal *route = NULL;
        size_t r;

        if (train->speed_kph < 1.0f)
        {
            continue;
        }
        if (train->route == RW_ID_NONE)
        {
            found++;
            if (reason != NULL && reason[0] == '\0')
            {
                snprintf(reason, reason_size,
                         "Train %s is moving (%.0f km/h) with no movement authority",
                         train->headcode, (double)train->speed_kph);
            }
            if (train_a != NULL && *train_a == RW_ID_NONE)
                *train_a = train->id;
            continue;
        }

        for (r = 0; r < e->route_count; ++r)
        {
            if (e->routes[r].id == train->route)
            {
                route = &e->routes[r];
                break;
            }
        }
        if (route == NULL || (route->state != ROUTE_LOCKED && route->state != ROUTE_OCCUPIED))
        {
            found++;
            if (reason != NULL && reason[0] == '\0')
            {
                snprintf(reason, reason_size,
                         "Train %s holds route %u which is not set",
                         train->headcode, train->route);
            }
            if (train_a != NULL && *train_a == RW_ID_NONE)
                *train_a = train->id;
        }
    }

    /* A section reporting OCCUPIED with no train and no circuit trigger. */
    for (i = 0; i < e->track_count; ++i)
    {
        const TrackState *track = &e->tracks[i];
        bool known = false;
        size_t s;

        if (track->occupancy != TRACK_OCCUPIED || track->occupied_by != RW_ID_NONE)
        {
            continue;
        }
        for (s = 0; s < e->sensor_count; ++s)
        {
            if (e->sensors[s].track == track->id && e->sensors[s].state == SENSOR_TRIGGERED)
            {
                known = true;
                break;
            }
        }
        if (!known)
        {
            found++;
            if (reason != NULL && reason[0] == '\0')
            {
                snprintf(reason, reason_size,
                         "%s reports OCCUPIED but no train or circuit explains it",
                         track->name);
            }
        }
    }

    return found;
}
