/* ==========================================================================
 * rcp_types.h - Core data model for the Railway Control Program.
 *
 * The model follows how a real interlocking is described:
 *
 *   Layout          the whole controlled area
 *    +-- Track      a section of line between two boundaries (the block)
 *    +-- Signal     a stop/go authority at one end of a track
 *    +-- Point      a switch / turnout joining up to three tracks
 *    +-- Route      a permitted path: an entry signal, points, and tracks
 *    +-- Sensor     a track circuit / axle counter / treadle input
 *    +-- Train      a movement authority holder occupying tracks
 *
 * Everything is a fixed-size array indexed by a small integer id, so the
 * program is deterministic, allocation-free after start-up, and auditable -
 * which is what a safety system should be.
 * ========================================================================== */
#ifndef RCP_TYPES_H
#define RCP_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --------------------------------------------------------------------------
 * Limits. Fixed capacity keeps the store flat and the tests reproducible.
 * -------------------------------------------------------------------------- */
#define RCP_MAX_TRACKS 64
#define RCP_MAX_SIGNALS 96
#define RCP_MAX_POINTS 32
#define RCP_MAX_ROUTES 48
#define RCP_MAX_SENSORS 128
#define RCP_MAX_TRAINS 24
#define RCP_MAX_EVENTS 4096
#define RCP_MAX_ROUTE_ITEMS 32
#define RCP_MAX_NAME 32
#define RCP_MAX_MESSAGE 192
#define RCP_MAX_LINE 256

/* --------------------------------------------------------------------------
 * Identifiers. 0 is always "none" / "invalid".
 * -------------------------------------------------------------------------- */
typedef uint16_t rcp_id_t;
#define RCP_ID_NONE ((rcp_id_t)0)

/* --------------------------------------------------------------------------
 * Time. Monotonic milliseconds since program start.
 * -------------------------------------------------------------------------- */
typedef uint64_t rcp_time_t;

/* --------------------------------------------------------------------------
 * Emergency / safety states, ordered by severity so they can be compared.
 * -------------------------------------------------------------------------- */
typedef enum
{
    RCP_SAFETY_NORMAL = 0, /* everything running */
    RCP_SAFETY_RESTRICTED, /* degraded: speed limits, manual working */
    RCP_SAFETY_EMERGENCY,  /* emergency stop active: all signals at danger */
    RCP_SAFETY_FAILED      /* interlocking fault: movement prohibited */
} rcp_safety_t;

const char *rcp_safety_name(rcp_safety_t state);

/* --------------------------------------------------------------------------
 * Signals
 * -------------------------------------------------------------------------- */
typedef enum
{
    RCP_SIGNAL_DANGER = 0, /* STOP - red */
    RCP_SIGNAL_CAUTION,    /* proceed at restricted speed - yellow */
    RCP_SIGNAL_CLEAR,      /* proceed - green */
    RCP_SIGNAL_CALLON,     /* proceed into an occupied section - white */
    RCP_SIGNAL_BLANK       /* out of service */
} rcp_signal_aspect_t;

const char *rcp_aspect_name(rcp_signal_aspect_t aspect);
/* Single character used by the map renderer. */
char rcp_aspect_char(rcp_signal_aspect_t aspect);

typedef enum
{
    RCP_SIGNAL_AUTO = 0, /* worked by the interlocking */
    RCP_SIGNAL_MANUAL    /* operator controlled */
} rcp_signal_mode_t;

typedef struct
{
    rcp_id_t id;
    char name[RCP_MAX_NAME]; /* e.g. "S1", "S3R" */
    rcp_id_t track;          /* track it protects */
    bool at_exit;            /* false = entry end, true = exit end */
    rcp_signal_aspect_t aspect;
    rcp_signal_aspect_t commanded; /* what the interlocking wants */
    rcp_signal_mode_t mode;
    bool approach_locked; /* route locked ahead */
    bool in_service;
    rcp_id_t controlled_by_route; /* route currently holding it */
    rcp_id_t protects_route;      /* route it is the entry signal of */
} rcp_signal_t;

