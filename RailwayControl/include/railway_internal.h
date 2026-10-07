/* ==========================================================================
   * railway_internal.h - Engine-internal state shared by the controllers and
   * the interlocking. Never included by front ends (GUI/console/network).
   * ========================================================================== */
#ifndef RAILWAY_INTERNAL_H
#define RAILWAY_INTERNAL_H

#include "railway_types.h"
#include "interlocking.h"
#include "mcu_plugin.h"
#include "naming.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
    /* --------------------------------------------------------------------------
     * Engine-internal representations. These carry considerably more detail than
     * the Rw*Info views the API hands out.
     * -------------------------------------------------------------------------- */

    typedef struct {
        RwId        id;
        char        name[RW_MAX_NAME];
        RwId        entry_signal;      /* signal at the start of the section */
        RwId        exit_signal;       /* signal at the end, 0 if none */
        RwId        point_upstream;    /* point that feeds this track, 0 if none */
        RwId        point_downstream;  /* point this track feeds, 0 if none */
        float       start_lat, start_lon;
        float       end_lat, end_lon;
        int         speed_limit_kph;
        float       length_m;
        TrackOccupancy occupancy;
        RwId        occupied_by;
        bool        circuit_failed;
        bool        in_service;
        bool        closed_for_work;
        unsigned    occupied_since_ms;
    } TrackState;

typedef struct
{
    RwId id;
    char name[RW_MAX_NAME];
    RwId track;
    bool at_exit;
    SignalState aspect;
    SignalState commanded;
    bool manual;
    bool in_service;
    bool approach_locked;
    RwId held_by_route;
    RwId entry_for_route; /* route this signal is the entry signal of */
} SignalStateInternal;

typedef struct {
    RwId           id;
    char           name[RW_MAX_NAME];
    RwId           track_upstream;
    RwId           track_normal;
    RwId           track_reverse;
    SwitchPosition position;
    SwitchPosition commanded;
    RwId           locked_by_route;
    bool           locked;
    bool           in_service;
    bool           fault;
    int            lock_type;                   /* 0 triggered, 1 keyed */
    unsigned       movement_remaining_ms;       /* >0 while swinging */
    float          latitude;
    float          longitude;
} PointState;

typedef struct
{
    RwId track;
    RwId point;
    SwitchPosition point_required;
} RouteStep;

typedef struct
{
    RwId id;
    char name[RW_MAX_NAME];
    RwId entry_signal;
    RwId exit_signal;
    RouteStep steps[RW_MAX_STEPS];
    size_t step_count;
    RouteState state;
    RwId booked_by;
    bool auto_signal;
    unsigned set_at_ms;
    char conflict[RW_MAX_TEXT];
} RouteStateInternal;

typedef struct
{
    RwId id;
    char name[RW_MAX_NAME];
    RwSensorKind kind;
    RwSensorState state;
    RwId track;
    RwId point;
    int value;
    float temperature_c;
    bool in_service;
    unsigned last_change_ms;
} SensorState;

typedef enum {
    TRAIN_CLASS_PASSENGER = 0,
    TRAIN_CLASS_FREIGHT,
    TRAIN_CLASS_ENGINEERING,
    TRAIN_CLASS_LIGHT_ENGINE
} TrainClass;

/* One vehicle. Cars live in a single engine-wide pool and reference their
 * train; a formation is rebuilt by ordering the pool by train + position. */
typedef struct {
    RwId          id;
    RwId          train;                      /* owning train, RW_ID_NONE if loose */
    int           position;                   /* 0 = head, counting back */
    char          number[RW_MAX_NAME];        /* vehicle number */
    char          designation[RW_MAX_NAME];   /* free text description */
    RcpCarType    type;
    RcpCarStatus  status;
    RcpCarCoupling coupling;
    float         length_m;
    float         tare_tonnes;
    int           capacity;
    float         load_percent;
    bool          occupied;
    bool          in_service;
} CarStateInternal;

typedef struct {
    RwId        id;
    char        headcode[RW_MAX_NAME];
    char        description[RW_MAX_NAME];
    TrainClass  cls;
    TrainState  state;
    RwId track;
    RwId route;
    RwId destination_track;
    float speed_kph;
    float target_speed_kph;
    float max_speed_kph;
    float position_m;
    unsigned length_m;
    int carriages;
    bool emergency_brake;
    bool held;
    unsigned stopped_since_ms;
    unsigned delay_seconds;
    RwId blocked_by_signal;
    RwId last_reporting_sensor;
    float latitude;
    float longitude;
} TrainStateInternal;

