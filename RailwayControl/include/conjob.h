/* ==========================================================================
 * conjob.h - ⚙️ CONJOB: automated control jobs.
 *
 * A CONJOB is a rule the interlocking runs continuously: WHEN a condition is
 * true, DO a sequence of actions, with a priority, a cooldown and a one-shot
 * or repeating behaviour. Think of it as a small, safe, auditable scripting
 * layer over the control API - it cannot bypass any interlocking proof,
 * because every action goes through the normal API and can therefore be
 * refused.
 *
 * Typical jobs:
 *   - "hold the 08:15 at signal S3 if platform 2 is occupied"
 *   - "auto-set route R2 when a train is waiting and the path is clear"
 *   - "trip the emergency stop if two trains enter the same section"
 *   - "return points to NORMAL after the last passenger train has cleared"
 *   - "log a warning if a track circuit reads UNKNOWN for over 30 seconds"
 *
 * Conditions and actions are small enums rather than callbacks so a job can
 * be described in a config file, logged verbatim, and reasoned about without
 * executing arbitrary code.
 * ========================================================================== */
#ifndef CONJOB_H
#define CONJOB_H

#include "railway_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define CONJOB_MAX_JOBS 128
#define CONJOB_MAX_ACTIONS 8
#define CONJOB_MAX_NAME 32
#define CONJOB_MAX_TEXT 192

    /* --------------------------------------------------------------------------
     * Trigger: WHEN does the job evaluate?
     * -------------------------------------------------------------------------- */
    typedef enum
    {
        CONJOB_TRIGGER_TICK = 0, /* every evaluation cycle */
        CONJOB_TRIGGER_TRACK_OCCUPIED,
        CONJOB_TRIGGER_TRACK_CLEAR,
        CONJOB_TRIGGER_SIGNAL_AT_DANGER,
        CONJOB_TRIGGER_SIGNAL_CLEAR,
        CONJOB_TRIGGER_TRAIN_WAITING,
        CONJOB_TRIGGER_TRAIN_STOPPED,
        CONJOB_TRIGGER_ROUTE_SET,
        CONJOB_TRIGGER_ROUTE_RELEASED,
        CONJOB_TRIGGER_CONFLICT_DETECTED,
        CONJOB_TRIGGER_EMERGENCY_ACTIVE,
        CONJOB_TRIGGER_SENSOR_TRIGGERED,
        CONJOB_TRIGGER_SENSOR_FAILED,
        CONJOB_TRIGGER_INTERVAL_SECONDS
    } ConJobTrigger;

    const char *conjob_trigger_name(ConJobTrigger trigger);
    ConJobTrigger conjob_trigger_parse(const char *name);

    /* --------------------------------------------------------------------------
     * Condition: an additional guard that must hold for the job to fire.
     * -------------------------------------------------------------------------- */
    typedef enum
    {
        CONJOB_COND_NONE = 0,
        CONJOB_COND_TRACK_CLEAR,
        CONJOB_COND_TRACK_OCCUPIED,
        CONJOB_COND_ROUTE_FREE, /* no conflicting route set */
        CONJOB_COND_ROUTE_NOT_SET,
        CONJOB_COND_SIGNAL_AT_DANGER,
        CONJOB_COND_SIGNAL_CLEAR,
        CONJOB_COND_TRAIN_STOPPED,
        CONJOB_COND_TRAIN_RUNNING,
        CONJOB_COND_NO_EMERGENCY,
        CONJOB_COND_EMERGENCY_ACTIVE,
        CONJOB_COND_POINT_IN_POSITION,
        CONJOB_COND_ALARM_COUNT_ABOVE
    } ConJobCondition;

    const char *conjob_condition_name(ConJobCondition condition);
    ConJobCondition conjob_condition_parse(const char *name);

    /* --------------------------------------------------------------------------
     * Action: WHAT does the job do? Every action maps onto a public API call, so
     * it is subject to the same interlocking checks and can be refused.
     * -------------------------------------------------------------------------- */
    typedef enum
    {
        CONJOB_ACTION_LOG_MESSAGE = 0,
        CONJOB_ACTION_SET_SIGNAL, /* arg0 = aspect */
        CONJOB_ACTION_SET_SIGNAL_MANUAL,
        CONJOB_ACTION_SET_SWITCH, /* arg0 = position */
        CONJOB_ACTION_RELEASE_SWITCH,
        CONJOB_ACTION_REQUEST_ROUTE,
        CONJOB_ACTION_CANCEL_ROUTE,
        CONJOB_ACTION_SET_ROUTE_AND_DISPATCH,
        CONJOB_ACTION_HOLD_TRAIN,
        CONJOB_ACTION_RELEASE_TRAIN,
        CONJOB_ACTION_SET_TRAIN_SPEED, /* arg0 = km/h * 10 */
        CONJOB_ACTION_STOP_TRAIN,
        CONJOB_ACTION_EMERGENCY_STOP,
        CONJOB_ACTION_CLEAR_EMERGENCY,
        CONJOB_ACTION_FAIL_TRACK_CIRCUIT,
        CONJOB_ACTION_ACKNOWLEDGE_ALARMS,
        CONJOB_ACTION_SNAPSHOT /* write a state snapshot to disk */
    } ConJobAction;

    const char *conjob_action_name(ConJobAction action);
    ConJobAction conjob_action_parse(const char *name);

    typedef struct
    {
        ConJobAction action;
        RwId target; /* asset id the action applies to */
        int arg0;    /* action specific (aspect, position, speed) */
        int arg1;
        char text[CONJOB_MAX_TEXT]; /* message / reason to log */
    } ConJobStep;

    typedef struct
    {
        RwId id;
        char name[CONJOB_MAX_NAME];
        char description[CONJOB_MAX_TEXT];
        bool enabled;
        int priority; /* higher runs first, 1..10 */
        ConJobTrigger trigger;
        RwId trigger_target; /* asset the trigger watches */
        int trigger_arg;     /* e.g. interval seconds, alarm count */
        ConJobCondition condition;
        RwId condition_target;
        int condition_arg0;
        ConJobStep actions[CONJOB_MAX_ACTIONS];
        size_t action_count;
        bool one_shot;
        bool fired;           /* set once a one-shot has run */
        unsigned cooldown_ms; /* minimum time between firings */
        unsigned last_fired_ms;
        unsigned fire_count;
        unsigned refuse_count; /* actions refused by the interlocking */
        unsigned last_evaluated_ms;
        bool evaluating; /* re-entrancy guard */
    } ConJob;

    /* --------------------------------------------------------------------------
     * Registry / engine settings
     * -------------------------------------------------------------------------- */
    typedef struct
    {
        bool enabled;                    /* global master switch */
        bool allow_emergency_actions;    /* may a job trip the emergency stop? */
        bool dry_run;                    /* evaluate and log, change nothing */
        unsigned evaluation_interval_ms; /* how often jobs are evaluated */
        unsigned max_fires_per_minute;   /* rate limit across all jobs */
        int log_level;                   /* 0 quiet, 1 normal, 2 verbose */
    } ConJobSettings;

    /* ==========================================================================
     * Public CONJOB API
     * ========================================================================== */

    /* Build the registry and install the default job set. */
    /* Install the day-one automation rule set. */
    void conjob_install_defaults(void);

    void conjob_init(void);
    void conjob_shutdown(void);

    /* Register a job. Returns the new job id, or RW_ID_NONE on failure.
     * The job is copied - the caller keeps ownership of its own struct. */
    RwId conjob_add(const ConJob *job);

    /* Add a simple single-action job in one call - the common case. */
    RwId conjob_add_simple(const char *name, ConJobTrigger trigger, RwId trigger_target,
                           ConJobAction action, RwId action_target, int arg0,
                           const char *text);

    /* Remove a job and free its slot. Cancels its schedule. */
    RwResult conjob_remove(RwId job_id);

    /* Enable / disable one job or all of them. */
    RwResult conjob_set_enabled(RwId job_id, bool enabled);
    int conjob_enable_all(bool enabled);

    /* Fire a job immediately, ignoring the trigger (but not the safety checks
     * or the cooldown unless `force`). */
    RwResult conjob_trigger_now(RwId job_id, bool force);

    /* Per-tick evaluation. Called by the engine tick after the controllers. */
    void conjob_tick(unsigned delta_ms);

    /* Settings  (⚙️) */
    ConJobSettings *conjob_settings(void);
    RwResult conjob_apply_settings(const ConJobSettings *settings);
    const char *conjob_settings_summary(void);

    /* Inspection for the GUI and the CLI. */
    int conjob_count(void);
    RwResult conjob_get(RwId job_id, ConJob *out);
    RwResult conjob_get_at(int index, ConJob *out);
    RwId conjob_find(const char *name);

    /* Statistics for the settings panel. */
    typedef struct
    {
        int jobs_total;
        int jobs_enabled;
        unsigned total_fires;
        unsigned total_refusals;
        unsigned fires_this_minute;
        unsigned evaluations;
    } ConJobStats;

    RwResult conjob_get_stats(ConJobStats *stats);

    /* Persistence: write / read the registry as a small INI-style file. */
    RwResult conjob_save(const char *path);
    RwResult conjob_load(const char *path);

    /* Human readable one-line rendering used by the GUI list:
     *   "AUTO-R2  [on]  prio 5  fires 3  WHEN train-waiting R2 AND route-free" */
    RwResult conjob_describe(RwId job_id, char *buffer, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* CONJOB_H */
