/* ==========================================================================
 * persist.h - 💾 Save and restore the railway.
 *
 * RailControl had no persistence. The terminal advertised `SAVE <path>` and
 * `LOAD <path>`, and both printed a friendly confirmation while writing and
 * reading precisely nothing - the worst kind of stub, because a script that
 * relies on it appears to work and silently loses the state.
 *
 * This module makes those commands real. It writes a complete snapshot of the
 * engine to a file and reads it back.
 *
 * WHAT IS SAVED, AND WHAT IS DELIBERATELY NOT
 * -------------------------------------------
 *   Saved:
 *     - the layout: tracks, points, signals and sensors, with their geometry
 *     - the fleet: every train, and every car in its formation
 *     - the running state: speed, position, aspect, occupancy, which routes
 *       are set, and which trains have authority
 *
 *   Not saved:
 *     - the event log. It is a record of what happened, not of what is; a
 *       reloaded file that replayed yesterday's alarms would be misleading.
 *     - live sessions and the authentication log. Those belong to the account
 *       store, which has its own file and its own retention rules.
 *
 * THE FORMAT IS THE INTERCHANGE FORMAT
 * ------------------------------------
 * The snapshot is the same JSON shape as the hand-written layout and fleet
 * files in data/. One format, one loader: a layout edited by hand and a state
 * saved from a running panel take the same path through the code, so the two
 * cannot drift apart.
 *
 * SAFETY
 * ------
 * A restore is a state change like any other. It is permission gated, it is
 * written to the event log, and it validates what it reads: a snapshot that
 * names a track that does not exist is refused rather than half-applied. A
 * partially loaded layout is exactly the situation an interlocking must never
 * be left in.
 * ========================================================================== */
#ifndef PERSIST_H
#define PERSIST_H

#include "railway_types.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define RC_PERSIST_MAX_PATH 260
#define RC_PERSIST_MAX_TEXT 256

    /* --------------------------------------------------------------------------
     * What a snapshot covers
     * -------------------------------------------------------------------------- */
    typedef enum
    {
        RC_SAVE_LAYOUT = 1 << 0,    /* tracks, points, signals, sensors */
        RC_SAVE_FLEET = 1 << 1,     /* trains and their car formations */
        RC_SAVE_RUNNING = 1 << 2,   /* aspects, occupancy, routes, authority */
        RC_SAVE_EVENTS = 1 << 3,    /* the event log - off by default */
        RC_SAVE_TIMETABLE = 1 << 4, /* the working timetable */

        /* The sensible default: everything that describes the railway as it is. */
        RC_SAVE_STANDARD = RC_SAVE_LAYOUT | RC_SAVE_FLEET | RC_SAVE_RUNNING
    } RcSaveScope;

    /* --------------------------------------------------------------------------
     * Result of a save or load
     * -------------------------------------------------------------------------- */
    typedef struct
    {
        bool ok;
        char path[RC_PERSIST_MAX_PATH];
        char message[RC_PERSIST_MAX_TEXT]; /* operator-facing explanation */

        /* What actually moved, for the confirmation line. */
        int tracks;
        int points;
        int signals;
        int sensors;
        int trains;
        int cars;
        int routes;
    } RcPersistResult;

    const char *rc_persist_last_error(void);

    /* --------------------------------------------------------------------------
     * Saving
     * -------------------------------------------------------------------------- */

    /** Write a snapshot with the standard scope. */
    RwResult rc_save_snapshot(const char *path, RcPersistResult *out);

    /** Write a snapshot with an explicit scope. */
    RwResult rc_save_snapshot_scoped(const char *path, RcSaveScope scope,
                                     RcPersistResult *out);

    /** Write just the layout, in the same shape as data/layout.skeffield.json. */
    RwResult rc_save_layout(const char *path, RcPersistResult *out);

    /** Write just the fleet, in the same shape as data/fleet.json. */
    RwResult rc_save_fleet(const char *path, RcPersistResult *out);

    /* --------------------------------------------------------------------------
     * Loading
     *
     * A load REPLACES the named parts of the engine. It validates the whole file
     * before applying any of it: a snapshot that references a track which does not
     * exist is refused intact, never half-applied.
     * -------------------------------------------------------------------------- */

    /** Restore a snapshot. The scope selects which sections to apply. */
    RwResult rc_load_snapshot(const char *path, RcSaveScope scope, RcPersistResult *out);

    /** Restore the layout section only, then rebuild the engine's derived state. */
    RwResult rc_load_layout(const char *path, RcPersistResult *out);

    /** Restore the fleet section only. */
    RwResult rc_load_fleet(const char *path, RcPersistResult *out);

    /* --------------------------------------------------------------------------
     * Inspection and maintenance
     * -------------------------------------------------------------------------- */

    /** Read a snapshot's header without applying it, so a caller can show what a
     *  file contains before committing to loading it. */
    RwResult rc_persist_inspect(const char *path, RcPersistResult *out);

    /** True when the path names an existing, readable file. */
    bool rc_persist_file_exists(const char *path);

    /** Default snapshot path for this build: data/snapshot.json under the
     *  directory the program was started from. */
    const char *rc_persist_default_path(void);