typedef struct {
    RwSeverity severity;
    char category[RW_MAX_NAME];
    char message[RW_MAX_TEXT];
    char timestamp[24];
    RwId id;
    bool acknowledged;
} EventRecord;

typedef struct
{
    bool fail_safe_unknown;
    bool allow_call_on;
    bool release_requires_clear;
    bool require_route_lock;
    int point_movement_ms;
    float default_speed_limit_kph;
} InterlockConfig;

/* The one and only engine instance. */
typedef struct
{
    bool initialized;
    char name[RW_MAX_NAME];
    char signal_box[RW_MAX_NAME];
    SystemState system_state;
    bool emergency;
    char emergency_reason[RW_MAX_TEXT];
    unsigned emergency_at_ms;

    TrackState tracks[RW_MAX_TRACKS];
    size_t track_count;
    SignalStateInternal signals[RW_MAX_SIGNALS];
    size_t signal_count;
    PointState points[RW_MAX_SWITCHES];
    size_t point_count;
    RouteStateInternal routes[RW_MAX_ROUTES];
    size_t route_count;
    SensorState sensors[RW_MAX_SENSORS];
    size_t sensor_count;
    TrainStateInternal trains[RW_MAX_TRAINS];
    size_t train_count;
    CarStateInternal cars[RW_MAX_CARS];
    size_t car_count;
    RwId next_car_id;
    EventRecord events[RW_MAX_EVENTS];
    size_t event_count;
    RwId next_event_id;

    InterlockConfig config;
    unsigned long clock_ms;
    unsigned long tick_count;
    unsigned long route_sets;
    unsigned long route_cancels;
    unsigned long conflicts_blocked;
    unsigned long emergency_stops;
    char last_error[RW_MAX_TEXT];

    /* Set when the engine state changes; the GUI polls it to know when to
     * repaint instead of forcing a redraw every timer tick. */
    unsigned long revision;
} RailwayEngine;

/* --------------------------------------------------------------------------
 * Engine accessors and internal services (shared across .c files).
 * -------------------------------------------------------------------------- */
RailwayEngine *rw_engine(void);
bool           rw_engine_ready(void);
void           rw_set_error(const char *format, ...);
void           rw_bump_revision(void);

/* Name lookups by name (defined in rcp_util.c). */
RwId rw_signal_find(const char *name);
RwId rw_track_find(const char *name);
RwId rw_point_find(const char *name);
RwId rw_route_find(const char *name);
RwId rw_train_find(const char *name);

/* Enum to text (defined in rcp_util.c). */
const char *rw_safety_name(SystemState state);
const char *signal_state_name(SignalState state);
const char *switch_position_name(SwitchPosition position);
const char *train_state_name(TrainState state);
const char *track_occupancy_name(TrackOccupancy occupancy);
const char *route_state_name(RouteState state);
const char *severity_name(RwSeverity severity);

/* Event log  (📝) */
void rw_event_log_init(void);
void rw_event(RwSeverity severity, const char *category, const char *format, ...);

/* Signal controller  (🚦) */
void rw_signals_init(void);
void rw_signals_tick(unsigned delta_ms);
bool rw_signal_is_proceed(SignalState aspect);
void rw_signal_force(SignalStateInternal *signal, SignalState aspect);
RwResult rw_signal_controls_track(RwId signal_id, RwId *track_out);

/* Switch controller  (🔀) */
void rw_points_init(void);
void rw_points_tick(unsigned delta_ms);
bool rw_point_is_moving(const PointState *point);
bool rw_point_route_matches(const PointState *point, SwitchPosition required);

/* Train controller  (🚆) */
void rw_trains_init(void);
void rw_trains_tick(unsigned delta_ms);
void rw_train_apply_emergency(TrainStateInternal *train);

/* Rolling stock  (🚃) - the cars that make up each formation. */
void  rw_cars_init(void);
RwId  rw_car_add(RwId train_id, RcpCarType type, const char *number,
                 const char *designation, float length_m,
                 float tare_tonnes, int capacity);
