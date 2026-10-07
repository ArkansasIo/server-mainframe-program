/* ==========================================================================
 * railway_types.h - Shared data model for the Railway Control Center.
 *
 * This header is GUI-agnostic and dependency-free. Everything that both the
 * engine and any front end (Win32 GUI, console, network server, simulator)
 * needs to agree on lives here.
 *
 * SAFETY NOTICE
 * -------------
 * This is a SIMULATOR / PROTOTYPE. It is not a certified interlocking and
 * must never be used to control real signalling. A deployable system needs
 * fail-safe hardware, formal verification and compliance with the applicable
 * railway safety standards (EN 50128 / EN 50129 / IEC 61508 SIL 4).
 * ========================================================================== */
#ifndef RAILWAY_TYPES_H
#define RAILWAY_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --------------------------------------------------------------------------
 * API result codes. Every public API call returns one of these.
 * -------------------------------------------------------------------------- */
typedef enum
{
    RW_OK = 0,                  /* success */
    RW_ERR_INVALID_ID = 1,      /* unknown id */
    RW_ERR_INVALID_ARG = 2,     /* bad argument */
    RW_ERR_NOT_INITIALIZED = 3, /* railway_init() not called */
    RW_ERR_INTERLOCK = 4,       /* refused by interlocking logic */
    RW_ERR_CONFLICT = 5,        /* conflicts with a set route */
    RW_ERR_OCCUPIED = 6,        /* track/route occupied */
    RW_ERR_POINT_LOCKED = 7,    /* point locked by a route */
    RW_ERR_POINT_MOVING = 8,    /* point in transit */
    RW_ERR_EMERGENCY = 9,       /* emergency stop active */
    RW_ERR_OUT_OF_SERVICE = 10, /* asset failed or withdrawn */
    RW_ERR_BUSY = 11,           /* resource busy */
    RW_ERR_NO_ROUTE = 12,       /* no such route */
            RW_ERR_ROUTE_SET       = 13,  /* route already set */
            RW_ERR_RANGE           = 14,  /* value out of range */
            RW_ERR_INTERNAL        = 15,  /* internal engine fault */
            RW_ERR_IO              = 16   /* file / stream error (CONJOB, scripting) */
        } RwResult;

const char *rw_result_string(RwResult result);
const char *rw_result_hint(RwResult result);

/* Human readable names for the public enumerations. These are used by the
 * GUI, the terminal and the event log so they all read the same way.
 * (Defined after the enumerations below, so declared at the foot of the file.)
 */

/* Look up an asset by its operator-facing name. Returns RW_ID_NONE when the
 * name is unknown; ids are 1-based so 0 is always "none".
 * (Declared at the foot of the file.)
 */

/* --------------------------------------------------------------------------
 * Public enumerations - deliberately small, describing observable state only.
 * -------------------------------------------------------------------------- */

/* Signal aspects as seen by the signaller. */
typedef enum
{
        SIGNAL_RED    = 0,   /* danger - stop */
        SIGNAL_YELLOW = 1,   /* caution - proceed prepared to stop */
        SIGNAL_GREEN  = 2,   /* clear - proceed */
        SIGNAL_CALLON = 3    /* proceed into an occupied section (call-on) */
    } SignalState;

/* Point / switch lie. */
typedef enum
{
    SWITCH_MAIN = 0,     /* set for the through (normal) route */
    SWITCH_DIVERGING = 1 /* set for the diverging (reverse) route */
} SwitchPosition;

/* Train movement state. */
typedef enum
{
    TRAIN_STOPPED = 0,
    TRAIN_RUNNING = 1,
    TRAIN_EMERGENCY_STOP = 2
} TrainState;

/* Track occupancy - what a track circuit reports. */
typedef enum
{
    TRACK_CLEAR = 0,
    TRACK_OCCUPIED = 1,
    TRACK_UNKNOWN = 2 /* loss of detection - treated as occupied */
} TrackOccupancy;

/* Route lifecycle. */
typedef enum
{
    ROUTE_IDLE = 0,
    ROUTE_REQUESTED = 1,
    ROUTE_LOCKED = 2, /* set and locked - movement authority granted */
    ROUTE_OCCUPIED = 3,
    ROUTE_RELEASED = 4
} RouteState;

/* System-wide operating condition. */
typedef enum
{
    SYSTEM_INITIALISING = 0,
    SYSTEM_ONLINE = 1,
    SYSTEM_DEGRADED = 2,
    SYSTEM_EMERGENCY = 3,
    SYSTEM_SHUTDOWN = 4
} SystemState;

