/* ==========================================================================
 * interlocking.h - 🔒 Interlocking logic.
 *
 * The safety core. It is the only component allowed to grant movement
 * authority, and it does so only after proving the route is safe:
 *
 *   1. route exists and is in service
 *   2. no conflicting route is already set over any shared track or point
 *   3. every point on the path is available and can reach its required lie
 *   4. every track on the path is proven clear (UNKNOWN fails safe)
 *   5. the exit signal exists or the route ends in a safe place
 *   6. an emergency stop is not active
 *
 * The result of a successful proof is a lock: the points are locked in the
 * required lie, the entry signal clears, and the route is marked LOCKED.
 * On refusal nothing is changed - the interlocking never leaves a route in a
 * partially set condition.
 *
 * This is a deliberately conservative, easily auditable implementation. It is
 * NOT certified for real signalling: see the safety notice in the README.
 * ========================================================================== */
#ifndef INTERLOCKING_H
#define INTERLOCKING_H

#include "railway_types.h"

/* Prepare the interlocking and its configuration. */
void interlocking_init(void);

/* Full safety proof for a route, without changing anything.
 * On refusal `reason` is filled with the operator-facing explanation. */
RwResult interlocking_validate_route(RwId route_id, char *reason, size_t reason_size);

/* Prove and SET a route atomically. On success the points are locked, the
 * entry signal is cleared and the route state becomes ROUTE_LOCKED.
 * On failure the layout is untouched. */
RwResult interlocking_request_route(RwId route_id);

/* Set a route and give authority to the first train waiting at its entry
 * signal. */
RwResult interlocking_set_route_and_dispatch(RwId route_id);

/* Cancel a route. Refused while a train is inside it. */
RwResult interlocking_cancel_route(RwId route_id);

/* Are two routes mutually exclusive? Fills `reason` when they conflict. */
RwResult interlocking_check_conflicts(RwId route_id, RwId *other_route,
                                      char *reason, size_t reason_size);

/* True when the two routes share a track or a point. */
bool interlocking_routes_conflict(RwId route_a, RwId route_b);

/* Per-tick: release routes a train has fully cleared, re-apply aspects. */
void interlocking_tick(unsigned delta_ms);

/* Emergency handling: drop every route to danger and hold all points. */
void interlocking_emergency_drop(const char *reason);

/* Find the route protected by a signal, or RW_ID_NONE. */
RwId interlocking_route_for_signal(RwId signal_id);

/* Route lookup by name, returning the internal id or RW_ID_NONE. */
RwId interlocking_find_route(const char *name);

/* Count of currently locked routes. */
int interlocking_locked_route_count(void);

#endif /* INTERLOCKING_H */
