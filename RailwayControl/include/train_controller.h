/* ==========================================================================
 * train_controller.h - 🚆 Train detection, movement authority and speed.
 *
 * Trains move under a movement authority granted by the interlocking. The
 * controller enforces:
 *   - speed limits for the section and the traction/stock
 *   - braking for a signal at danger (including a sub-approach-visible stop)
 *   - train separation (never enter an occupied section)
 *   - emergency braking under the emergency stop or on a route cancellation
 *   - track occupancy updates as the train crosses section boundaries
 * ========================================================================== */
#ifndef TRAIN_CONTROLLER_H
#define TRAIN_CONTROLLER_H

#include "railway_types.h"

/* Build the initial fleet and place each train on its starting track. */
void train_controller_init(void);

/* Per-tick work: integrate position and speed, move across boundaries,
 * update track occupancy, apply route authority. */
void train_controller_tick(unsigned delta_ms);

/* Explicitly set a train's movement state.
 *   TRAIN_STOPPED        - normal brake to a stand
 *   TRAIN_RUNNING        - run under the granted authority
 *   TRAIN_EMERGENCY_STOP - emergency brake immediately */
RwResult train_controller_set_state(RwId train_id, TrainState state);

/* Target speed in km/h. Values are clamped to the stock limit and the
 * section limit; 0 requests a controlled stop. */
RwResult train_controller_set_speed(RwId train_id, float speed_kph);

/* Give a train authority over a route. The interlocking must already have set
 * the route (route_locked); otherwise RW_ERR_NO_ROUTE. */
RwResult train_controller_assign_route(RwId train_id, RwId route_id);

/* Hold or release a train at its current location (does not cancel a route). */
RwResult train_controller_hold(RwId train_id, bool hold);

/* Lookups. */
RwResult train_controller_get_state(RwId train_id, TrainState *state);
RwResult train_controller_get_info(RwId train_id, RwTrainInfo *info);
int train_controller_count(void);
RwResult train_controller_get_at(int index, RwTrainInfo *info);

/* The first train currently standing at the entry signal of `route_id`, or
 * RW_ID_NONE. Used by the "set route and dispatch" operation. */
RwId train_controller_waiting_for_route(RwId route_id);

/* Emergency braking for every train. Returns how many were affected. */
int train_controller_emergency_all(const char *reason);

/* True when every train is at a stand. Required before an emergency can be
 * cleared. */
bool train_controller_all_stopped(void);

/* Collision / conflict detection (⚠️).
 * Returns the number of pairs found; fills `train_a`/`train_b` for the worst
 * case and writes a human readable explanation. */
int train_controller_detect_conflicts(RwId *train_a, RwId *train_b,
                                      char *reason, size_t reason_size);

/* Total track circuit occupancies contributed by trains. */
int train_controller_occupancy_count(void);

#endif /* TRAIN_CONTROLLER_H */
