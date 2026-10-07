#ifndef RAILCONTROL_H
#define RAILCONTROL_H

#include <stdbool.h>
#include <stddef.h>

/* ==========================================================================
 * railcontrol.h - Project identity and configuration for RailControl.
 *
 *   RailControl
 *   Railway Traffic Management & Interlocking Simulator
 *   (specification name: Railway Traffic Control & Interlocking System, RTCIS)
 *
 * Where this sits in the taxonomy of real railway software
 * -------------------------------------------------------
 *   TMS   Railway Traffic Management System  - timetable, regulation, traffic
 *   TCS   Train Control System               - coordinates train operations
 *   CBI   Computer-Based Interlocking        - signals + points, no conflicts
 *   EI    Electronic Interlocking            - synonym for CBI in practice
 *   CTC   Centralised Traffic Control        - central signal/point/train control
 *   SCADA Supervisory Control & Data Acq.    - infrastructure monitoring
 *   ATC   Automatic Train Control            - speed supervision / ATP
 *   RMS   Railway Management System          - broader operations
 *
 * RailControl occupies the CBI + CTC core with a TMS/SCADA style supervisory
 * layer on top:
 *
 *   - CBI/EI : interlocking.c  - the route proof, point locking, aspect logic
 *   - CTC    : railway_api.c   - one central control point for the whole area
 *   - SCADA  : sensor + event   - field inputs, alarms, event log
 *   - TMS    : conjob + trains  - automated regulation of train movements
 *   - ATC    : train_controller - speed supervision against signalling
 *
 * SAFETY NOTICE
 * -------------
 * RailControl is a SIMULATOR and training/demonstration prototype. It is NOT
 * a certified interlocking and must not be connected to real signalling.
 * A deployable Computer-Based Interlocking must satisfy:
 *
 *   EN 50126  RAMS - reliability, availability, maintainability, safety
 *   EN 50128  Software for railway control and protection systems
 *   EN 50129  Safety-related electronic systems for signalling
 *   EN 50159  Safety-related communication in transmission systems
 *   IEC 61508 SIL 4 - functional safety, and in particular:
 *     - fail-safe / fail-silent hardware with 2-out-of-2 or 2-out-of-3 voting
 *     - formal verification of the interlocking truth tables
 *     - independent safety assessor sign-off
 *     - proven-in-use operating system and certified compiler toolchain
 *     - rigorous change control for every line of safety-related code
 *
 * None of those properties can be claimed for this code. It is deliberately
 * written so the safety logic is small, total and auditable, so that it can
 * be reviewed as a teaching example - not so that it can be deployed.
 * ========================================================================== */

#define RC_NAME            "RailControl"
#define RC_LONG_NAME       "Railway Traffic Management & Interlocking Simulator"
#define RC_SPEC_NAME       "Railway Traffic Control & Interlocking System"
#define RC_SPEC_ABBREV     "RTCIS"
#define RC_VERSION_MAJOR   1
#define RC_VERSION_MINOR   0
#define RC_VERSION_PATCH   0
#define RC_VERSION_STRING  "1.0.0"
#define RC_BUILD_DATE      __DATE__

/* Status line / console banner text. */
#define RC_BANNER \
    RC_NAME " " RC_VERSION_STRING " - " RC_LONG_NAME

#define RC_COPYRIGHT \
    "RailControl is a simulator. NOT CERTIFIED FOR REAL SIGNALLING."

/* --------------------------------------------------------------------------
 * Safety architecture the simulator models (and the real thing would need).
 * --------------------------------------------------------------------------
 * The engine enforces these invariants on every tick; a violation is logged
 * as CRITICAL and the relevant asset is withdrawn from service:
 *
 *   INV-1  A signal may only show proceed while a route is set for it and the
 *          section ahead is proven clear.
 *   INV-2  Two routes that share any track or point shall never be set
 *          simultaneously.
 *   INV-3  A point shall not move while the section it occupies is occupied.
 *   INV-4  A point locked by a route shall not be moved until released.
 *   INV-5  An UNKNOWN or failed track circuit shall be treated as occupied.
 *   INV-6  On emergency stop every signal shall display danger and every
 *          train shall apply the emergency brake.
 *   INV-7  A route is released only when the train has cleared it, or by an
 *          explicit, audited cancel with the path proven clear.
 *   INV-8  Movement authority is granted only through the interlocking.
 * -------------------------------------------------------------------------- */
enum RcInvariant {
    RC_INV_SIGNAL_REQUIRES_ROUTE   = 1,
    RC_INV_ROUTES_EXCLUSIVE        = 2,
    RC_INV_NO_MOVE_UNDER_TRAIN     = 3,
    RC_INV_POINT_LOCK_HELD         = 4,
    RC_INV_UNKNOWN_IS_OCCUPIED     = 5,
    RC_INV_EMERGENCY_ALL_DANGER    = 6,
    RC_INV_RELEASE_ON_CLEAR        = 7,
    RC_INV_AUTHORITY_VIA_INTERLOCK = 8
};

const char *rc_invariant_text(enum RcInvariant invariant);

/* --------------------------------------------------------------------------
 * Program-wide runtime settings (the "Settings" page in the GUI).
 * -------------------------------------------------------------------------- */
typedef struct {
    /* Simulation */
    int   tick_interval_ms;          /* GUI timer period, default 100 */
    int   time_scale;                /* 1..20 x real time, default 1 */
    bool  auto_signal_routes;        /* interlocking clears signals itself */
    bool  auto_release_routes;       /* release routes once cleared */

    /* Safety */
    bool  fail_safe_unknown;         /* INV-5, always on in a real system */
    bool  allow_call_on;             /* permit call-on into an occupied platform */
    bool  block_on_circuit_failure;  /* refuse routes over a failed circuit */
    bool  emergency_requires_stop;   /* cannot clear while trains move */

    /* Display */
    bool  show_grid;
    bool  show_sensor_ids;
    bool  show_train_labels;
    bool  animate_points;
    int   diagram_zoom_percent;

    /* Logging */
    bool  log_to_file;
    char  log_path[260];
    bool  log_info_events;
    int   event_retention;           /* max events kept for display */

    /* Automation */
    bool  conjobs_enabled;           /* CONJOB registry master switch */
    bool  conjob_dry_run;
    bool  scripting_enabled;         /* Lua object layer */
} RcSettings;

/* The single settings instance, shared by the GUI and the engine. */
RcSettings *rc_settings(void);

/* Validate and clamp a settings structure in place. Returns the number of
 * fields that had to be corrected. */
int rc_settings_normalise(RcSettings *settings);

/* Persist / restore to a small INI-style file. Returns 0 on success. */
int rc_settings_save(const RcSettings *settings, const char *path);
int rc_settings_load(RcSettings *settings, const char *path);

#endif /* RAILCONTROL_H */
