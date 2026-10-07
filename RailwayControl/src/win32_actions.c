/* ==========================================================================
 * win32_actions.c - 🎛️ Menu and command handling.
 *
 * Every railway-changing action the GUI can perform is here, and every one of
 * them goes through railway_api.h. That is not a style preference: the GUI
 * must not be a privileged back door into the engine. When the interlocking
 * refuses - a route over an occupied section, a point under a train - the GUI
 * reports the refusal in the status bar and changes nothing.
 *
 * Handling is split by intent:
 *
 *   local commands    zoom, grid, labels, panes   - touch only the window
 *   engine commands   emergency, acknowledge      - call the public API
 *   informational     help, credits, permissions  - write into the terminal
 * ========================================================================== */
#include "win32.h"

#include "railway_api.h"
#include "railcontrol.h"
#include "rc_version.h"
#include "rc_account.h"
#include "conjob.h"
#include "terminal.h"

#include <stdio.h>
#include <string.h>

/* The status line text, shared with win32_status.c. */
static wchar_t g_last_status[256];

const wchar_t *rcwin_last_status(void)
{
    return g_last_status;
}

/* --------------------------------------------------------------------------
 * Putting a message on the status line and in the terminal pane
 * -------------------------------------------------------------------------- */
static void report(const char *text)
{
    rcwin_set_status_text(text);
    rcwin_terminal_append(text);
}

/**
 * Report the outcome of an engine call.
 *
 * This is the single place a command result becomes operator feedback. It
 * distinguishes success from refusal explicitly, because "the interlocking
 * said no" and "it worked" must never look the same on a signalling display.
 */
static void report_result(const char *what, RwResult result)
{
    char line[RW_MAX_TEXT + 64];

    if (result == RW_OK)
    {
        snprintf(line, sizeof(line), "OK: %s", what);
    }
    else
    {
        snprintf(line, sizeof(line), "REFUSED: %s - %s",
                 what, rw_result_string(result));
    }
    report(line);
}

/* --------------------------------------------------------------------------
 * Local (window-only) commands
 * -------------------------------------------------------------------------- */
static void command_zoom(int delta)
{
    RcWinApp *app = rcwin_app();
    char line[64];

    app->zoom_percent += delta;
    if (app->zoom_percent < 50)
        app->zoom_percent = 50;
    if (app->zoom_percent > 300)
        app->zoom_percent = 300;

    snprintf(line, sizeof(line), "Zoom %d%%", app->zoom_percent);
    rcwin_set_status_text(line);
}

static void command_toggle_grid(void)
{
    RcWinApp *app = rcwin_app();
    app->show_grid = !app->show_grid;
    rcwin_set_status_text(app->show_grid ? "Grid on" : "Grid off");
}

static void command_toggle_labels(void)
{
    RcWinApp *app = rcwin_app();
    app->show_labels = !app->show_labels;
    rcwin_set_status_text(app->show_labels ? "Train labels on" : "Train labels off");
}

static void command_toggle_sensors(void)
{
    RcWinApp *app = rcwin_app();
    app->show_sensors = !app->show_sensors;
    rcwin_set_status_text(app->show_sensors ? "Sensor ids on" : "Sensor ids off");
}

/* --------------------------------------------------------------------------
 * Engine commands
 * -------------------------------------------------------------------------- */
static void command_emergency_stop(void)
{
    if (!railway_is_initialized()) {
        report("REFUSED: no engine");
        return;
    }
    /* The emergency stop is the one action that must never be gated behind a
     * confirmation dialogue: an operator reaching for it is already certain. */
    report_result("emergency stop applied - all signals at danger, all trains braked",
                  railway_emergency_stop_reason("Dispatch console emergency stop"));
}

static void command_clear_emergency(void)
{
    report_result("emergency cleared, normal working resumed",
                  railway_clear_emergency());
}

static void command_acknowledge_all(void)
{
    const int count = railway_acknowledge_all();
    char line[96];

    snprintf(line, sizeof(line), "Acknowledged %d alarm(s)", count);
    report(line);
}

static void command_hold_all_trains(bool hold)
{
    int affected = 0;
    int i;
    char line[128];

    for (i = 0; i < railway_train_count(); ++i) {
        RwTrainInfo train;

        if (railway_get_train_at(i, &train) != RW_OK) {
            continue;
        }
        if (railway_hold_train((int)train.id, hold) == RW_OK) {
            affected++;
        }
    }

    snprintf(line, sizeof(line), "%s %d train(s)",
             hold ? "Held" : "Released", affected);
    report(line);
}

static void command_set_conjobs(bool enabled)
{
    RcSettings *settings = rc_settings();
    char line[96];

    settings->conjobs_enabled = enabled;
    snprintf(line, sizeof(line), "CONJOB automation %s",
             enabled ? "ACTIVATED" : "SUSPENDED");
    report(line);
}

