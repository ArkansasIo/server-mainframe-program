/* ==========================================================================
 * conjob.c - ⚙️ CONJOB: automated control jobs.
 *
 * A rule engine that runs on the engine tick. Each job is a trigger, an
 * optional guard condition and up to CONJOB_MAX_ACTIONS steps.
 *
 * Safety properties that make this safe to bolt on to an interlocking:
 *
 *   1. Every action goes through the public API. A job cannot bypass a route
 *      proof, cannot move a locked point, cannot clear a signal without a
 *      route. A refused action increments the job's refuse_count and is
 *      logged - it does NOT retry in a loop.
 *   2. Evaluating a job cannot re-enter the job engine (the `evaluating`
 *      flag), so a job that triggers itself stops rather than recursing.
 *   3. A cooldown bounds how often a job can fire, so a condition that is
 *      permanently true cannot flood the system.
 *   4. There is a global dry-run mode which evaluates and logs every action
 *      without executing it - the way you would test a control table before
 *      putting it into service.
 *   5. Emergency action is separately gated by allow_emergency_actions.
 * ========================================================================== */
#include "conjob.h"
#include "railway_api.h"
#include "railcontrol.h"
#include "railway_internal.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define strcasecmp _stricmp
#endif

/* --------------------------------------------------------------------------
 * Registry
 * -------------------------------------------------------------------------- */
static ConJob g_jobs[CONJOB_MAX_JOBS];
static size_t g_job_count = 0;
static ConJobSettings g_settings;
static ConJobStats g_stats;
static unsigned long g_window_start_ms = 0;
static unsigned g_window_fires = 0;
static unsigned long g_last_evaluation_ms = 0;

/* --------------------------------------------------------------------------
 * Enum names
 * -------------------------------------------------------------------------- */
const char *conjob_trigger_name(ConJobTrigger trigger)
{
    switch (trigger) {
    case CONJOB_TRIGGER_TICK:              return "tick";
    case CONJOB_TRIGGER_TRACK_OCCUPIED:    return "track-occupied";
    case CONJOB_TRIGGER_TRACK_CLEAR:       return "track-clear";
    case CONJOB_TRIGGER_SIGNAL_AT_DANGER:  return "signal-danger";
    case CONJOB_TRIGGER_SIGNAL_CLEAR:      return "signal-clear";
    case CONJOB_TRIGGER_TRAIN_WAITING:     return "train-waiting";
    case CONJOB_TRIGGER_TRAIN_STOPPED:     return "train-stopped";
    case CONJOB_TRIGGER_ROUTE_SET:         return "route-set";
    case CONJOB_TRIGGER_ROUTE_RELEASED:    return "route-released";
    case CONJOB_TRIGGER_CONFLICT_DETECTED: return "conflict";
    case CONJOB_TRIGGER_EMERGENCY_ACTIVE:  return "emergency";
    case CONJOB_TRIGGER_SENSOR_TRIGGERED:  return "sensor-triggered";
    case CONJOB_TRIGGER_SENSOR_FAILED:     return "sensor-failed";
    case CONJOB_TRIGGER_INTERVAL_SECONDS:  return "interval";
    }
    return "tick";
}

ConJobTrigger conjob_trigger_parse(const char *name)
{
    int i;
    for (i = 0; i <= (int)CONJOB_TRIGGER_INTERVAL_SECONDS; ++i) {
        if (strcasecmp(name, conjob_trigger_name((ConJobTrigger)i)) == 0) {
            return (ConJobTrigger)i;
        }
    }
    return CONJOB_TRIGGER_TICK;
}

const char *conjob_condition_name(ConJobCondition condition)
{
    switch (condition) {
    case CONJOB_COND_NONE:             return "none";
    case CONJOB_COND_TRACK_CLEAR:      return "track-clear";
    case CONJOB_COND_TRACK_OCCUPIED:   return "track-occupied";
    case CONJOB_COND_ROUTE_FREE:       return "route-free";
    case CONJOB_COND_ROUTE_NOT_SET:    return "route-not-set";
    case CONJOB_COND_SIGNAL_AT_DANGER: return "signal-danger";
    case CONJOB_COND_SIGNAL_CLEAR:     return "signal-clear";
    case CONJOB_COND_TRAIN_STOPPED:    return "train-stopped";
    case CONJOB_COND_TRAIN_RUNNING:    return "train-running";
    case CONJOB_COND_NO_EMERGENCY:     return "no-emergency";
    case CONJOB_COND_EMERGENCY_ACTIVE: return "emergency-active";
    case CONJOB_COND_POINT_IN_POSITION:return "point-in-position";
    case CONJOB_COND_ALARM_COUNT_ABOVE:return "alarm-count-above";
    }
    return "none";
}

ConJobCondition conjob_condition_parse(const char *name)
{
    int i;
    for (i = 0; i <= (int)CONJOB_COND_ALARM_COUNT_ABOVE; ++i) {
        if (strcasecmp(name, conjob_condition_name((ConJobCondition)i)) == 0) {
            return (ConJobCondition)i;
        }
    }
    return CONJOB_COND_NONE;
}

