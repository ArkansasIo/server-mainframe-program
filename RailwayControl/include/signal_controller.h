/* ==========================================================================
 * signal_controller.h - 🚦 Signal control.
 *
 * Owns signal aspects, manual/automatic working, approach locking and the
 * rule that a signal may only be cleared when the section ahead is proven
 * clear and the interlocking has granted the route.
 * ========================================================================== */
#ifndef SIGNAL_CONTROLLER_H
#define SIGNAL_CONTROLLER_H

#include "railway_types.h"

/* Build the initial signal fleet and bind each signal to its track. */
void signal_controller_init(void);

/* Per-tick work: applies commanded aspects, expires approach locking, and
 * forces every signal to danger while an emergency stop is active. */
void signal_controller_tick(unsigned delta_ms);

/* Request an aspect. Runs the safety proof:
 *   - emergency active            -> RW_ERR_EMERGENCY
 *   - signal out of service       -> RW_ERR_OUT_OF_SERVICE
 *   - clearing into UNKNOWN track -> RW_ERR_INTERLOCK (fail safe)
 *   - clearing into occupied track-> RW_ERR_OCCUPIED (unless call-on)
 *   - no route set for this signal-> RW_ERR_INTERLOCK
 * Forcing to danger is ALWAYS permitted - it is the safe direction. */
RwResult signal_controller_set_aspect(RwId signal_id, SignalState aspect);

/* Manual (operator) or automatic (interlocking) working. */
RwResult signal_controller_set_manual(RwId signal_id, bool manual);

/* In-service toggle. Withdrawing a signal forces it to danger. */
RwResult signal_controller_set_in_service(RwId signal_id, bool in_service);

/* Lookups. */
RwResult signal_controller_get_aspect(RwId signal_id, SignalState *aspect);
RwResult signal_controller_get_info(RwId signal_id, RwSignalInfo *info);
int signal_controller_count(void);
RwResult signal_controller_get_at(int index, RwSignalInfo *info);

/* True when the aspect permits movement. */
bool signal_controller_is_proceed(SignalState aspect);

/* Drive every signal in the diagram to danger (emergency / fail-safe). */
void signal_controller_all_to_danger(const char *reason);

/* Auto-signalling: clear or replace aspects according to the routes that are
 * set. Called by the interlocking each tick. */
void signal_controller_apply_route_aspects(void);

/* Interlocking helpers - used by interlock.c. */
void signal_controller_hold_for_route(RwId signal_id, RwId route_id);
void signal_controller_release_from_route(RwId signal_id);

#endif /* SIGNAL_CONTROLLER_H */