RwResult rw_car_remove(RwId car_id);
CarStateInternal *rw_car_by_id(RwId car_id);
void  rw_car_to_info(RwId car_id, RwCarInfo *info);
int   rw_cars_of_train(RwId train_id, RwCarInfo *out, int max);
int   rw_cars_of_train_count(RwId train_id);
float rw_train_formation_length(RwId train_id);
RwResult rw_train_recompute_length(RwId train_id);
const char *rw_car_default_designation(RcpCarType type);
float rw_car_default_length(RcpCarType type);
float rw_car_default_tare(RcpCarType type);
int rw_car_default_capacity(RcpCarType type);

/* Signal-internal helpers used by the train controller. */
bool rw_signal_visible_to_train(const RailwayEngine *engine, RwId signal_id,
                                const TrainStateInternal *train,
                                SignalState *aspect_out);

/* Track occupancy  (🛤️) */
const char *severity_name(RwSeverity severity);
void           rw_tracks_init(void);
void rw_track_set_occupancy(RwId track_id, TrackOccupancy occupancy, RwId train_id);
TrackOccupancy rw_track_occupancy(const RailwayEngine *engine, RwId track_id);
bool rw_track_is_clear_for_route(const RailwayEngine *engine, RwId track_id);
void rw_track_recompute_from_circuits(void);

/* Interlocking  (🔒) */
void rw_interlock_init(void);
RwResult rw_interlock_validate_route(RwId route_id, char *reason, size_t reason_size);
RwResult rw_interlock_set_route(RwId route_id, RwId train_id);
RwResult rw_interlock_cancel_route(RwId route_id);
void rw_interlock_release_routes(void);
void rw_interlock_apply_aspects(void);
RwResult rw_interlock_route_conflicts(RwId route_id, RwId *other_route, char *reason, size_t size);
bool rw_interlock_path_conflict(const RailwayEngine *engine, RwId route_a, RwId route_b);
RwResult rw_interlock_force_signal(RwId signal_id, SignalState aspect);

/* Emergency  (🛑) */
void rw_emergency_engage(const char *reason);
bool rw_emergency_can_clear(void);

/* Sensors  (📡) */
void rw_sensors_init(void);
RwResult rw_sensor_apply(RwId sensor_id, int value, int state);
void rw_sensors_tick(unsigned delta_ms);
void rw_sensor_fail(RwId sensor_id);

/* Rolling stock / field-bus glue  (🔌).
 *
 * A microcontroller plugin reports a reading for a *track*, not for a sensor
 * id it happens to know. These helpers map a plugin binding onto whatever
 * sensor the layout put on that track, so the plugin never has to be told
 * the sensor numbering - and a layout change does not break a board. */
typedef enum {
    TRACK_CIRCUIT_SENSOR = 0,
    AXLE_COUNTER_SENSOR,
    TREADLE_SENSOR,
    HOT_BOX_SENSOR,
    LEVEL_CROSSING_SENSOR
} RcFieldSensorKind;

/* The plugin types are needed by the signature below. mcu_plugin.h is
 * self-contained (it only depends on railway_types.h), so it can be included
 * here without a cycle. */
RwResult rw_sensor_apply_routed(const McuPlugin *plugin,
                                const McuBinding *binding,
                                RcFieldSensorKind kind, int value, int state);

/* Wrap-around safe "has this deadline passed?" for the millisecond clock. */
bool elapsed_exceeded(unsigned now_ms, unsigned then_ms, unsigned limit_ms);

/* Layout construction (the demonstration railway). */
RwResult rw_layout_build_default(RailwayEngine *engine);

/* Utilities */
const char *rw_now_string(char *buffer, size_t size);

void rw_point_apply_position(PointState *point, SwitchPosition position);
bool rw_point_reached(const PointState *point, SwitchPosition wanted);
bool rw_point_is_moving(const PointState *point);

const char *rcp_sensor_kind_name(RwSensorKind kind);
const char *rcp_sensor_state_name(RwSensorState state);
const char *rcp_train_class_name(int cls);
char        rcp_aspect_char(SignalState aspect);

/* Human readable label for a terminal command result. */
const char *rw_result_string_short(int terminal_status);

/* --------------------------------------------------------------------------
 * Terminal status labels. Kept here rather than in terminal.c so that batch
 * mode and the GUI share one definition.
 * -------------------------------------------------------------------------- */
const char *rw_result_string_short(int terminal_status);

#endif /* RAILWAY_INTERNAL_H */