/* --------------------------------------------------------------------------
 * Track sections (blocks)
 * -------------------------------------------------------------------------- */
typedef enum
{
    RCP_TRACK_FREE = 0,
    RCP_TRACK_OCCUPIED,
    RCP_TRACK_UNKNOWN, /* circuit failed / no data - treat as unsafe */
    RCP_TRACK_CLOSED   /* closed for engineering work */
} rcp_track_state_t;

const char *rcp_track_state_name(rcp_track_state_t state);

typedef enum
{
    RCP_TRACK_MAIN = 0, /* running line */
    RCP_TRACK_LOOP,     /* passing loop / refuge siding */
    RCP_TRACK_SIDING,   /* dead-end siding */
    RCP_TRACK_PLATFORM, /* platform road */
    RCP_TRACK_CROSSOVER /* crossover between lines */
} rcp_track_kind_t;

const char *rcp_track_kind_name(rcp_track_kind_t kind);

typedef struct
{
    rcp_id_t id;
    char name[RCP_MAX_NAME]; /* e.g. "T1", "P2-A" */
    rcp_track_kind_t kind;
    rcp_track_state_t state;
    rcp_id_t occupied_by; /* train id, 0 when clear */
    float length_m;       /* physical length in metres */
    int speed_limit_kph;  /* line speed over this section */
    bool in_service;
    unsigned occupancy_since_ms; /* 0 when clear */
} rcp_track_t;

/* --------------------------------------------------------------------------
 * Points / switches
 * -------------------------------------------------------------------------- */
typedef enum
{
    RCP_POINT_NORMAL = 0, /* trailing the straight route */
    RCP_POINT_REVERSE,    /* set for the diverging route */
    RCP_POINT_MOVING,     /* in transit - must not be passed */
    RCP_POINT_OUT_OF_SERVICE,
    RCP_POINT_FAULT /* detected failed to correspond */
} rcp_point_state_t;

const char *rcp_point_state_name(rcp_point_state_t state);

typedef enum
{
    RCP_POINT_TRIGGERED = 0, /* points lock when a route is set */
    RCP_POINT_KEYED          /* operator must use a key (manual release) */
} rcp_point_lock_t;

typedef struct
{
    rcp_id_t id;
    char name[RCP_MAX_NAME]; /* e.g. "P1", "P2" */
    rcp_id_t track_normal;   /* track reached when NORMAL */
    rcp_id_t track_reverse;  /* track reached when REVERSE */
    rcp_id_t track_upstream; /* the common track */
    rcp_point_state_t state;
    rcp_point_state_t commanded;
    rcp_point_lock_t lock_type;
    bool locked; /* locked by a set route */
    rcp_id_t locked_by_route;
    bool in_service;
    unsigned movement_ms; /* ms remaining to complete */
} rcp_point_t;

/* --------------------------------------------------------------------------
 * Sensors
 * -------------------------------------------------------------------------- */
typedef enum
{
    RCP_SENSOR_TRACK_CIRCUIT = 0, /* occupies a track when true */
    RCP_SENSOR_AXLE_COUNTER,      /* counts axles in / out of a track */
    RCP_SENSOR_TREADLE,           /* momentary: something passed */
    RCP_SENSOR_HOT_BOX,           /* temperature alarm */
    RCP_SENSOR_CCTV,              /* operator observation */
    RCP_SENSOR_LEVEL_CROSSING     /* barrier / obstruction input */
} rcp_sensor_kind_t;

const char *rcp_sensor_kind_name(rcp_sensor_kind_t kind);

typedef enum
{
    RCP_SENSOR_OK = 0,
    RCP_SENSOR_TRIGGERED,
    RCP_SENSOR_FAILED /* loss of input - fail safe, treat as occupied */
} rcp_sensor_state_t;