static void command_early_release(void)
{
    /* Early release cancels every route the interlocking will let go. A route
     * a train is still standing on is refused, which is the correct and safe
     * outcome - the operator sees which ones were refused. */
    int released = 0;
    int refused = 0;
    int i;
    char line[128];

    for (i = 0; i < railway_route_count(); ++i)
    {
        RwRouteInfo info;

        if (railway_get_route_at(i, &info) != RW_OK)
        {
            continue;
        }
        if (info.state != ROUTE_LOCKED && info.state != ROUTE_OCCUPIED)
        {
            continue;
        }
        if (railway_cancel_route((int)info.id) == RW_OK)
        {
            released++;
        }
        else
        {
            refused++;
        }
    }

    snprintf(line, sizeof(line), "Early release: %d route(s) cancelled, %d refused "
                                 "(still occupied)",
             released, refused);
    report(line);
}

/* --------------------------------------------------------------------------
 * Informational commands - written into the terminal pane
 * -------------------------------------------------------------------------- */
static void command_show_permissions(void)
{
    int role;
    int permission;

    rcwin_terminal_append("");
    rcwin_terminal_append("ROLE / PERMISSION MATRIX");
    for (permission = 0; permission < PERM_COUNT; ++permission)
    {
        char line[160];
        int used = snprintf(line, sizeof(line), "%-13s",
                            rc_permission_name((RcPermission)permission));
        for (role = ROLE_VIEWER; role <= ROLE_ADMIN; ++role)
        {
            used += snprintf(line + used, sizeof(line) - (size_t)used, " %-11s",
                             rc_role_has((RcRole)role, (RcPermission)permission)
                                 ? "yes"
                                 : "-");
        }
        rcwin_terminal_append(line);
    }
}

static void command_show_jobs(void)
{
    ConJobStats stats;
    int i;

    rcwin_terminal_append("");
    rcwin_terminal_append("AUTOMATION JOBS (CONJOB)");

    if (conjob_get_stats(&stats) == RW_OK)
    {
        char line[160];
        snprintf(line, sizeof(line),
                 "  %d job(s), %d enabled, %u fires, %u refusals",
                 stats.jobs_total, stats.jobs_enabled,
                 stats.total_fires, stats.total_refusals);
        rcwin_terminal_append(line);
    }

    for (i = 0; i < conjob_count(); ++i)
    {
        ConJob job;
        char line[256];

        if (conjob_get_at(i, &job) != RW_OK)
        {
            continue;
        }
        snprintf(line, sizeof(line), "  %-14s [%s] prio %d fires %u",
                 job.name, job.enabled ? "on " : "off", job.priority, job.fire_count);
        rcwin_terminal_append(line);
    }
}

static void command_show_settings(void)
{
    RcSettings *settings = rc_settings();
    char line[256];

    rcwin_terminal_append("");
    rcwin_terminal_append("RUNTIME SETTINGS");
    snprintf(line, sizeof(line), "  tick %d ms   time scale x%d   zoom %d%%",
             settings->tick_interval_ms, settings->time_scale,
             settings->diagram_zoom_percent);
    rcwin_terminal_append(line);
    snprintf(line, sizeof(line), "  auto signals %s   auto release %s",
             settings->auto_signal_routes ? "on" : "off",
             settings->auto_release_routes ? "on" : "off");
    rcwin_terminal_append(line);
    snprintf(line, sizeof(line), "  fail-safe unknown %s   CONJOBs %s",
             settings->fail_safe_unknown ? "ON (locked)" : "OFF",
             settings->conjobs_enabled ? "enabled" : "disabled");
    rcwin_terminal_append(line);
}

static void command_show_help(void)
{
    rcwin_terminal_append("");
    rcwin_terminal_append("DISPATCH CONSOLE HELP");
    rcwin_terminal_append("  Type commands in the terminal pane below.");
    rcwin_terminal_append("  SIGNAL <id> GREEN|DANGER     work a signal");
    rcwin_terminal_append("  POINT <id> NORMAL|REVERSE    work a point");
    rcwin_terminal_append("  ROUTE <id>   / CANCEL <id>   set or cancel a route");
    rcwin_terminal_append("  DISPATCH <id>                set a route and release a train");
    rcwin_terminal_append("  TRAIN <id> RUN|STOP|SPEED <n>");
    rcwin_terminal_append("  LIST SIGNALS|POINTS|ROUTES|TRACKS|TRAINS|SENSORS|EVENTS|JOBS");
    rcwin_terminal_append("  SHOW                         overall status");
    rcwin_terminal_append("  EMERGENCY [reason] / CLEAREMG");
    rcwin_terminal_append("  ACK ALL                      acknowledge alarms");
    rcwin_terminal_append("  HELP <command>               detail on one command");
}

static void command_show_shortcuts(void)
{
    rcwin_terminal_append("");
    rcwin_terminal_append("KEYBOARD SHORTCUTS");
    rcwin_terminal_append("  F1   help            F2   shortcuts");
    rcwin_terminal_append("  F5   pause engine    F12  emergency stop");
    rcwin_terminal_append("  ESC  clear input line   Enter  run command");
    rcwin_terminal_append("  Ctrl+S / Ctrl+O      save / load state");
}

static void command_show_credits(void)
{
    rcwin_terminal_append("");
    rcwin_terminal_append(rc_credits_text());
}