/* --------------------------------------------------------------------------
 * Capacity limits (fixed arrays - deterministic and allocation free).
 * -------------------------------------------------------------------------- */
#define RW_MAX_SIGNALS 96
#define RW_MAX_SWITCHES 32
#define RW_MAX_TRACKS 64
#define RW_MAX_ROUTES 48
#define RW_MAX_TRAINS 24
#define RW_MAX_CARS 32
#define RW_MAX_CARS_PER_TRAIN 16
#define RW_MAX_SENSORS 128
#define RW_MAX_EVENTS  2048
#define RW_EVENT_RETENTION_MAX 2048
#define RW_MAX_NAME 32
#define RW_MAX_TEXT 192
#define RW_MAX_STEPS     32

/* Bound used by the Settings page for the event display window. */
#define RW_EVENT_RETENTION_MAX 2048

typedef uint16_t RwId;
#define RW_ID_NONE ((RwId)0)

/* --------------------------------------------------------------------------
 * Public, read-only views. The GUI copies these out and never touches the
 * engine's internal storage directly.
 * -------------------------------------------------------------------------- */
typedef struct
{
    RwId id;
    char name[RW_MAX_NAME];
    SignalState aspect;
    SignalState commanded;
    bool manual; /* true when operator worked */
    bool in_service;
    RwId track;         /* the track the signal protects */
    RwId holding_route; /* route that holds it at danger/clear */
    float latitude;     /* layout position for the diagram (0..1) */
    float longitude;
} RwSignalInfo;

typedef struct
{
    RwId id;
    char name[RW_MAX_NAME];
    SwitchPosition position;
    SwitchPosition commanded;
    bool locked; /* locked by a route */
    bool in_service;
    bool moving; /* in transit */
    RwId locked_by_route;
    float latitude;
    float longitude;
} RwSwitchInfo;

typedef struct
{
    RwId id;
    char name[RW_MAX_NAME];
    TrackOccupancy occupancy;
    RwId occupied_by; /* train id when occupied */
    int speed_limit_kph;
    bool in_service;
    float length_m;
    float start_lat;
    float start_lon;
    float end_lat;
    float end_lon;
} RwTrackInfo;

typedef struct
{
    RwId id;
    char name[RW_MAX_NAME];
    TrainState state;
    float speed_kph;
    float max_speed_kph;
    RwId track;
    RwId route;
    int carriages;
    bool emergency_brake;
    float latitude;
    float longitude;
    unsigned delay_seconds;
} RwTrainInfo;

/* --------------------------------------------------------------------------
 * Rolling stock - the individual vehicles that make up a train.
 *
 * A train is an ordered formation of cars: a traction unit at the head and
 * one or more vehicles behind it. The car list is what actually determines a
 * train's length, and therefore how many track sections it occupies - a
 * 9-car set straddles more track circuits than a 2-car unit, which is the
 * entire reason a signaller cares about the formation.
 * -------------------------------------------------------------------------- */

/* What kind of vehicle a car is. */
typedef enum
{
    CAR_CLASS_LOCOMOTIVE = 0,  /* traction unit - at the head (or both ends) */
    CAR_CLASS_PASSENGER,       /* passenger saloon / multiple unit vehicle */
    CAR_CLASS_FREIGHT_BOX,     /* covered box wagon */
    CAR_CLASS_FREIGHT_FLAT,    /* flat wagon - containers, timber, steel */
    CAR_CLASS_TANK,            /* tank wagon - liquids and gases */
    CAR_CLASS_HOPPER,          /* bulk hopper - coal, aggregate, ballast */
    CAR_CLASS_BRAKE_VAN,       /* guards van / brake van at the tail */
    CAR_CLASS_DINING,          /* catering vehicle */
    CAR_CLASS_SLEEPER,         /* sleeping car */
    CAR_CLASS_ENGINEERING      /* works vehicle - crane, tamper, etc. */
} RcpCarType;

/* A car's mechanical disposition - what a depot might report. */
typedef enum
{
    CAR_STATUS_OK = 0,
    CAR_STATUS_DEFECTIVE,
    CAR_STATUS_SHUNTING,
    CAR_STATUS_OUT_OF_SERVICE,
    CAR_STATUS_LOCKED
} RcpCarStatus;

/* How a car is coupled to the formation. */
typedef enum
{
    CAR_COUPLING_INTERNAL = 0, /* coupled to the car ahead */
    CAR_COUPLING_LEAD,         /* nothing ahead - the head of the train */
    CAR_COUPLING_TAIL,         /* nothing behind - the tail of the train */
    CAR_COUPLING_LOOSE         /* detached / parked */
} RcpCarCoupling;