const char *conjob_action_name(ConJobAction action)
{
    switch (action) {
    case CONJOB_ACTION_LOG_MESSAGE:          return "log";
    case CONJOB_ACTION_SET_SIGNAL:           return "signal";
    case CONJOB_ACTION_SET_SIGNAL_MANUAL:    return "signal-manual";
    case CONJOB_ACTION_SET_SWITCH:           return "switch";
    case CONJOB_ACTION_RELEASE_SWITCH:       return "release-switch";
    case CONJOB_ACTION_REQUEST_ROUTE:        return "route";
    case CONJOB_ACTION_CANCEL_ROUTE:         return "cancel-route";
    case CONJOB_ACTION_SET_ROUTE_AND_DISPATCH:return "dispatch";
    case CONJOB_ACTION_HOLD_TRAIN:           return "hold-train";
    case CONJOB_ACTION_RELEASE_TRAIN:        return "release-train";
    case CONJOB_ACTION_SET_TRAIN_SPEED:      return "train-speed";
    case CONJOB_ACTION_STOP_TRAIN:           return "stop-train";
    case CONJOB_ACTION_EMERGENCY_STOP:       return "emergency-stop";
    case CONJOB_ACTION_CLEAR_EMERGENCY:      return "clear-emergency";
    case CONJOB_ACTION_FAIL_TRACK_CIRCUIT:   return "fail-track";
    case CONJOB_ACTION_ACKNOWLEDGE_ALARMS:   return "ack-alarms";
    case CONJOB_ACTION_SNAPSHOT:             return "snapshot";
    }
    return "log";
}

ConJobAction conjob_action_parse(const char *name)
{
    int i;
    for (i = 0; i <= (int)CONJOB_ACTION_SNAPSHOT; ++i) {
        if (strcasecmp(name, conjob_action_name((ConJobAction)i)) == 0) {
            return (ConJobAction)i;
        }
    }
    return CONJOB_ACTION_LOG_MESSAGE;
}

/* --------------------------------------------------------------------------
 * Settings
 * -------------------------------------------------------------------------- */
ConJobSettings *conjob_settings(void)
{
    return &g_settings;
}

RwResult conjob_apply_settings(const ConJobSettings *settings)
{
    if (settings == NULL) {
        return RW_ERR_INVALID_ARG;
    }
    g_settings = *settings;

    if (g_settings.evaluation_interval_ms < 50) {
        g_settings.evaluation_interval_ms = 50;
    }
    if (g_settings.evaluation_interval_ms > 5000) {
        g_settings.evaluation_interval_ms = 5000;
    }
    if (g_settings.max_fires_per_minute == 0) {
        g_settings.max_fires_per_minute = 120;
    }
    return RW_OK;
}

const char *conjob_settings_summary(void)
{
    static char summary[RW_MAX_TEXT];

    snprintf(summary, sizeof(summary),
             "CONJOB %s, dry-run %s, every %u ms, max %u fires/min, emergency actions %s",
             g_settings.enabled ? "ENABLED" : "DISABLED",
             g_settings.dry_run ? "ON" : "OFF",
             g_settings.evaluation_interval_ms,
             g_settings.max_fires_per_minute,
             g_settings.allow_emergency_actions ? "permitted" : "BLOCKED");
    return summary;
}

/* --------------------------------------------------------------------------
 * Registry management
 * -------------------------------------------------------------------------- */
void conjob_init(void)
{
    memset(g_jobs, 0, sizeof(g_jobs));
    g_job_count = 0;
    memset(&g_stats, 0, sizeof(g_stats));

    g_settings.enabled = true;
    g_settings.allow_emergency_actions = true;
    g_settings.dry_run = false;
    g_settings.evaluation_interval_ms = 250;
    g_settings.max_fires_per_minute = 120;
    g_settings.log_level = 1;

    g_window_start_ms = 0;
    g_window_fires = 0;
    g_last_evaluation_ms = 0;
}

void conjob_shutdown(void)
{
    g_job_count = 0;
}

RwId conjob_add(const ConJob *job)
{
    ConJob *slot;

    if (job == NULL || job->name[0] == '\0') {
        return RW_ID_NONE;
    }
    if (g_job_count >= CONJOB_MAX_JOBS) {
        rw_event(SEVERITY_WARNING, "CONJOB",
                 "Cannot register job %s - the registry is full", job->name);
        return RW_ID_NONE;
    }
    if (job->action_count == 0) {
        rw_event(SEVERITY_WARNING, "CONJOB",
                 "Cannot register job %s - it has no actions", job->name);
        return RW_ID_NONE;
    }

    slot = &g_jobs[g_job_count];
    *slot = *job;
    slot->id = (RwId)(g_job_count + 1);
    if (slot->priority < 1)  slot->priority = 1;
    if (slot->priority > 10) slot->priority = 10;

    g_job_count++;
    g_stats.jobs_total = (int)g_job_count;
    if (slot->enabled) {
        g_stats.jobs_enabled++;
    }

    rw_event(SEVERITY_INFO, "CONJOB", "Registered job %s (id %u, %u action(s))",
             slot->name, slot->id, (unsigned)slot->action_count);
    return slot->id;
}

