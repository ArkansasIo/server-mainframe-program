/* ==========================================================================
 * rcp_names.c - Enum-to-text helpers shared by every front end.
 * ========================================================================== */
#include "railway_internal.h"

#include <string.h>

const char *rcp_safety_name(SystemState state)
{
    return rw_safety_name(state);
}

char rcp_aspect_char(SignalState aspect)
{
    switch (aspect)
    {
    case SIGNAL_RED:
        return 'R';
    case SIGNAL_YELLOW:
        return 'Y';
    case SIGNAL_GREEN:
        return 'G';
    case SIGNAL_CALLON:
        return 'C';
    }
    return '?';
}

const char *rcp_sensor_kind_name(RwSensorKind kind)
{
    switch (kind)
    {
    case SENSOR_TRACK_CIRCUIT:
        return "TRACK-CIRCUIT";
    case SENSOR_AXLE_COUNTER:
        return "AXLE-COUNTER";
    case SENSOR_TREADLE:
        return "TREADLE";
    case SENSOR_HOT_BOX:
        return "HOT-BOX";
    case SENSOR_LEVEL_CROSSING:
        return "LEVEL-CROSSING";
    }
    return "SENSOR";
}

const char *rcp_sensor_state_name(RwSensorState state)
{
    switch (state)
    {
    case SENSOR_OK:
        return "OK";
    case SENSOR_TRIGGERED:
        return "TRIGGERED";
    case SENSOR_FAILED:
        return "FAILED";
    }
    return "?";
}

const char *rcp_train_class_name(int cls)
{
    switch ((TrainClass)cls)
    {
    case TRAIN_CLASS_PASSENGER:
        return "PASSENGER";
    case TRAIN_CLASS_FREIGHT:
        return "FREIGHT";
    case TRAIN_CLASS_ENGINEERING:
        return "ENGINEERING";
    case TRAIN_CLASS_LIGHT_ENGINE:
        return "LIGHT-ENGINE";
    }
    return "TRAIN";
}

/* --------------------------------------------------------------------------
 * Terminal status labels. Kept here rather than in terminal.c so that batch
 * mode and the GUI share one definition.
 * -------------------------------------------------------------------------- */
const char *rw_result_string_short(int terminal_status)
{
    /* Values mirror the TerminalStatus enum. Anything above TERM_QUIT is a
     * refusal of some kind and is reported as a failure. */
    switch (terminal_status) {
    case 0: return "OK";
    case 1: return "SYNTAX-ERROR";
    case 2: return "UNKNOWN-COMMAND";
    case 3: return "REFUSED";
    case 4: return "BAD-ARGUMENT";
    case 5: return "IO-ERROR";
    case 6: return "QUIT";
    default: return "FAILED";
    }
}
