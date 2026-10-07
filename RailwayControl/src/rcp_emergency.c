/* ==========================================================================
 * rcp_emergency.c - 🛑 Emergency stop.
 *
 * This is the single most important operation in the whole program, so it is
 * written to be unconditional and fast:
 *
 *   - every signal goes to DANGER, immediately, with no proof required
 *   - every train applies the EMERGENCY brake
 *   - every route is dropped
 *   - every point is held where it stands
 *
 * Nothing can refuse it. There is no interlock on the way in. Even a script
 * with no privileges can trip it, because refusing to stop is never the safe
 * answer.
 *
 * Clearing it is the opposite: it is deliberately hard. You cannot clear the
 * emergency while a train is still moving, because doing so would let a train
 * accept a signal before the signaller has confirmed the area is safe.
 * ========================================================================== */
#include "railway_internal.h"
#include "railcontrol.h"

#include "signal_controller.h"
#include "switch_controller.h"
#include "train_controller.h"
#include "interlocking.h"

#include <stdio.h>
#include <string.h>

void rw_emergency_engage(const char *reason)
{
    RailwayEngine *e = rw_engine();
    const char *text = (reason != NULL && reason[0] != '\0')
                           ? reason
                           : "Emergency stop - reason not given";

    if (e == NULL || !e->initialized)
    {
        return;
    }

    /* Already tripped: still re-apply the safe state. Idempotent by design,
     * because a second emergency stop request must never be ignored. */
    const bool already = e->emergency;

    e->emergency = true;
    e->system_state = SYSTEM_EMERGENCY;
    e->emergency_at_ms = (unsigned)e->clock_ms;
    snprintf(e->emergency_reason, sizeof(e->emergency_reason), "%s", text);
    if (!already)
    {
        e->emergency_stops++;
    }

    /* 1. Every signal to danger. The safe direction is never refused. */
    signal_controller_all_to_danger(text);

    /* 2. Every route dropped. */
    interlocking_emergency_drop(text);

    /* 3. Every train emergency braked. */
    train_controller_emergency_all(text);

    /* 4. Points are held (not moved) - see the note below. */
    rw_event(SEVERITY_CRITICAL, "EMERGENCY",
             "*** EMERGENCY STOP %s *** %s",
             already ? "RE-ASSERTED" : "TRIPPED", text);
    rw_event(SEVERITY_CRITICAL, "EMERGENCY",
             "All signals at danger, all routes dropped, all trains emergency braked. "
             "Points are held in their last proven lie. Use CLEAREMG when the "
             "area is confirmed safe.");

    if (!already)
    {
        fprintf(stderr,
                "\n*** EMERGENCY STOP ***\n%s\n"
                "All signals are at danger and every train has been stopped.\n\n",
                text);
        fflush(stderr);
    }
}

/* --------------------------------------------------------------------------
 * Can the emergency be cleared?
 *
 * Required: every train at a stand. That is the whole condition, and it is
 * deliberately strict. A section that is still occupied by a train is fine -
 * the train is standing in it - but nothing may be moving.
 * -------------------------------------------------------------------------- */
bool rw_emergency_can_clear(void)
{
    RailwayEngine *e = rw_engine();
    RcSettings *settings = rc_settings();

    if (!e->emergency)
    {
        return true;
    }
    if (settings->emergency_requires_stop && !train_controller_all_stopped())
    {
        return false;
    }
    return true;
}
