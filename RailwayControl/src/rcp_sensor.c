/* ==========================================================================
 * rcp_sensor.c - 📡 Sensor inputs (SCADA field interface).
 *
 * In a real installation this file would sit behind a field bus: the sensors
 * would be read from the interlocking's own safe transmission system
 * (EN 50159) with sequence numbers and timeouts, and any loss of input would
 * degrade the section to UNKNOWN.
 *
 * The simulated equivalent is here: inject_sensor() is the entry point a
 * field interface, a test harness or the operator would use.
 *
 * Failure handling is the interesting part:
 *   - a FAILED sensor makes its track circuit UNKNOWN, which blocks routes
 *   - an axle counter whose count does not return to zero keeps the section
 *     occupied - never silently cleared
 *   - a hot box alarm is latched until acknowledged
 * ========================================================================== */
#include "railway_internal.h"

#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Setup: give every non-circuit sensor a sensible starting value.
 * -------------------------------------------------------------------------- */
void rw_sensors_init(void)
{
    RailwayEngine *e = rw_engine();
    size_t i;

    if (e == NULL) {
        return;
    }
    for (i = 0; i < e->sensor_count; ++i) {
        SensorState *sensor = &e->sensors[i];

        sensor->in_service = true;
        sensor->last_change_ms = (unsigned)e->clock_ms;

        switch (sensor->kind) {
        case SENSOR_TRACK_CIRCUIT:
        case SENSOR_TREADLE:
            sensor->state = SENSOR_OK;
            sensor->value = 0;
            break;
        case SENSOR_AXLE_COUNTER:
            sensor->state = SENSOR_OK;
            sensor->value = 0;      /* net axle count in the section */
            break;
        case SENSOR_HOT_BOX:
            sensor->state = SENSOR_OK;
            sensor->temperature_c = 32.0f;   /* ambient running temperature */
            break;
        case SENSOR_LEVEL_CROSSING:
            sensor->state = SENSOR_OK;
            sensor->value = 0;      /* 0 barriers up, 1 barriers down */
            break;
        }
    }
}

static SensorState *sensor_by_id(RwId sensor_id)
{
    RailwayEngine *e = rw_engine();

    if (sensor_id == RW_ID_NONE || sensor_id > e->sensor_count) {
        return NULL;
    }
    return &e->sensors[sensor_id - 1];
}

/* --------------------------------------------------------------------------
 * Apply a reading
 * -------------------------------------------------------------------------- */