/* --------------------------------------------------------------------------
 * Timetable  (⏱️)
 *
 * The working timetable is the schedule the railway is meant to run to:
 * which service calls at which platform, at what time, for how long.
 *
 * It is stored separately from the running state because it describes intent,
 * not observation. A restored snapshot of a train standing in a platform does
 * not tell you whether it was on time; the timetable is what says when it
 * should have been there.
 * -------------------------------------------------------------------------- */
#define RC_TIMETABLE_MAX_SERVICES 64
#define RC_TIMETABLE_MAX_CALLS 12
#define RC_TIMETABLE_MAX_PLATFORMS 16

    typedef struct
    {
        char station[RW_MAX_NAME];
        int platform;   /* 1-based; 0 when there is no platform */
        char arrive[8]; /* "HH:MM" */
        char depart[8]; /* "HH:MM" */
        int dwell_seconds;
        char booking[RW_MAX_NAME]; /* the route id the call expects */
    } RcTimetableCall;

    typedef struct
    {
        char headcode[RW_MAX_NAME];
        char description[RW_MAX_TEXT];
        char service_class[RW_MAX_NAME]; /* passenger / freight / ... */
        char origin[RW_MAX_NAME];
        char destination[RW_MAX_NAME];
        int priority; /* 1..10, higher runs first */
        float planned_speed_kph;
        RcTimetableCall calls[RC_TIMETABLE_MAX_CALLS];
        int call_count;

        /* Observed against planned, filled in as the day runs. */
        int minutes_late;
        bool complete;
    } RcTimetableService;

    typedef struct
    {
        int id;
        char name[RW_MAX_NAME];
        int track;
        float length_m;
        char direction[RW_MAX_NAME]; /* up / down */
    } RcTimetablePlatform;

    typedef struct
    {
        int minimum_headway_seconds;
        int platform_reoccupation_seconds;
        int dwell_tolerance_seconds;
        int late_threshold_seconds;
        char regulation_strategy[RW_MAX_NAME];
    } RcTimetableRules;

    typedef struct
    {
        char station[RW_MAX_NAME];
        char valid_from[16];
        char valid_to[16];
        RcTimetablePlatform platforms[RC_TIMETABLE_MAX_PLATFORMS];
        int platform_count;
        RcTimetableService services[RC_TIMETABLE_MAX_SERVICES];
        int service_count;
        RcTimetableRules rules;
    } RcTimetable;

    /** The loaded timetable, or NULL when none has been loaded. */
    RcTimetable *rc_timetable(void);

    /** Load the working timetable. Replaces any timetable already held. */
    RwResult rc_timetable_load(const char *path, RcPersistResult *out);

    /** Write the working timetable back out. */
    RwResult rc_timetable_save(const char *path, RcPersistResult *out);

    /** Find a service by headcode, or -1. */
    int rc_timetable_find(const char *headcode);

    /** A one-line status for the service: "1A34  arr 08:15  plat 1  on time". */
    RwResult rc_timetable_describe(int index, char *buffer, size_t size);

    /** Render the whole timetable as text, for the terminal's LIST TIMETABLE. */
    RwResult rc_timetable_format(char *buffer, size_t size);

    /** Empty the timetable. */
    void rc_timetable_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* PERSIST_H */