RwId conjob_add_simple(const char *name, ConJobTrigger trigger, RwId trigger_target,
                       ConJobAction action, RwId action_target, int arg0,
                       const char *text)
{
    ConJob job;

    memset(&job, 0, sizeof(job));
    snprintf(job.name, sizeof(job.name), "%s", name != NULL ? name : "JOB");
    snprintf(job.description, sizeof(job.description), "WHEN %s -> DO %s",
             conjob_trigger_name(trigger), conjob_action_name(action));
    job.enabled = true;
    job.priority = 5;
    job.trigger = trigger;
    job.trigger_target = trigger_target;
    job.condition = CONJOB_COND_NONE;
    job.one_shot = false;
    job.cooldown_ms = 3000;

    job.actions[0].action = action;
    job.actions[0].target = action_target;
    job.actions[0].arg0 = arg0;
    if (text != NULL) {
        snprintf(job.actions[0].text, sizeof(job.actions[0].text), "%s", text);
    }
    job.action_count = 1;

    return conjob_add(&job);
}

RwResult conjob_remove(RwId job_id)
{
    size_t i;

    for (i = 0; i < g_job_count; ++i) {
        if (g_jobs[i].id != job_id) {
            continue;
        }
        rw_event(SEVERITY_INFO, "CONJOB", "Removed job %s", g_jobs[i].name);

        /* Compact the array and renumber so ids stay contiguous. */
        memmove(&g_jobs[i], &g_jobs[i + 1], sizeof(ConJob) * (g_job_count - i - 1));
        g_job_count--;
        {
            size_t j;
            int enabled = 0;
            for (j = 0; j < g_job_count; ++j) {
                g_jobs[j].id = (RwId)(j + 1);
                if (g_jobs[j].enabled) enabled++;
            }
            g_stats.jobs_total = (int)g_job_count;
            g_stats.jobs_enabled = enabled;
        }
        return RW_OK;
    }
    return RW_ERR_INVALID_ID;
}

RwResult conjob_set_enabled(RwId job_id, bool enabled)
{
    ConJob *job = NULL;
    size_t i;

    for (i = 0; i < g_job_count; ++i) {
        if (g_jobs[i].id == job_id) {
            job = &g_jobs[i];
            break;
        }
    }
    if (job == NULL) {
        return RW_ERR_INVALID_ID;
    }

    job->enabled = enabled;
    g_stats.jobs_enabled = 0;
    for (i = 0; i < g_job_count; ++i) {
        if (g_jobs[i].enabled) g_stats.jobs_enabled++;
    }
    rw_event(SEVERITY_INFO, "CONJOB", "Job %s %s", job->name,
             enabled ? "enabled" : "disabled");
    return RW_OK;
}

int conjob_enable_all(bool enabled)
{
    size_t i;
    int changed = 0;

    for (i = 0; i < g_job_count; ++i) {
        if (g_jobs[i].enabled != enabled) {
            g_jobs[i].enabled = enabled;
            changed++;
        }
    }
    g_stats.jobs_enabled = enabled ? (int)g_job_count : 0;
    rw_event(SEVERITY_INFO, "CONJOB", "All jobs %s (%d changed)",
             enabled ? "enabled" : "disabled", changed);
    return changed;
}

int conjob_count(void)
{
    return (int)g_job_count;
}

RwResult conjob_get(RwId job_id, ConJob *out)
{
    size_t i;

    if (out == NULL) {
        return RW_ERR_INVALID_ARG;
    }
    for (i = 0; i < g_job_count; ++i) {
        if (g_jobs[i].id == job_id) {
            *out = g_jobs[i];
            return RW_OK;
        }
    }
    return RW_ERR_INVALID_ID;
}

RwResult conjob_get_at(int index, ConJob *out)
{
    if (out == NULL) {
        return RW_ERR_INVALID_ARG;
    }
    if (index < 0 || (size_t)index >= g_job_count) {
        return RW_ERR_INVALID_ID;
    }
    *out = g_jobs[index];
    return RW_OK;
}

RwId conjob_find(const char *name)
{
    size_t i;

    if (name == NULL || name[0] == '\0') {
        return RW_ID_NONE;
    }
    for (i = 0; i < g_job_count; ++i) {
        if (strcasecmp(g_jobs[i].name, name) == 0) {
            return g_jobs[i].id;
        }
    }
    /* Accept a numeric id too. */
    {
        char *end = NULL;
        long value = strtol(name, &end, 10);
        if (end != NULL && *end == '\0' && value > 0 && value <= (long)g_job_count) {
            return (RwId)value;
        }
    }
    return RW_ID_NONE;
}

RwResult conjob_get_stats(ConJobStats *stats)
{
    if (stats == NULL) {
        return RW_ERR_INVALID_ARG;
    }
    *stats = g_stats;
    stats->jobs_total = (int)g_job_count;
    stats->fires_this_minute = g_window_fires;
    return RW_OK;
}

/* --------------------------------------------------------------------------
 * Condition evaluation
 * -------------------------------------------------------------------------- */