static void command_show_safety(void)
{
    rcwin_terminal_append("");
    rcwin_terminal_append(rc_safety_notice());
}

static void command_show_accounts(void)
{
    int i;

    rcwin_terminal_append("");
    rcwin_terminal_append("ACCOUNTS");

    for (i = 0; i < rc_account_count(); ++i)
    {
        const RcAccount *account = rc_account_get_at(i);
        char line[192];

        if (account == NULL)
        {
            continue;
        }
        snprintf(line, sizeof(line), "  %-12s %-11s %s",
                 account->username, rc_role_name(account->role),
                 rc_account_status_name(account->status));
        rcwin_terminal_append(line);
    }
}

/* --------------------------------------------------------------------------
 * Save / load state
 *
 * state_io.h is named here rather than railway_api.h because snapshotting the
 * whole engine is a persistence concern, not a control one. When the
 * persistence module is not linked the command reports that plainly instead of
 * pretending to have saved something.
 * -------------------------------------------------------------------------- */
static void command_save_state(void)
{
    report("Save state: use the terminal  SAVE <path>  command");
}

static void command_load_state(void)
{
    report("Load state: use the terminal  LOAD <path>  command");
}

/* --------------------------------------------------------------------------
 * Dispatch
 * -------------------------------------------------------------------------- */
bool rcwin_handle_command(int command_id)
{
    switch (command_id)
    {
    /* File */
    case RCWIN_CMD_SAVE_STATE:
        command_save_state();
        break;
    case RCWIN_CMD_LOAD_STATE:
        command_load_state();
        break;
    case RCWIN_CMD_SAVE_SETTINGS:
        report("Save settings: use SAVE settings <path>");
        break;
    case RCWIN_CMD_LOAD_SETTINGS:
        report("Load settings: use LOAD settings <path>");
        break;
    case RCWIN_CMD_EXIT:
        PostMessageW(rcwin_app()->window, WM_CLOSE, 0, 0);
        return true;

    /* View */
    case RCWIN_CMD_ZOOM_IN:
        command_zoom(10);
        break;
    case RCWIN_CMD_ZOOM_OUT:
        command_zoom(-10);
        break;
    case RCWIN_CMD_ZOOM_RESET:
        rcwin_reset_view();
        break;
    case RCWIN_CMD_TOGGLE_GRID:
        command_toggle_grid();
        break;
    case RCWIN_CMD_TOGGLE_LABELS:
        command_toggle_labels();
        break;
    case RCWIN_CMD_TOGGLE_SENSORS:
        command_toggle_sensors();
        break;
    case RCWIN_CMD_SHOW_EVENTS:
        report("Event log pane is shown below the diagram");
        break;
    case RCWIN_CMD_SHOW_TERMINAL:
        report("Terminal pane is shown bottom-right");
        break;

    /* Control */
    case RCWIN_CMD_EMERGENCY_STOP:
        command_emergency_stop();
        break;
    case RCWIN_CMD_CLEAR_EMERGENCY:
        command_clear_emergency();
        break;
    case RCWIN_CMD_ACK_ALL:
        command_acknowledge_all();
        break;
    case RCWIN_CMD_EARLY_RELEASE:
        command_early_release();
        break;
    case RCWIN_CMD_HOLD_ALL_TRAINS:
        command_hold_all_trains(true);
        break;
    case RCWIN_CMD_RELEASE_ALL_TRAINS:
        command_hold_all_trains(false);
        break;
    case RCWIN_CMD_ACTIVATE_CONJOBS:
        command_set_conjobs(true);
        break;
    case RCWIN_CMD_SUSPEND_CONJOBS:
        command_set_conjobs(false);
        break;

    /* Tools */
    case RCWIN_CMD_PERMISSIONS:
        command_show_permissions();
        break;
    case RCWIN_CMD_ACCOUNTS:
        command_show_accounts();
        break;
    case RCWIN_CMD_JOBS:
        command_show_jobs();
        break;
    case RCWIN_CMD_SETTINGS:
        command_show_settings();
        break;
    case RCWIN_CMD_SCRIPT_CONSOLE:
        report("Scripting: use the terminal  SCRIPT <file>");
        break;
    case RCWIN_CMD_AUDIO_TEST:
        report("Audio: use the terminal  SET audio.muted false");
        break;

    /* Help */
    case RCWIN_CMD_HELP:
        command_show_help();
        break;
    case RCWIN_CMD_SHORTCUTS:
        command_show_shortcuts();
        break;
    case RCWIN_CMD_CREDITS:
        command_show_credits();
        break;
    case RCWIN_CMD_SAFETY_NOTICE:
        command_show_safety();
        break;
    case RCWIN_CMD_ABOUT:
        command_show_credits();
        break;

    default:
        return false;
    }

    rcwin_request_repaint();
    return true;
}

void rcwin_reset_view(void)
{
    RcWinApp *app = rcwin_app();

    app->zoom_percent = 100;
    app->show_grid = true;
    app->show_labels = true;
    app->show_sensors = false;
    rcwin_set_status_text("View reset");
}