RwResult rw_sensor_apply(RwId sensor_id, int value, int state)
{
    RailwayEngine *e = rw_engine();
    SensorState *sensor = sensor_by_id(sensor_id);
    RwSensorState new_state;

    if (sensor == NULL) {
        return RW_ERR_INVALID_ID;
    }
    if (state < SENSOR_OK || state > SENSOR_FAILED) {
        return RW_ERR_INVALID_ARG;
    }
    if (!sensor->in_service) {
        return RW_ERR_OUT_OF_SERVICE;
    }

    new_state = (RwSensorState)state;
    sensor->value = value;
    sensor->last_change_ms = (unsigned)e->clock_ms;

    if (sensor->state != new_state) {
        sensor->state = new_state;

        switch (new_state) {
        case SENSOR_FAILED:
            rw_event(SEVERITY_ALARM, "SENSOR",
                     "%s FAILED - loss of input, section treated as UNSAFE",
                     sensor->name);
            break;
        case SENSOR_TRIGGERED:
            rw_event(SEVERITY_INFO, "SENSOR", "%s triggered (value %d)",
                     sensor->name, value);
            break;
        case SENSOR_OK:
            rw_event(SEVERITY_INFO, "SENSOR", "%s restored to normal", sensor->name);
            break;
        }
    }

    /* Now propagate the reading into the track circuit. */
    if (sensor->track != RW_ID_NONE && sensor->track <= e->track_count) {
        TrackState *track = &e->tracks[sensor->track - 1];

        switch (sensor->kind) {
        case SENSOR_TRACK_CIRCUIT:
            if (new_state == SENSOR_FAILED) {
                track->circuit_failed = true;
                track->occupancy = TRACK_UNKNOWN;
            } else {
                track->circuit_failed = false;
                if (new_state == SENSOR_TRIGGERED) {
                    rw_track_set_occupancy(track->id, TRACK_OCCUPIED, RW_ID_NONE);
                } else if (track->occupied_by == RW_ID_NONE) {
                    /* Only clear when no train is standing on it. */
                    rw_track_set_occupancy(track->id, TRACK_CLEAR, RW_ID_NONE);
                }
            }
            break;

        case SENSOR_AXLE_COUNTER:
            /* Net axles in the section. Zero means nothing is there. */
            if (new_state == SENSOR_FAILED) {
                track->circuit_failed = true;
                track->occupancy = TRACK_UNKNOWN;
                rw_event(SEVERITY_ALARM, "SENSOR",
                         "%s failed - axle count cannot be trusted for %s",
                         sensor->name, track->name);
            } else if (value != 0) {
                rw_track_set_occupancy(track->id, TRACK_OCCUPIED, RW_ID_NONE);
            }
            break;

        case SENSOR_HOT_BOX:
            /* A hot box is an alarm, not an occupancy. It is latched so the
             * signaller has to acknowledge it. */
            sensor->temperature_c = (float)value;
            if (value >= 90) {
                sensor->state = SENSOR_TRIGGERED;
                rw_event(SEVERITY_ALARM, "SENSOR",
                         "%s HOT BOX ALARM: %d C on %s - stop and examine",
                         sensor->name, value, track->name);
            }
            break;

        case SENSOR_TREADLE:
            /* Momentary: a wheel passed. Counts traffic, does not hold
             * occupancy by itself. */
            if (new_state == SENSOR_TRIGGERED) {
                sensor->value = value > 0 ? value : sensor->value + 1;
            }
            break;

        case SENSOR_LEVEL_CROSSING:
            if (value != 0) {
                rw_event(SEVERITY_WARNING, "SENSOR",
                         "%s barriers DOWN across %s - road traffic stopped",
                         sensor->name, track->name);
            } else {
                rw_event(SEVERITY_INFO, "SENSOR",
                         "%s barriers UP - road reopened", sensor->name);
            }
            break;
        }
    } else if (sensor->point != RW_ID_NONE) {
        /* Point detection input. A failed contact means the point can no
         * longer prove its lie, so it must be withdrawn (INV-3). */
        if (new_state == SENSOR_FAILED && sensor->point <= e->point_count) {
            PointState *point = &e->points[sensor->point - 1];
            point->fault = true;
            rw_event(SEVERITY_ALARM, "SENSOR",
                     "%s failed - point %s detection lost, point withdrawn",
                     sensor->name, point->name);
        }
    }

    return RW_OK;
}

void rw_sensor_fail(RwId sensor_id)
{
    (void)rw_sensor_apply(sensor_id, 0, SENSOR_FAILED);
}

/* --------------------------------------------------------------------------
 * Per-tick: age out momentary and latched inputs.
 * -------------------------------------------------------------------------- */
void rw_sensors_tick(unsigned delta_ms)
{
    RailwayEngine *e = rw_engine();
    size_t i;

    for (i = 0; i < e->sensor_count; ++i) {
        SensorState *sensor = &e->sensors[i];

        if (!sensor->in_service) {
            continue;
        }

        /* A treadle reading is consumed after two seconds. */
        if (sensor->kind == SENSOR_TREADLE && sensor->state == SENSOR_TRIGGERED) {
            if (e->clock_ms - sensor->last_change_ms > 2000) {
                sensor->state = SENSOR_OK;
            }
        }

        /* A hot box cools once the train has passed. */
        if (sensor->kind == SENSOR_HOT_BOX && sensor->state == SENSOR_TRIGGERED) {
            sensor->temperature_c -= (float)delta_ms / 1000.0f * 0.5f;
            if (sensor->temperature_c < 60.0f) {
                sensor->temperature_c = 60.0f;
            }
        }
    }
}