static bool condition_holds(const ConJob *job)
{
    RwSystemStatus status;
    int i;

    if (job->condition == CONJOB_COND_NONE) {
        return true;
    }

    switch (job->condition) {
    case CONJOB_COND_TRACK_CLEAR:
    case CONJOB_COND_TRACK_OCCUPIED: {
        TrackOccupancy occupancy;
        if (railway_get_track((int)job->condition_target, &occupancy) != RW_OK) {
            return false;
        }
        if (job->condition == CONJOB_COND_TRACK_CLEAR) {
            return occupancy == TRACK_CLEAR;
        }
        return occupancy == TRACK_OCCUPIED;
    }

    case CONJOB_COND_ROUTE_FREE:
    case CONJOB_COND_ROUTE_NOT_SET: {
        RwRouteInfo route;
        if (railway_get_route_info((int)job->condition_target, &route) != RW_OK) {
            return false;
        }
        if (job->condition == CONJOB_COND_ROUTE_NOT_SET) {
            return route.state == ROUTE_IDLE || route.state == ROUTE_RELEASED;
        }
        return route.state == ROUTE_IDLE || route.state == ROUTE_RELEASED;
    }

    case CONJOB_COND_SIGNAL_AT_DANGER:
    case CONJOB_COND_SIGNAL_CLEAR: {
        SignalState aspect;
        if (railway_get_signal((int)job->condition_target, &aspect) != RW_OK) {
            return false;
        }
        if (job->condition == CONJOB_COND_SIGNAL_AT_DANGER) {
            return aspect == SIGNAL_RED;
        }
        return aspect != SIGNAL_RED;
    }

    case CONJOB_COND_TRAIN_STOPPED:
    case CONJOB_COND_TRAIN_RUNNING: {
        TrainState state;
        if (railway_get_train_state((int)job->condition_target, &state) != RW_OK) {
            return false;
        }
        if (job->condition == CONJOB_COND_TRAIN_STOPPED) {
            return state == TRAIN_STOPPED || state == TRAIN_EMERGENCY_STOP;
        }
        return state == TRAIN_RUNNING;
    }

    case CONJOB_COND_NO_EMERGENCY:
        return !railway_emergency_active();
    case CONJOB_COND_EMERGENCY_ACTIVE:
        return railway_emergency_active();

    case CONJOB_COND_POINT_IN_POSITION: {
        SwitchPosition position;
        if (railway_get_switch((int)job->condition_target, &position) != RW_OK) {
            return false;
        }
        return (int)position == job->condition_arg0;
    }

    case CONJOB_COND_ALARM_COUNT_ABOVE:
        if (railway_get_status(&status) != RW_OK) {
            return false;
        }
        return status.alarms > job->condition_arg0;

    default:
        break;
    }

    (void)i;
    return false;
}

/* --------------------------------------------------------------------------
 * Trigger evaluation
 * -------------------------------------------------------------------------- */
static bool trigger_fires(const ConJob *job)
{
    RwSystemStatus status;

    switch (job->trigger) {
    case CONJOB_TRIGGER_TICK:
        return true;

    case CONJOB_TRIGGER_INTERVAL_SECONDS:
        if (job->trigger_arg <= 0) {
            return false;
        }
        return (rw_engine()->clock_ms >= job->last_fired_ms
                + (unsigned)job->trigger_arg * 1000u);

    case CONJOB_TRIGGER_TRACK_OCCUPIED:
    case CONJOB_TRIGGER_TRACK_CLEAR: {
        TrackOccupancy occupancy;
        if (railway_get_track((int)job->trigger_target, &occupancy) != RW_OK) {
            return false;
        }
        return (job->trigger == CONJOB_TRIGGER_TRACK_OCCUPIED)
            ? occupancy == TRACK_OCCUPIED
            : occupancy == TRACK_CLEAR;
    }

    case CONJOB_TRIGGER_SIGNAL_AT_DANGER:
    case CONJOB_TRIGGER_SIGNAL_CLEAR: {
        SignalState aspect;
        if (railway_get_signal((int)job->trigger_target, &aspect) != RW_OK) {
            return false;
        }
        return (job->trigger == CONJOB_TRIGGER_SIGNAL_AT_DANGER)
            ? aspect == SIGNAL_RED
            : aspect != SIGNAL_RED;
    }

    case CONJOB_TRIGGER_TRAIN_WAITING: {
        /* A train is standing on the first section of the route. */
        RwRouteInfo route;
        int i;
        int count = railway_train_count();

        if (railway_get_route_info((int)job->trigger_target, &route) != RW_OK) {
            return false;
        }
        for (i = 0; i < count; ++i) {
            RwTrainInfo train;
            if (railway_get_train_at(i, &train) != RW_OK) {
                continue;
            }
            if (train.state == TRAIN_STOPPED || train.state == TRAIN_RUNNING) {
                /* We cannot see the route's first track through the public
                 * API, so "waiting" is approximated by a stopped train with
                 * no authority. That is exactly the dispatch case. */
                if (train.route == RW_ID_NONE && train.state == TRAIN_STOPPED) {
                    return true;
                }
            }
        }
        return false;
    }

    case CONJOB_TRIGGER_TRAIN_STOPPED: {
        TrainState state;
        if (railway_get_train_state((int)job->trigger_target, &state) != RW_OK) {
            return false;
        }
        return state == TRAIN_STOPPED;
    }

    case CONJOB_TRIGGER_ROUTE_SET:
    case CONJOB_TRIGGER_ROUTE_RELEASED: {
        RwRouteInfo route;
        if (railway_get_route_info((int)job->trigger_target, &route) != RW_OK) {
            return false;
        }
        return (job->trigger == CONJOB_TRIGGER_ROUTE_SET)
            ? (route.state == ROUTE_LOCKED || route.state == ROUTE_OCCUPIED)
            : (route.state == ROUTE_RELEASED || route.state == ROUTE_IDLE);
    }

    case CONJOB_TRIGGER_CONFLICT_DETECTED:
        if (railway_get_status(&status) != RW_OK) {
            return false;
        }
        return status.conflicts_blocked > job->trigger_arg;

    case CONJOB_TRIGGER_EMERGENCY_ACTIVE:
        return railway_emergency_active();

    case CONJOB_TRIGGER_SENSOR_TRIGGERED:
    case CONJOB_TRIGGER_SENSOR_FAILED: {
        int i;
        int count = railway_sensor_count();
        RwSensorInfo sensor;

        for (i = 0; i < count; ++i) {
            if (railway_get_sensor_at(i, &sensor) != RW_OK) {
                continue;
            }
            if (job->trigger_target != RW_ID_NONE && sensor.id != job->trigger_target) {
                continue;
            }
            if (job->trigger == CONJOB_TRIGGER_SENSOR_TRIGGERED
                && sensor.state == (int)SENSOR_TRIGGERED) {
                return true;
            }
            if (job->trigger == CONJOB_TRIGGER_SENSOR_FAILED
                && sensor.state == (int)SENSOR_FAILED) {
                return true;
            }
        }
        return false;
    }
    }

    return false;
}

