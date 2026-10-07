/* ==========================================================================
 * railway_api.h - Public API for the Railway Control Center.
 *
 * This is the ONLY header a front end needs. The engine (interlocking,
 * controllers) is fully encapsulated behind it: the Win32 GUI, a console,
 * a network server or an automated simulator all drive the railway through
 * these calls and cannot reach the internals.
 *
 * Thread-safety: every function takes the engine lock and may be called from
 * any thread (the GUI calls from the UI thread, the network server from a
 * worker). It is NOT re-entrant: do not call the API from inside a callback.
 *
 * SAFETY NOTICE
 * -------------
 * Prototype / simulator. Not certified for real signalling. See the README
 * for the safety case and the standards a deployable system must satisfy.
 * ========================================================================== */
#ifndef RAILWAY_API_H
#define RAILWAY_API_H

#include "railway_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* ==========================================================================
     * 1. LIFECYCLE
     * ========================================================================== */

    /* Initialise the engine and build the default demonstration layout.
     * Safe to call twice; the second call is a no-op. */
    RwResult railway_init(void);

    /* Tear the engine down and release every resource. */
    void railway_shutdown(void);

    /* True once railway_init() has completed successfully. */
    bool railway_is_initialized(void);

    /* ==========================================================================
     * 2. SIGNAL CONTROL  (🚦)
     * ========================================================================== */

    /* Force an aspect. Fails with RW_ERR_INTERLOCK when the interlocking refuses
     * (e.g. clearing a signal into an occupied or unsafe section), and with
     * RW_ERR_EMERGENCY while an emergency stop is active. */
    RwResult railway_set_signal(int signal_id, SignalState state);

    /* Put a signal under manual (operator) or automatic (interlocking) control. */
    RwResult railway_set_signal_mode(int signal_id, bool manual);

    /* Place a signal in or out of service. Out of service forces it to danger. */
    RwResult railway_set_signal_in_service(int signal_id, bool in_service);

    RwResult railway_get_signal(int signal_id, SignalState *state);
    RwResult railway_get_signal_info(int signal_id, RwSignalInfo *info);

    /* Replaceable signal list accessors - the GUI enumerates with these. */
    int railway_signal_count(void);
    RwResult railway_get_signal_at(int index, RwSignalInfo *info);

    /* ==========================================================================
     * 3. SWITCH / POINT CONTROL  (🔀)
     * ========================================================================== */

    /* Request a lie. The point swings over time; the call returns RW_OK once the
     * request is accepted (not once it has completed - poll with
     * railway_get_switch_info()). Refused when locked by a route, occupied, or in
     * an emergency. */
    RwResult railway_set_switch(int switch_id, SwitchPosition position);

    /* Override a route lock. Requires the emergency stop or an operator key in a
     * real interlocking; here it is an explicit, audited operation. */
    RwResult railway_release_switch(int switch_id);
    RwResult railway_set_switch_in_service(int switch_id, bool in_service);

    RwResult railway_get_switch(int switch_id, SwitchPosition *position);
    RwResult railway_get_switch_info(int switch_id, RwSwitchInfo *info);

    int railway_switch_count(void);
    RwResult railway_get_switch_at(int index, RwSwitchInfo *info);

    /* ==========================================================================
     * 4. TRAIN CONTROL  (🚆)
     * ========================================================================== */

    RwResult railway_set_train_state(int train_id, TrainState state);
    RwResult railway_get_train_state(int train_id, TrainState *state);
    RwResult railway_get_train_info(int train_id, RwTrainInfo *info);

    /* Set a movement authority: the train may run to the end of `route_id`. */
    RwResult railway_assign_train_route(int train_id, int route_id);

    /* Target speed (0 stops under normal braking, no emergency). */
    RwResult railway_set_train_speed(int train_id, float speed_kph);

    /* Hold a train at its current location. */
    RwResult railway_hold_train(int train_id, bool hold);

    int railway_train_count(void);
    RwResult railway_get_train_at(int index, RwTrainInfo *info);

    /* ==========================================================================
     * 4b. ROLLING STOCK  (🚃)
     * ==========================================================================
     *
     * Each train is an ordered formation of cars. The train's reported length
     * is derived from its cars, so adding or removing a car changes how much
     * track the train occupies.
     */

    /* Number of cars in the whole system, and on one train. */
    int railway_car_count(void);
    int railway_train_car_count(int train_id);

    /* Read one car by its id, or by its position on a train (0 = the head).
     * Returns RW_ERR_INVALID_ID when there is no such car. */
    RwResult railway_get_car_info(int car_id, RwCarInfo *info);
    RwResult railway_get_car_at(int index, RwCarInfo *info);
    RwResult railway_get_train_car(int train_id, int position, RwCarInfo *info);

    /* The full formation, head first. Returns the number of cars written. */
    int railway_list_train_cars(int train_id, RwCarInfo *out, int max);

    /* Total length of a formation in metres (0 if the train has no cars). */
    float railway_train_length_m(int train_id);

    /* Attach a vehicle to a train (train_id = 0 creates a loose car).
     * The type defaults supply the length, tare and capacity when the
     * corresponding argument is 0 / NULL. Returns the new car id, or
     * RW_ID_NONE when the pool or the formation is full. */
    int railway_add_train_car(int train_id, RcpCarType type,
                              const char *number, const char *designation,
                              float length_m, float tare_tonnes, int capacity);

    /* Detach a vehicle. The train's length is recomputed. */
    RwResult railway_remove_train_car(int car_id);

    /* Place a car in or out of service; a defective car is withdrawn. */
    RwResult railway_set_car_status(int car_id, RcpCarStatus status);
    RwResult railway_set_car_in_service(int car_id, bool in_service);

    /* Resolve a car by its vehicle number. RW_ID_NONE when unknown. */
    int railway_find_car(const char *number);

    /* ==========================================================================
     * 5. ROUTES AND INTERLOCKING  (🔒)
     * ==========================================================================
     *
     * railway_request_route() runs the full interlocking proof before granting
     * authority. On refusal, `railway_last_error()` returns the reason and the
     * route's conflict field is populated for display.
     */
    RwResult railway_request_route(int route_id);

    /* Cancel a route. Refused while a train occupies it (use emergency or
     * release after the train has cleared). */
    RwResult railway_cancel_route(int route_id);

    RwResult railway_get_route_info(int route_id, RwRouteInfo *info);
    int railway_route_count(void);
    RwResult railway_get_route_at(int index, RwRouteInfo *info);

    /* Resolve a route by name ("R1", "UP MAIN"). Returns RW_ID_NONE as error. */
    int railway_find_route(const char *name);

    /* Set a route and immediately give authority to the first train waiting at
     * its entry signal - the common "one button" signaller operation. */
    RwResult railway_set_route_and_dispatch(int route_id);

    /* ==========================================================================
     * 6. EMERGENCY STOP  (🛑)
     * ========================================================================== */

    /* Trip the emergency stop: every signal to danger, every train emergency
     * braked, all points held. Idempotent. */
    int railway_emergency_stop(void);

    /* As above but records an operator supplied reason. */
    RwResult railway_emergency_stop_reason(const char *reason);

    /* Clear the emergency and return to normal working. Refused while any train
     * is still moving above walking pace or a track remains occupied by a train
     * under emergency braking. */
    RwResult railway_clear_emergency(void);

    bool railway_emergency_active(void);
    SystemState railway_system_state(void);

    /* ==========================================================================
     * 7. TRACK OCCUPANCY  (🛤️) and SENSOR INPUTS  (📡)
     * ========================================================================== */

    RwResult railway_get_track(int track_id, TrackOccupancy *occupancy);
    RwResult railway_get_track_info(int track_id, RwTrackInfo *info);
    int railway_track_count(void);
    RwResult railway_get_track_at(int index, RwTrackInfo *info);

    /* Inject a sensor reading. This is how a real field interface would drive the
     * engine: the sensor updates the track circuit / point detection and the
     * interlocking reacts. */
    RwResult railway_inject_sensor(int sensor_id, int value, int state);

    /* Mark a track circuit as failed (loss of detection). The interlocking then
     * treats the section as unsafe and will not set routes over it. */
    RwResult railway_fail_track_circuit(int track_id, bool failed);

    int railway_sensor_count(void);
    RwResult railway_get_sensor_at(int index, RwSensorInfo *info);

    /* ==========================================================================
     * 8. SIMULATION AND MONITORING  (🎛️)
     * ========================================================================== */

    /* Advance the simulation by `delta_ms`. The GUI timer and the headless
     * simulator both call this; nothing else drives time. */
    RwResult railway_tick(unsigned delta_ms);

    /* Aggregate status for the header bar and status line. */
    RwResult railway_get_status(RwSystemStatus *status);

    /* Event log  (📝) */
    int railway_event_count(void);
    RwResult railway_get_event_at(int index, RwEventInfo *info);
    int railway_unacknowledged_count(void);
    RwResult railway_acknowledge_event(int event_id);
    int railway_acknowledge_all(void);
    RwResult railway_log_message(RwSeverity severity, const char *category, const char *message);

    /* Human readable description of the last failure, for the status bar. */
    const char *railway_last_error(void);

    /* Layout metadata for titles and the About box. */
    const char *railway_system_name(void);
    const char *railway_signal_box_name(void);

#ifdef __cplusplus
}
#endif

#endif /* RAILWAY_API_H */