const char *rcp_sensor_state_name(rcp_sensor_state_t state);

typedef struct
{
    rcp_id_t id;
    char name[RCP_MAX_NAME]; /* e.g. "TC1", "AC3" */
    rcp_sensor_kind_t kind;
    rcp_sensor_state_t state;
    rcp_id_t track; /* track it reports on */
    rcp_id_t point; /* for point detection, else 0 */
    int value;      /* last reading (axle count, temp) */
    float temperature_c;
    bool in_service;
    unsigned last_change_ms;
} rcp_sensor_t;

/* --------------------------------------------------------------------------
 * Trains
 * -------------------------------------------------------------------------- */
typedef enum
{
    RCP_TRAIN_IDLE = 0,  /* in a siding, no movement authority */
    RCP_TRAIN_READY,     /* crewed, awaiting a route */
    RCP_TRAIN_MOVING,    /* running under a movement authority */
    RCP_TRAIN_HELD,      /* held at a signal */
    RCP_TRAIN_STOPPED,   /* stopped short of a signal or on a section */
    RCP_TRAIN_EMERGENCY, /* emergency brake applied */
    RCP_TRAIN_DERELICT   /* failed on the line - needs recovery */
} rcp_train_state_t;

const char *rcp_train_state_name(rcp_train_state_t state);

typedef enum
{
    RCP_TRAIN_PASSENGER = 0,
    RCP_TRAIN_FREIGHT,
    RCP_TRAIN_ENGINEERING,
    RCP_TRAIN_LIGHT_ENGINE
} rcp_train_class_t;

const char *rcp_train_class_name(rcp_train_class_t cls);

typedef struct
{
    rcp_id_t id;
    char headcode[RCP_MAX_NAME]; /* e.g. "1A34" */
    char description[RCP_MAX_NAME];
    rcp_train_class_t cls;
    rcp_train_state_t state;
    rcp_id_t track; /* current track */
    rcp_id_t destination_track;
    rcp_id_t route;  /* movement authority, 0 if none */
    float speed_kph; /* current speed */
    float target_speed_kph;
    float max_speed_kph; /* traction/stock limit */
    float position_m;    /* distance along current track */
    unsigned length_m;
    int carriages;
    bool emergency_brake;
    unsigned stopped_since_ms;
    unsigned delay_seconds; /* accumulated delay */
    rcp_id_t blocked_by_signal;
} rcp_train_t;

/* --------------------------------------------------------------------------
 * Routes
 *
 * A route is the interlocking's unit of authority: entry signal -> exit
 * signal over a list of tracks with a required lie for each point on the way.
 * -------------------------------------------------------------------------- */
typedef enum
{
    RCP_ROUTE_CALLED = 0, /* requested by the signaller, not yet set */
    RCP_ROUTE_SET,        /* locked, signals cleared, trains may move */
    RCP_ROUTE_OCCUPIED,   /* a train is within the route */
    RCP_ROUTE_RELEASING,  /* train has passed, approaching release */
    RCP_ROUTE_CANCELLED,
    RCP_ROUTE_CONFLICT /* could not be set - conflicting route/track */
} rcp_route_state_t;

const char *rcp_route_state_name(rcp_route_state_t state);

typedef struct
{
    rcp_id_t track;                   /* track on the path */
    rcp_id_t point;                   /* point required on this path, 0 = none */
    rcp_point_state_t point_required; /* NORMAL or REVERSE */
} rcp_route_step_t;

typedef struct
{
    rcp_id_t id;
    char name[RCP_MAX_NAME]; /* e.g. "R1", "UP MAIN" */
    rcp_id_t entry_signal;
    rcp_id_t exit_signal;
    rcp_route_step_t steps[RCP_MAX_ROUTE_ITEMS];
    size_t step_count;
    rcp_route_state_t state;
    rcp_id_t booked_by; /* train holding the route */
    bool auto_signal;   /* clear automatically when free */
    unsigned set_at_ms;
    char conflict_reason[RCP_MAX_MESSAGE];
} rcp_route_t;

