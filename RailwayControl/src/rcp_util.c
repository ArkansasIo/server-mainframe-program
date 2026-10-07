/* ==========================================================================
 * rcp_util.c - Small shared helpers: identifiers, lookups, formatting.
 * ========================================================================== */
#include "railway_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* --------------------------------------------------------------------------
 * Timestamps
 * -------------------------------------------------------------------------- */
const char *rw_now_string(char *buffer, size_t size)
{
    time_t now = time(NULL);
    struct tm tm_buf;
#if defined(_WIN32)
    if (gmtime_s(&tm_buf, &now) != 0)
    {
        snprintf(buffer, size, "0000-00-00 00:00:00");
        return buffer;
    }
#else
    if (gmtime_r(&now, &tm_buf) == NULL)
    {
        snprintf(buffer, size, "0000-00-00 00:00:00");
        return buffer;
    }
#endif
    snprintf(buffer, size, "%04d-%02d-%02d %02d:%02d:%02d",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return buffer;
}

/* --------------------------------------------------------------------------
 * Result code text
 * -------------------------------------------------------------------------- */
const char *rw_result_string(RwResult result)
{
    switch (result)
    {
    case RW_OK:
        return "OK";
    case RW_ERR_INVALID_ID:
        return "INVALID-ID";
    case RW_ERR_INVALID_ARG:
        return "INVALID-ARGUMENT";
    case RW_ERR_NOT_INITIALIZED:
        return "NOT-INITIALIZED";
    case RW_ERR_INTERLOCK:
        return "INTERLOCK-REFUSED";
    case RW_ERR_CONFLICT:
        return "ROUTE-CONFLICT";
    case RW_ERR_OCCUPIED:
        return "TRACK-OCCUPIED";
    case RW_ERR_POINT_LOCKED:
        return "POINT-LOCKED";
    case RW_ERR_POINT_MOVING:
        return "POINT-MOVING";
    case RW_ERR_EMERGENCY:
        return "EMERGENCY-STOP";
    case RW_ERR_OUT_OF_SERVICE:
        return "OUT-OF-SERVICE";
    case RW_ERR_BUSY:
        return "BUSY";
    case RW_ERR_NO_ROUTE:
        return "NO-ROUTE";
    case RW_ERR_ROUTE_SET:
        return "ROUTE-ALREADY-SET";
    case RW_ERR_RANGE:           return "OUT-OF-RANGE";
    case RW_ERR_INTERNAL:        return "INTERNAL-ERROR";
    case RW_ERR_IO:              return "IO-ERROR";
    }
    return "UNKNOWN";
}

const char *rw_result_hint(RwResult result)
{
    switch (result)
    {
    case RW_OK:
        return "Accepted.";
    case RW_ERR_INVALID_ID:
        return "Unknown asset id - check the number.";
    case RW_ERR_INVALID_ARG:
        return "Bad argument.";
    case RW_ERR_NOT_INITIALIZED:
        return "Engine not started.";
    case RW_ERR_INTERLOCK:
        return "Interlocking proof failed - see the event log.";
    case RW_ERR_CONFLICT:
        return "Another route already uses part of this path.";
    case RW_ERR_OCCUPIED:
        return "A train or failed circuit is in the path.";
    case RW_ERR_POINT_LOCKED:
        return "Release the route holding that point first.";
    case RW_ERR_POINT_MOVING:
        return "Point is still in transit - wait.";
    case RW_ERR_EMERGENCY:
        return "Emergency stop is active. Clear it first.";
    case RW_ERR_OUT_OF_SERVICE:
        return "Asset is out of service.";
    case RW_ERR_BUSY:
        return "Resource busy - retry shortly.";
    case RW_ERR_NO_ROUTE:
        return "Set the route before giving authority.";
    case RW_ERR_ROUTE_SET:
        return "Route is already set.";
    case RW_ERR_RANGE:           return "Value outside the permitted range.";
    case RW_ERR_INTERNAL:        return "Internal engine fault - check the event log.";
    case RW_ERR_IO:              return "File could not be read or written.";
    }
    return "";
}

const char *rw_safety_name(SystemState state)
{
    switch (state)
    {
    case SYSTEM_INITIALISING:
        return "INITIALISING";
    case SYSTEM_ONLINE:
        return "ONLINE";
    case SYSTEM_DEGRADED:
        return "DEGRADED";
    case SYSTEM_EMERGENCY:
        return "EMERGENCY";
    case SYSTEM_SHUTDOWN:
        return "SHUTDOWN";
    }
    return "UNKNOWN";
}

/* --------------------------------------------------------------------------
 * Name lookups
 *
 * Each asset type keeps its own table, so the lookup walks the engine store
 * directly rather than through a generic stride walk - it is clearer and the
 * engine is O(30) elements at most.
 * -------------------------------------------------------------------------- */
RwId rw_signal_find(const char *name)
{
    /* Exact match first, then the resolver. The engine's own arrays are the
     * authority; the resolver adds case-insensitivity, short names and
     * aliases on top without ever overriding an exact hit. */
    RailwayEngine *e = rw_engine();
    size_t i;

    if (e == NULL || name == NULL)
    {
        return RW_ID_NONE;
    }
    for (i = 0; i < e->signal_count; ++i)
    {
        if (strcmp(e->signals[i].name, name) == 0)
        {
            return e->signals[i].id;
        }
    }
    return rc_name_resolve_id(RC_NAME_KIND_SIGNAL, name);
}

RwId rw_track_find(const char *name)
{
    RailwayEngine *e = rw_engine();
    size_t i;

    if (e == NULL || name == NULL)
    {
        return RW_ID_NONE;
    }
    for (i = 0; i < e->track_count; ++i)
    {
        if (strcmp(e->tracks[i].name, name) == 0)
        {
            return e->tracks[i].id;
        }
    }
    return rc_name_resolve_id(RC_NAME_KIND_TRACK, name);
}

RwId rw_point_find(const char *name)
{
    RailwayEngine *e = rw_engine();
    size_t i;

    if (e == NULL || name == NULL)
    {
        return RW_ID_NONE;
    }
    for (i = 0; i < e->point_count; ++i)
    {
        if (strcmp(e->points[i].name, name) == 0)
        {
            return e->points[i].id;
        }
    }
    return rc_name_resolve_id(RC_NAME_KIND_POINT, name);
}

RwId rw_route_find(const char *name)
{
    return interlocking_find_route(name);
}

RwId rw_train_find(const char *name)
{
    RailwayEngine *e = rw_engine();
    size_t i;

    if (e == NULL || name == NULL)
    {
        return RW_ID_NONE;
    }
    for (i = 0; i < e->train_count; ++i)
    {
        if (strcmp(e->trains[i].headcode, name) == 0)
        {
            return e->trains[i].id;
        }
    }
    return rc_name_resolve_id(RC_NAME_KIND_TRAIN, name);
}

/* --------------------------------------------------------------------------
 * Enum to text (used by the GUI and the event log)
 * -------------------------------------------------------------------------- */
const char *signal_state_name(SignalState state)
{
    switch (state)
    {
    case SIGNAL_RED:
        return "RED";
    case SIGNAL_YELLOW:
        return "YELLOW";
    case SIGNAL_GREEN:
        return "GREEN";
    case SIGNAL_CALLON:
        return "CALL-ON";
    }
    return "?";
}

const char *switch_position_name(SwitchPosition position)
{
    switch (position)
    {
    case SWITCH_MAIN:
        return "MAIN";
    case SWITCH_DIVERGING:
        return "DIVERGING";
    }
    return "?";
}

const char *train_state_name(TrainState state)
{
    switch (state)
    {
    case TRAIN_STOPPED:
        return "STOPPED";
    case TRAIN_RUNNING:
        return "RUNNING";
    case TRAIN_EMERGENCY_STOP:
        return "EMERGENCY-STOP";
    }
    return "?";
}

const char *track_occupancy_name(TrackOccupancy occupancy)
{
    switch (occupancy)
    {
    case TRACK_CLEAR:
        return "CLEAR";
    case TRACK_OCCUPIED:
        return "OCCUPIED";
    case TRACK_UNKNOWN:
        return "UNKNOWN";
    }
    return "?";
}

const char *route_state_name(RouteState state)
{
    switch (state)
    {
    case ROUTE_IDLE:
        return "IDLE";
    case ROUTE_REQUESTED:
        return "REQUESTED";
    case ROUTE_LOCKED:
        return "LOCKED";
    case ROUTE_OCCUPIED:
        return "OCCUPIED";
    case ROUTE_RELEASED:
        return "RELEASED";
    }
    return "?";
}

const char *severity_name(RwSeverity severity)
{
    switch (severity)
    {
    case SEVERITY_INFO:
        return "INFO";
    case SEVERITY_WARNING:
        return "WARNING";
    case SEVERITY_ALARM:
        return "ALARM";
    case SEVERITY_CRITICAL:
        return "CRITICAL";
    }
    return "INFO";
}