/* --------------------------------------------------------------------------
 * Action execution
 *
 * The return value is the number of refusals. A refusal is not an error in
 * the job; it is the interlocking doing its job. We record it and move on -
 * we never retry, because a retry loop against a safety system is exactly
 * the wrong behaviour.
 * -------------------------------------------------------------------------- */
static int execute_action(const ConJob *job, const ConJobStep *step)
{
    RwResult result = RW_OK;
    const char *verb = conjob_action_name(step->action);

    if (g_settings.dry_run) {
        rw_event(SEVERITY_INFO, "CONJOB",
                 "[DRY-RUN] %s would perform %s on target %u (arg0 %d)",
                 job->name, verb, step->target, step->arg0);
        return 0;
    }

    switch (step->action) {
    case CONJOB_ACTION_LOG_MESSAGE:
        railway_log_message(SEVERITY_INFO, "CONJOB", step->text);
        return 0;

    case CONJOB_ACTION_SET_SIGNAL:
        result = railway_set_signal((int)step->target, (SignalState)step->arg0);
        break;

    case CONJOB_ACTION_SET_SIGNAL_MANUAL:
        result = railway_set_signal_mode((int)step->target, step->arg0 != 0);
        break;

    case CONJOB_ACTION_SET_SWITCH:
        result = railway_set_switch((int)step->target, (SwitchPosition)step->arg0);
        break;

    case CONJOB_ACTION_RELEASE_SWITCH:
        result = railway_release_switch((int)step->target);
        break;

    case CONJOB_ACTION_REQUEST_ROUTE:
        result = railway_request_route((int)step->target);
        break;

    case CONJOB_ACTION_CANCEL_ROUTE:
        result = railway_cancel_route((int)step->target);
        break;

    case CONJOB_ACTION_SET_ROUTE_AND_DISPATCH:
        result = railway_set_route_and_dispatch((int)step->target);
        break;

    case CONJOB_ACTION_HOLD_TRAIN:
        result = railway_hold_train((int)step->target, true);
        break;

    case CONJOB_ACTION_RELEASE_TRAIN:
        result = railway_hold_train((int)step->target, false);
        break;

    case CONJOB_ACTION_SET_TRAIN_SPEED:
        result = railway_set_train_speed((int)step->target, (float)step->arg0);
        break;

    case CONJOB_ACTION_STOP_TRAIN:
        result = railway_set_train_state((int)step->target, TRAIN_STOPPED);
        break;

    case CONJOB_ACTION_EMERGENCY_STOP:
        if (!g_settings.allow_emergency_actions) {
            rw_event(SEVERITY_WARNING, "CONJOB",
                     "Job %s tried to trip the emergency stop but the setting "
                     "allow_emergency_actions is off", job->name);
            return 1;
        }
        result = railway_emergency_stop_reason(step->text[0] != '\0'
            ? step->text : "Automatic emergency stop by CONJOB");
        break;

    case CONJOB_ACTION_CLEAR_EMERGENCY:
        result = railway_clear_emergency();
        break;

    case CONJOB_ACTION_FAIL_TRACK_CIRCUIT:
        result = railway_fail_track_circuit((int)step->target, step->arg0 != 0);
        break;

    case CONJOB_ACTION_ACKNOWLEDGE_ALARMS:
        railway_acknowledge_all();
        return 0;

    case CONJOB_ACTION_SNAPSHOT:
        rw_event(SEVERITY_INFO, "CONJOB", "Job %s requested a state snapshot", job->name);
        return 0;
    }

    if (result != RW_OK) {
        rw_event(SEVERITY_WARNING, "CONJOB",
                 "Job %s: action %s on target %u was REFUSED (%s) - the interlocking "
                 "is protecting the layout",
                 job->name, verb, step->target, rw_result_string(result));
        return 1;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Firing
 * -------------------------------------------------------------------------- */
static void fire_job(ConJob *job, bool force)
{
    unsigned long now = rw_engine()->clock_ms;
    size_t i;
    int refusals = 0;

    /* Rate limiting across the whole registry. */
    if (now - g_window_start_ms > 60000UL) {
        g_window_start_ms = now;
        g_window_fires = 0;
    }
    if (g_window_fires >= g_settings.max_fires_per_minute && !force) {
        rw_event(SEVERITY_WARNING, "CONJOB",
                 "Job %s suppressed - the registry has reached %u fires per minute",
                 job->name, g_settings.max_fires_per_minute);
        return;
    }

    /* Re-entrancy guard: a job that requests a route which clears a signal
     * which triggers the same job must not recurse. */
    if (job->evaluating) {
        return;
    }
    job->evaluating = true;

    rw_event(SEVERITY_INFO, "CONJOB", "Job %s fired (%s)", job->name,
             conjob_trigger_name(job->trigger));

    for (i = 0; i < job->action_count; ++i) {
        refusals += execute_action(job, &job->actions[i]);
    }

    job->evaluating = false;
    job->last_fired_ms = (unsigned)now;
    job->fire_count++;
    job->refuse_count += (unsigned)refusals;

    g_stats.total_fires++;
    g_stats.total_refusals += (unsigned)refusals;
    g_window_fires++;

    if (job->one_shot) {
        job->fired = true;
        job->enabled = false;
        rw_event(SEVERITY_INFO, "CONJOB",
                 "Job %s was one-shot and has now disabled itself", job->name);
    }
}

RwResult conjob_trigger_now(RwId job_id, bool force)
{
    ConJob *job = NULL;
    size_t i;

    for (i = 0; i < g_job_count; ++i) {
        if (g_jobs[i].id == job_id) {
            job = &g_jobs[i];
            break;
        }
    }
    if (job == NULL) {
        return RW_ERR_INVALID_ID;
    }
    if (!job->enabled && !force) {
        return RW_ERR_OUT_OF_SERVICE;
    }
    fire_job(job, true);
    return RW_OK;
}

/* --------------------------------------------------------------------------
 * Per-tick evaluation
 * -------------------------------------------------------------------------- */
void conjob_tick(unsigned delta_ms)
{
    unsigned long now = rw_engine()->clock_ms;
    size_t i;

    (void)delta_ms;

    if (!g_settings.enabled) {
        return;
    }

    /* Evaluate on a fixed cadence so a 10 ms GUI tick does not run 100x more
     * job evaluations than a 1000 ms one. */
    if (g_last_evaluation_ms != 0
        && now - g_last_evaluation_ms < g_settings.evaluation_interval_ms) {
        return;
    }
    g_last_evaluation_ms = now;
    g_stats.evaluations++;

    for (i = 0; i < g_job_count; ++i) {
        ConJob *job = &g_jobs[i];

        if (!job->enabled || job->fired || job->evaluating) {
            continue;
        }
        /* Cooldown between firings. */
        if (job->last_fired_ms != 0
            && now - job->last_fired_ms < job->cooldown_ms) {
            continue;
        }
        if (!trigger_fires(job)) {
            continue;
        }
        if (!condition_holds(job)) {
            continue;
        }
        fire_job(job, false);
    }
}

/* --------------------------------------------------------------------------
 * Description (GUI / terminal)
 * -------------------------------------------------------------------------- */
RwResult conjob_describe(RwId job_id, char *buffer, size_t size)
{
    ConJob job;

    if (buffer == NULL || size == 0) {
        return RW_ERR_INVALID_ARG;
    }
    if (conjob_get(job_id, &job) != RW_OK) {
        return RW_ERR_INVALID_ID;
    }

    snprintf(buffer, size,
             "%-14s [%-3s] prio %-2d fires %-4u refused %-4u WHEN %s%s%s",
             job.name, job.enabled ? "on" : "off", job.priority,
             job.fire_count, job.refuse_count,
             conjob_trigger_name(job.trigger),
             job.condition != CONJOB_COND_NONE ? " AND " : "",
             job.condition != CONJOB_COND_NONE ? conjob_condition_name(job.condition) : "");
    return RW_OK;
}

/* --------------------------------------------------------------------------
 * Persistence
 * -------------------------------------------------------------------------- */
RwResult conjob_save(const char *path)
{
    FILE *file;
    size_t i;

    if (path == NULL) {
        return RW_ERR_INVALID_ARG;
    }
    file = fopen(path, "w");
    if (file == NULL) {
        return RW_ERR_IO;
    }

    fprintf(file, "# %s CONJOB registry\n", RC_NAME);
    fprintf(file, "# One [job] section per automation rule.\n");
    fprintf(file, "settings_enabled = %s\n", g_settings.enabled ? "true" : "false");
    fprintf(file, "settings_dry_run = %s\n", g_settings.dry_run ? "true" : "false");
    fprintf(file, "settings_allow_emergency = %s\n",
            g_settings.allow_emergency_actions ? "true" : "false");
    fprintf(file, "settings_interval_ms = %u\n\n", g_settings.evaluation_interval_ms);

    for (i = 0; i < g_job_count; ++i) {
        const ConJob *job = &g_jobs[i];
        size_t a;

        fprintf(file, "[job]\n");
        fprintf(file, "name = %s\n", job->name);
        fprintf(file, "description = %s\n", job->description);
        fprintf(file, "enabled = %s\n", job->enabled ? "true" : "false");
        fprintf(file, "priority = %d\n", job->priority);
        fprintf(file, "trigger = %s\n", conjob_trigger_name(job->trigger));
        fprintf(file, "trigger_target = %u\n", job->trigger_target);
        fprintf(file, "trigger_arg = %d\n", job->trigger_arg);
        fprintf(file, "condition = %s\n", conjob_condition_name(job->condition));
        fprintf(file, "condition_target = %u\n", job->condition_target);
        fprintf(file, "condition_arg0 = %d\n", job->condition_arg0);
        fprintf(file, "one_shot = %s\n", job->one_shot ? "true" : "false");
        fprintf(file, "cooldown_ms = %u\n", job->cooldown_ms);

        for (a = 0; a < job->action_count; ++a) {
            fprintf(file, "action%u = %s:%u:%d:%.*s\n",
                    (unsigned)(a + 1),
                    conjob_action_name(job->actions[a].action),
                    job->actions[a].target,
                    job->actions[a].arg0,
                    80, job->actions[a].text);
        }
        fprintf(file, "\n");
    }

    fclose(file);
    rw_event(SEVERITY_INFO, "CONJOB", "Saved %u job(s) to %s", (unsigned)g_job_count, path);
    return RW_OK;
}

RwResult conjob_load(const char *path)
{
    FILE *file;
    char line[512];
    ConJob current;
    bool in_job = false;
    int loaded = 0;

    if (path == NULL) {
        return RW_ERR_INVALID_ARG;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        return RW_ERR_IO;
    }

    memset(&current, 0, sizeof(current));
    current.enabled = true;
    current.priority = 5;
    current.cooldown_ms = 3000;
    current.trigger = CONJOB_TRIGGER_TICK;
    current.condition = CONJOB_COND_NONE;

    while (fgets(line, (int)sizeof(line), file) != NULL) {
        char *cursor = line;
        char *equals;
        char *key;
        char *value;

        while (*cursor != '\0' && isspace((unsigned char)*cursor)) cursor++;
        if (*cursor == '\0' || *cursor == '#' || *cursor == ';') continue;

        /* A new [job] section flushes the previous one. */
        if (strncmp(cursor, "[job]", 5) == 0) {
            if (in_job && current.name[0] != '\0') {
                (void)conjob_add(&current);
                loaded++;
            }
            memset(&current, 0, sizeof(current));
            current.enabled = true;
            current.priority = 5;
            current.cooldown_ms = 3000;
            current.trigger = CONJOB_TRIGGER_TICK;
            current.condition = CONJOB_COND_NONE;
            in_job = true;
            continue;
        }

        equals = strchr(cursor, '=');
        if (equals == NULL) continue;
        *equals = '\0';
        key = cursor;
        value = equals + 1;
        {
            char *end = value + strlen(value) - 1;
            while (end > value && isspace((unsigned char)*end)) *end-- = '\0';
            while (*value != '\0' && isspace((unsigned char)*value)) value++;
        }

        if (strcmp(key, "settings_enabled") == 0) {
            g_settings.enabled = (strcmp(value, "true") == 0);
        } else if (strcmp(key, "settings_dry_run") == 0) {
            g_settings.dry_run = (strcmp(value, "true") == 0);
        } else if (strcmp(key, "settings_allow_emergency") == 0) {
            g_settings.allow_emergency_actions = (strcmp(value, "true") == 0);
        } else if (strcmp(key, "settings_interval_ms") == 0) {
            g_settings.evaluation_interval_ms = (unsigned)atoi(value);
        } else if (strcmp(key, "name") == 0) {
            snprintf(current.name, sizeof(current.name), "%s", value);
        } else if (strcmp(key, "description") == 0) {
            snprintf(current.description, sizeof(current.description), "%s", value);
        } else if (strcmp(key, "enabled") == 0) {
            current.enabled = (strcmp(value, "true") == 0);
        } else if (strcmp(key, "priority") == 0) {
            current.priority = atoi(value);
        } else if (strcmp(key, "trigger") == 0) {
            current.trigger = conjob_trigger_parse(value);
        } else if (strcmp(key, "trigger_target") == 0) {
            current.trigger_target = (RwId)atoi(value);
        } else if (strcmp(key, "trigger_arg") == 0) {
            current.trigger_arg = atoi(value);
        } else if (strcmp(key, "condition") == 0) {
            current.condition = conjob_condition_parse(value);
        } else if (strcmp(key, "condition_target") == 0) {
            current.condition_target = (RwId)atoi(value);
        } else if (strcmp(key, "condition_arg0") == 0) {
            current.condition_arg0 = atoi(value);
        } else if (strcmp(key, "one_shot") == 0) {
            current.one_shot = (strcmp(value, "true") == 0);
        } else if (strcmp(key, "cooldown_ms") == 0) {
            current.cooldown_ms = (unsigned)atoi(value);
        } else if (strncmp(key, "action", 6) == 0) {
            /* actionN = verb:target:arg0:text */
            char *first = strchr(value, ':');
            if (first != NULL && current.action_count < CONJOB_MAX_ACTIONS) {
                char verb[32];
                size_t verb_length = (size_t)(first - value);
                ConJobStep *step = &current.actions[current.action_count];

                if (verb_length < sizeof(verb)) {
                    memcpy(verb, value, verb_length);
                    verb[verb_length] = '\0';
                    step->action = conjob_action_parse(verb);
                    step->target = (RwId)atoi(first + 1);

                    {
                        char *second = strchr(first + 1, ':');
                        if (second != NULL) {
                            step->arg0 = atoi(second + 1);
                            {
                                char *third = strchr(second + 1, ':');
                                if (third != NULL) {
                                    snprintf(step->text, sizeof(step->text), "%s", third + 1);
                                }
                            }
                        }
                    }
                    current.action_count++;
                }
            }
        }
    }

    if (in_job && current.name[0] != '\0') {
        (void)conjob_add(&current);
        loaded++;
    }

    fclose(file);
    rw_event(SEVERITY_INFO, "CONJOB", "Loaded %d job(s) from %s", loaded, path);
    return RW_OK;
}

/* --------------------------------------------------------------------------
 * The default job set: the automation a real signal box would be configured
 * with on day one. Every one of these is a rule an operator would recognise.
 * -------------------------------------------------------------------------- */
void conjob_install_defaults(void);

void conjob_install_defaults(void)
{
    ConJob job;

    /* 1. Automatically set the up main route into platform 1 when a train is
     *    waiting and the path is free. */
    memset(&job, 0, sizeof(job));
    snprintf(job.name, sizeof(job.name), "AUTO-R1");
    snprintf(job.description, sizeof(job.description),
             "Set route R1 when a train is waiting and platform 1 is clear");
    job.enabled = true;
    job.priority = 6;
    job.trigger = CONJOB_TRIGGER_TRAIN_WAITING;
    job.trigger_target = 1;                     /* route R1 */
    job.condition = CONJOB_COND_NO_EMERGENCY;
    job.cooldown_ms = 5000;
    job.actions[0].action = CONJOB_ACTION_REQUEST_ROUTE;
    job.actions[0].target = 1;
    job.action_count = 1;
    (void)conjob_add(&job);

    /* 2. Return points to normal once a route has been released, so the
     *    layout is left in the through position ready for the next train. */
    memset(&job, 0, sizeof(job));
    snprintf(job.name, sizeof(job.name), "NORMALISE-P1");
    snprintf(job.description, sizeof(job.description),
             "Return P1 to NORMAL when no route holds it");
    job.enabled = true;
    job.priority = 2;
    job.trigger = CONJOB_TRIGGER_TICK;
    job.condition = CONJOB_COND_ROUTE_NOT_SET;
    job.condition_target = 2;
    job.cooldown_ms = 8000;
    job.actions[0].action = CONJOB_ACTION_SET_SWITCH;
    job.actions[0].target = 1;                  /* P1 */
    job.actions[0].arg0 = (int)SWITCH_MAIN;
    job.actions[0].text[0] = '\0';
    job.action_count = 1;
    (void)conjob_add(&job);

    /* 3. Announce a train arriving on the down platform. */
    memset(&job, 0, sizeof(job));
    snprintf(job.name, sizeof(job.name), "ARRIVAL-DN");
    snprintf(job.description, sizeof(job.description),
             "Log when the down platform becomes occupied");
    job.enabled = true;
    job.priority = 3;
    job.trigger = CONJOB_TRIGGER_TRACK_OCCUPIED;
    job.trigger_target = 9;                     /* T9 down platform */
    job.cooldown_ms = 6000;
    job.actions[0].action = CONJOB_ACTION_LOG_MESSAGE;
    snprintf(job.actions[0].text, sizeof(job.actions[0].text),
             "Down platform now occupied - train arrived");
    job.action_count = 1;
    (void)conjob_add(&job);

    /* 4. Warn if a section stays occupied for an unusual time. */
    memset(&job, 0, sizeof(job));
    snprintf(job.name, sizeof(job.name), "STALL-WATCH");
    snprintf(job.description, sizeof(job.description),
             "Warn if two or more alarms are outstanding");
    job.enabled = true;
    job.priority = 4;
    job.trigger = CONJOB_TRIGGER_TICK;
    job.condition = CONJOB_COND_ALARM_COUNT_ABOVE;
    job.condition_arg0 = 1;
    job.cooldown_ms = 30000;
    job.actions[0].action = CONJOB_ACTION_LOG_MESSAGE;
    snprintf(job.actions[0].text, sizeof(job.actions[0].text),
             "Multiple alarms outstanding - signaller attention required");
    job.action_count = 1;
    (void)conjob_add(&job);

    /* 5. Release the emergency automatically once every train has stopped and
     *    a minute has passed - the "area confirmed safe" case. */
    memset(&job, 0, sizeof(job));
    snprintf(job.name, sizeof(job.name), "AUTO-CLEAR-EMG");
    snprintf(job.description, sizeof(job.description),
             "Clear the emergency stop once all trains are at a stand");
    job.enabled = false;            /* off by default: clearing is a human call */
    job.priority = 1;
    job.trigger = CONJOB_TRIGGER_EMERGENCY_ACTIVE;
    job.cooldown_ms = 60000;
    job.actions[0].action = CONJOB_ACTION_CLEAR_EMERGENCY;
    job.action_count = 1;
    (void)conjob_add(&job);

    rw_event(SEVERITY_INFO, "CONJOB", "Installed %d default control job(s)",
             (int)g_job_count);
}