/* --------------------------------------------------------------------------
 * Interlocking configuration
 * -------------------------------------------------------------------------- */
typedef struct
{
    bool fail_safe_unknown_occupancy; /* UNKNOWN track blocks routes */
    bool allow_call_on;               /* permit call-on into occupied track */
    bool release_requires_clear;      /* route releases only once clear */
    int point_movement_ms;            /* time for a point to swing */
    float default_speed_limit_kph;
    bool require_route_lock; /* always true in practice */
} rcp_interlock_config_t;

/* --------------------------------------------------------------------------
 * The whole layout
 * -------------------------------------------------------------------------- */
typedef struct
{
    char name[RCP_MAX_NAME];
    char signal_box[RCP_MAX_NAME];
    rcp_track_t tracks[RCP_MAX_TRACKS];
    size_t track_count;
    rcp_signal_t signals[RCP_MAX_SIGNALS];
    size_t signal_count;
    rcp_point_t points[RCP_MAX_POINTS];
    size_t point_count;
    rcp_route_t routes[RCP_MAX_ROUTES];
    size_t route_count;
    rcp_sensor_t sensors[RCP_MAX_SENSORS];
    size_t sensor_count;
    rcp_train_t trains[RCP_MAX_TRAINS];
    size_t train_count;

    rcp_safety_t safety;
    bool emergency_active;
    char emergency_reason[RCP_MAX_MESSAGE];
    unsigned emergency_at_ms;

    rcp_interlock_config_t config;

    /* Simulation clock and counters. */
    rcp_time_t clock_ms;
    unsigned long tick_count;
    unsigned long route_sets;
    unsigned long route_cancels;
    unsigned long conflict_blocks;
    unsigned long emergency_stops;
} rcp_layout_t;

/* --------------------------------------------------------------------------
 * Lookup helpers. All return RCP_ID_NONE / NULL when not found and never
 * dereference an invalid id.
 * -------------------------------------------------------------------------- */
rcp_track_t *rcp_track_get(rcp_layout_t *layout, rcp_id_t id);
const rcp_track_t *rcp_track_get_const(const rcp_layout_t *layout, rcp_id_t id);
rcp_signal_t *rcp_signal_get(rcp_layout_t *layout, rcp_id_t id);
const rcp_signal_t *rcp_signal_get_const(const rcp_layout_t *layout, rcp_id_t id);
rcp_point_t *rcp_point_get(rcp_layout_t *layout, rcp_id_t id);
const rcp_point_t *rcp_point_get_const(const rcp_layout_t *layout, rcp_id_t id);
rcp_route_t *rcp_route_get(rcp_layout_t *layout, rcp_id_t id);
const rcp_route_t *rcp_route_get_const(const rcp_layout_t *layout, rcp_id_t id);
rcp_sensor_t *rcp_sensor_get(rcp_layout_t *layout, rcp_id_t id);
const rcp_sensor_t *rcp_sensor_get_const(const rcp_layout_t *layout, rcp_id_t id);
rcp_train_t *rcp_train_get(rcp_layout_t *layout, rcp_id_t id);
const rcp_train_t *rcp_train_get_const(const rcp_layout_t *layout, rcp_id_t id);

rcp_id_t rcp_signal_find(const rcp_layout_t *layout, const char *name);
rcp_id_t rcp_track_find(const rcp_layout_t *layout, const char *name);
rcp_id_t rcp_point_find(const rcp_layout_t *layout, const char *name);
rcp_id_t rcp_route_find(const rcp_layout_t *layout, const char *name);
rcp_id_t rcp_train_find(const rcp_layout_t *layout, const char *name);

#endif /* RCP_TYPES_H */
