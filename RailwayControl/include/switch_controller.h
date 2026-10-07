/* ==========================================================================
 * switch_controller.h - 🔀 Switch / point control.
 *
 * Points move over time, lock when a route is set, and must never be moved
 * under a train. Detection ("correspondence") is modelled: a point that fails
 * to reach its commanded lie is flagged as a fault and the interlocking then
 * refuses routes over it.
 * ========================================================================== */
#ifndef SWITCH_CONTROLLER_H
#define SWITCH_CONTROLLER_H

#include "railway_types.h"

/* Build the initial point fleet. */
void switch_controller_init(void);

/* Per-tick work: advance points in transit, complete or fault their swing. */
void switch_controller_tick(unsigned delta_ms);

/* Request a lie.
 *   - emergency active         -> RW_ERR_EMERGENCY (points are held)
 *   - out of service / faulted -> RW_ERR_OUT_OF_SERVICE
 *   - locked by a route        -> RW_ERR_POINT_LOCKED
 *   - already moving           -> RW_ERR_BUSY
 *   - track upstream occupied  -> RW_ERR_OCCUPIED (never move under a train)
 * A no-op request (already in the required lie) returns RW_OK. */
RwResult switch_controller_set_position(RwId switch_id, SwitchPosition position);

/* Release a route lock. This is the equivalent of a manual release: it is
 * refused while a train is on or approaching the point unless an emergency
 * stop is active. Logged as a warning event when it succeeds. */
RwResult switch_controller_release(RwId switch_id);

/* In or out of service. */
RwResult switch_controller_set_in_service(RwId switch_id, bool in_service);

/* Lookups. */
RwResult switch_controller_get_position(RwId switch_id, SwitchPosition *position);
RwResult switch_controller_get_info(RwId switch_id, RwSwitchInfo *info);
int switch_controller_count(void);
RwResult switch_controller_get_at(int index, RwSwitchInfo *info);

/* Interlocking interface. */
void switch_controller_lock_for_route(RwId switch_id, RwId route_id);
void switch_controller_unlock_for_route(RwId switch_id);
void switch_controller_release_all(void);
bool switch_controller_is_available(RwId switch_id);
int switch_controller_locked_count(void);

/* Mark a point as failed to correspond - detection loss. */
void switch_controller_set_fault(RwId switch_id, bool fault);

#endif /* SWITCH_CONTROLLER_H */