/* Read-only view of one car, handed out by the API. */
typedef struct
{
    RwId          id;                         /* car id, 1-based, unique globally */
    RwId          train;                      /* owning train, RW_ID_NONE if loose */
    int           position;                   /* 0 = head, counting back */
    char          number[RW_MAX_NAME];        /* vehicle number, e.g. "10234" */
    char          designation[RW_MAX_NAME];   /* e.g. "Class 220 Car" */
    RcpCarType    type;
    RcpCarStatus  status;
    RcpCarCoupling coupling;
    float         length_m;                   /* over-vehicles length */
    float         tare_tonnes;                /* unladen weight */
    int           capacity;                   /* seats, or tonnes of freight */
    float         load_percent;               /* how full, 0..100 */
    bool          occupied;                   /* passengers/freight on board */
    bool          in_service;
} RwCarInfo;

typedef struct
{
    RwId id;
    char name[RW_MAX_NAME];
    RouteState state;
    RwId entry_signal;
    RwId exit_signal;
    RwId booked_by;
    size_t step_count;
    char conflict[RW_MAX_TEXT];
} RwRouteInfo;

typedef struct
{
    RwId id;
    char name[RW_MAX_NAME];
    int kind;  /* RwSensorKind */
    int state; /* RwSensorState */
    RwId track;
    int value;
    bool in_service;
} RwSensorInfo;

typedef enum
{
    SENSOR_TRACK_CIRCUIT = 0,
    SENSOR_AXLE_COUNTER,
    SENSOR_TREADLE,
    SENSOR_HOT_BOX,
    SENSOR_LEVEL_CROSSING
} RwSensorKind;

typedef enum
{
    SENSOR_OK = 0,
    SENSOR_TRIGGERED,
    SENSOR_FAILED
} RwSensorState;

/* Aggregate status for the header / status bar. */
typedef struct
{
    SystemState state;
    int signals_total;
    int signals_green;
    int signals_red;
    int switches_total;
    int switches_locked;
    int tracks_total;
    int tracks_clear;
    int tracks_occupied;
    int tracks_unknown;
    int trains_total;
    int trains_running;
    int trains_stopped;
    int routes_total;
    int routes_locked;
    int alarms; /* unacknowledged events of severity >= warning */
    int conflicts_blocked;
    unsigned long uptime_seconds;
    unsigned long ticks;
} RwSystemStatus;

typedef enum
{
    SEVERITY_INFO = 0,
    SEVERITY_WARNING,
    SEVERITY_ALARM,
    SEVERITY_CRITICAL
} RwSeverity;

typedef struct
{
    RwId id;
    RwSeverity severity;
    char category[RW_MAX_NAME];
    char message[RW_MAX_TEXT];
    char timestamp[24]; /* "YYYY-MM-DD HH:MM:SS" */
    bool acknowledged;
} RwEventInfo;

/* --------------------------------------------------------------------------
 * Shared helpers declared last so every type above is known.
 * -------------------------------------------------------------------------- */

/* Safety invariants (enum RcInvariant lives in railcontrol.h). */
enum RcInvariant;
const char *rc_invariant_text(enum RcInvariant invariant);

/* Asset lookup by name. Part of the public surface because the terminal, the
 * command line tools and the GUI all resolve operator input the same way. */
RwId rw_signal_find(const char *name);
RwId rw_track_find(const char *name);
RwId rw_point_find(const char *name);
RwId rw_route_find(const char *name);
RwId rw_train_find(const char *name);

/* Enum to text - every front end renders the same labels. */
const char *rw_safety_name(SystemState state);
const char *signal_state_name(SignalState state);
const char *switch_position_name(SwitchPosition position);
const char *train_state_name(TrainState state);
const char *track_occupancy_name(TrackOccupancy occupancy);
const char *route_state_name(RouteState state);
const char *severity_name(RwSeverity severity);
const char *rcp_sensor_kind_name(RwSensorKind kind);
const char *rcp_sensor_state_name(RwSensorState state);
const char *rcp_train_class_name(int cls);
char        rcp_aspect_char(SignalState aspect);
const char *rw_result_string_short(int terminal_status);

/* Rolling stock lookups and labels. Every front end renders cars the same way. */
RwId rw_car_find(const char *number);              /* by vehicle number */
RwId rw_car_find_on_train(RwId train_id, int position);
const char *rcp_car_type_name(RcpCarType type);
const char *rcp_car_status_name(RcpCarStatus status);
const char *rcp_car_coupling_name(RcpCarCoupling coupling);
const char *rcp_car_type_code(RcpCarType type);      /* short code, e.g. "LOCO" */

#endif /* RAILWAY_TYPES_H */
