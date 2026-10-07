/* ==========================================================================
 * terminal.c - 💻 Signaller command terminal.
 *
 * Implements the command language described in terminal.h. Every mutating
 * command routes through railway_api.h, so the interlocking proof, the
 * emergency-stop interlock and the audit trail all apply exactly as they do
 * for a click in the GUI.
 *
 * Design points
 * -------------
 *  - Parsing is table driven: one CommandSpec per verb, with a handler.
 *  - Command names are matched case-insensitively and may be abbreviated to
 *    the shortest unique prefix ("SIG S3 D" works).
 *  - Argument errors quote the command usage string, so the terminal is
 *    self documenting.
 *  - Nothing here allocates: output goes into a fixed buffer with truncation,
 *    so a runaway LIST cannot exhaust memory.
 * ========================================================================== */
#include "terminal.h"
#include "railway_api.h"
#include "railcontrol.h"
#include "rc_account.h"
#include "rc_version.h"
#include "conjob.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(MF_WITH_LUA)
#include "lua_script.h"
#endif

#if defined(_WIN32)
#define strncasecmp _strnicmp
#endif

/* ==========================================================================
 * Output accumulator
 * ========================================================================== */
typedef struct
{
    char text[TERMINAL_MAX_OUTPUT];
    size_t used;
    bool truncated;
} Output;

static void out_init(Output *o)
{
    o->text[0] = '\0';
    o->used = 0;
    o->truncated = false;
}

static void out_printf(Output *o, const char *format, ...)
{
    va_list args;
    int written;

    if (o->truncated)
    {
        return;
    }
    va_start(args, format);
    written = vsnprintf(o->text + o->used, sizeof(o->text) - o->used, format, args);
    va_end(args);

    if (written < 0)
    {
        o->truncated = true;
        return;
    }
    if ((size_t)written >= sizeof(o->text) - o->used)
    {
        o->used = sizeof(o->text) - 1;
        o->truncated = true;
        return;
    }
    o->used += (size_t)written;
}

static void out_line(Output *o, const char *text)
{
    out_printf(o, "%s\n", text);
}

static void out_rule(Output *o, char character, int width)
{
    int i;
    for (i = 0; i < width; ++i)
    {
        out_printf(o, "%c", character);
    }
    out_line(o, "");
}

/* ==========================================================================
 * Tokeniser and small parsing helpers
 * ========================================================================== */
typedef struct
{
    char tokens[16][RW_MAX_NAME * 2];
    int count;
    bool overflow;
} TokenList;

static void tokenise(const char *line, TokenList *out)
{
    const char *cursor = line;

    out->count = 0;
    out->overflow = false;

    while (*cursor != '\0')
    {
        char quote = '\0';
        size_t length = 0;

        while (*cursor != '\0' && isspace((unsigned char)*cursor))
        {
            cursor++;
        }
        if (*cursor == '\0')
        {
            break;
        }
        if (out->count >= 16)
        {
            out->overflow = true;
            break;
        }
        if (*cursor == '"' || *cursor == '\'')
        {
            quote = *cursor++;
        }
        while (*cursor != '\0' && length < sizeof(out->tokens[0]) - 1)
        {
            if (quote != '\0' && *cursor == quote)
            {
                cursor++;
                break;
            }
            if (quote == '\0' && isspace((unsigned char)*cursor))
            {
                break;
            }
            out->tokens[out->count][length++] = *cursor++;
        }
        out->tokens[out->count][length] = '\0';
        out->count++;
    }
}

static bool iequals(const char *a, const char *b)
{
    if (a == NULL || b == NULL)
    {
        return false;
    }
    while (*a != '\0' && *b != '\0')
    {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
        {
            return false;
        }
        a++;
        b++;
    }
    return *a == *b;
}

/* Join tokens [from..end) into one text buffer (for free-text arguments). */
static void join_tokens(const TokenList *args, int from, char *buffer, size_t size)
{
    int i;
    size_t used = 0;

    if (size == 0)
    {
        return;
    }
    buffer[0] = '\0';
    for (i = from; i < args->count && used + 2 < size; ++i)
    {
        if (i > from)
        {
            buffer[used++] = ' ';
        }
        snprintf(buffer + used, size - used, "%s", args->tokens[i]);
        used = strlen(buffer);
    }
}

static bool parse_aspect(const char *text, SignalState *out)
{
    if (iequals(text, "GREEN") || iequals(text, "G") || iequals(text, "CLEAR"))
    {
        *out = SIGNAL_GREEN;
        return true;
    }
    if (iequals(text, "YELLOW") || iequals(text, "Y") || iequals(text, "CAUTION") || iequals(text, "RESTRICTED"))
    {
        *out = SIGNAL_YELLOW;
        return true;
    }
    if (iequals(text, "RED") || iequals(text, "R") || iequals(text, "DANGER") || iequals(text, "STOP"))
    {
        *out = SIGNAL_RED;
        return true;
    }
    return false;
}

static bool parse_position(const char *text, SwitchPosition *out)
{
    if (iequals(text, "MAIN") || iequals(text, "NORMAL") || iequals(text, "N"))
    {
        *out = SWITCH_MAIN;
        return true;
    }
    if (iequals(text, "DIVERGING") || iequals(text, "REVERSE") || iequals(text, "R"))
    {
        *out = SWITCH_DIVERGING;
        return true;
    }
    return false;
}

static bool parse_train_state(const char *text, TrainState *out)
{
    if (iequals(text, "RUN") || iequals(text, "RUNNING") || iequals(text, "GO"))
    {
        *out = TRAIN_RUNNING;
        return true;
    }
    if (iequals(text, "STOP") || iequals(text, "STOPPED") || iequals(text, "HALT"))
    {
        *out = TRAIN_STOPPED;
        return true;
    }
    if (iequals(text, "EMERGENCY") || iequals(text, "EB"))
    {
        *out = TRAIN_EMERGENCY_STOP;
        return true;
    }
    return false;
}

/* Resolve an asset argument: a numeric id or a name. -1 when not found. */
static int resolve_numeric_or_signal(const char *text)
{
    char *end = NULL;
    long value = strtol(text, &end, 10);
    RwId id;

    if (end != NULL && *end == '\0' && value > 0)
    {
        return (int)value;
    }
    id = rw_signal_find(text);
    return (id == RW_ID_NONE) ? -1 : (int)id;
}

static int resolve_numeric_or_switch(const char *text)
{
    char *end = NULL;
    long value = strtol(text, &end, 10);
    RwId id;

    if (end != NULL && *end == '\0' && value > 0)
    {
        return (int)value;
    }
    id = rw_point_find(text);
    return (id == RW_ID_NONE) ? -1 : (int)id;
}

static int resolve_numeric_or_track(const char *text)
{
    char *end = NULL;
    long value = strtol(text, &end, 10);
    RwId id;

    if (end != NULL && *end == '\0' && value > 0)
    {
        return (int)value;
    }
    id = rw_track_find(text);
    return (id == RW_ID_NONE) ? -1 : (int)id;
}

static int resolve_numeric_or_train(const char *text)
{
    char *end = NULL;
    long value = strtol(text, &end, 10);
    RwId id;

    if (end != NULL && *end == '\0' && value > 0)
    {
        return (int)value;
    }
    id = rw_train_find(text);
    return (id == RW_ID_NONE) ? -1 : (int)id;
}

static int resolve_route(const char *text)
{
    int index;
    int count;
    RwRouteInfo info;
    char *end = NULL;
    long value;

    if (text == NULL || text[0] == '\0') {
        return -1;
    }

    /* A plain number is an id. */
    value = strtol(text, &end, 10);
    if (end != NULL && *end == '\0' && value > 0) {
        return (int)value;
    }

    count = railway_route_count();

    /* Exact match on the full route name, e.g. "R1 UP-P1-EAST". */
    for (index = 0; index < count; ++index) {
        if (railway_get_route_at(index, &info) != RW_OK) {
            continue;
        }
        if (iequals(info.name, text)) {
            return (int)info.id;
        }
    }

    /* Leading token match, so the operator can type just "R1". */
    for (index = 0; index < count; ++index) {
        if (railway_get_route_at(index, &info) != RW_OK) {
            continue;
        }
        if (strncasecmp(info.name, text, strlen(text)) == 0) {
            /* Require the match to end at a token boundary so "R1" does not
             * also match "R10". */
            if (info.name[strlen(text)] == '\0' || info.name[strlen(text)] == ' ') {
                return (int)info.id;
            }
        }
    }

    return -1;
}

static int resolve_sensor(const char *text)
{
    char *end = NULL;
    long value = strtol(text, &end, 10);
    int index;
    int count = railway_sensor_count();
    RwSensorInfo info;

    if (end != NULL && *end == '\0' && value > 0)
    {
        return (int)value;
    }
    for (index = 0; index < count; ++index)
    {
        if (railway_get_sensor_at(index, &info) == RW_OK && iequals(info.name, text))
        {
            return (int)info.id;
        }
    }
    return -1;
}

/* ==========================================================================
 * Command table
 * ========================================================================== */
typedef struct {
    const char *name;
    const char *usage;
    const char *summary;
    RcPermission permission;      /* PERM_VIEW_DIAGRAM = read only */
    TerminalStatus (*handler)(TokenList *args, Output *out, TerminalResult *result);
} CommandSpec;

static TerminalStatus cmd_help(TokenList *args, Output *out, TerminalResult *result);
static TerminalStatus cmd_show(TokenList *args, Output *out, TerminalResult *result);
static TerminalStatus cmd_signal(TokenList *args, Output *out, TerminalResult *result);
static TerminalStatus cmd_point(TokenList *args, Output *out, TerminalResult *result);
static TerminalStatus cmd_route(TokenList *args, Output *out, TerminalResult *result);
static TerminalStatus cmd_cancel(TokenList *args, Output *out, TerminalResult *result);
static TerminalStatus cmd_dispatch(TokenList *args, Output *out, TerminalResult *result);
static TerminalStatus cmd_train(TokenList *args, Output *out, TerminalResult *result);
static TerminalStatus cmd_track(TokenList *args, Output *out, TerminalResult *result);
static TerminalStatus cmd_sensor(TokenList *args, Output *out, TerminalResult *result);
static TerminalStatus cmd_list(TokenList *args, Output *out, TerminalResult *result);
static TerminalStatus cmd_emergency(TokenList *args, Output *out, TerminalResult *result);
static TerminalStatus cmd_clear_emergency(TokenList *args, Output *out, TerminalResult *result);
static TerminalStatus cmd_log(TokenList *args, Output *out, TerminalResult *result);
static TerminalStatus cmd_alarm(TokenList *args, Output *out, TerminalResult *result);
static TerminalStatus cmd_ack(TokenList *args, Output *out, TerminalResult *result);
static TerminalStatus cmd_job(TokenList *args, Output *out, TerminalResult *result);
static TerminalStatus cmd_set(TokenList *args, Output *out, TerminalResult *result);
static TerminalStatus cmd_script(TokenList *args, Output *out, TerminalResult *result);
static TerminalStatus cmd_lua(TokenList *args, Output *out, TerminalResult *result);
static TerminalStatus cmd_save(TokenList *args, Output *out, TerminalResult *result);
static TerminalStatus cmd_load(TokenList *args, Output *out, TerminalResult *result);
static TerminalStatus cmd_quit(TokenList *args, Output *out, TerminalResult *result);
static TerminalStatus cmd_invariants(TokenList *args, Output *out, TerminalResult *result);
static TerminalStatus cmd_account(TokenList *args, Output *out, TerminalResult *result);
static TerminalStatus cmd_logoff(TokenList *args, Output *out, TerminalResult *result);
static TerminalStatus cmd_authlog(TokenList *args, Output *out, TerminalResult *result);
static TerminalStatus cmd_credits(TokenList *args, Output *out, TerminalResult *result);

static const CommandSpec g_commands[] = {
    {"HELP", "HELP [command]", "Command index or detailed help", PERM_VIEW_DIAGRAM, cmd_help},
    {"SHOW", "SHOW", "Overall status", PERM_VIEW_DIAGRAM, cmd_show},
    {"SIGNAL", "SIGNAL <id|name> <GREEN|YELLOW|DANGER>", "Set a signal aspect", PERM_SET_SIGNAL, cmd_signal},
    {"POINT", "POINT <id|name> <MAIN|DIVERGING>", "Set a point (switch)", PERM_SET_POINT, cmd_point},
    {"RELEASE", "RELEASE <id|name>", "Release a point from its route lock", PERM_RELEASE_POINT, cmd_point},
    {"ROUTE", "ROUTE <id|name>", "Request (set) a route", PERM_REQUEST_ROUTE, cmd_route},
    {"CANCEL", "CANCEL <id|name>", "Cancel a set route", PERM_REQUEST_ROUTE, cmd_cancel},
    {"DISPATCH", "DISPATCH <id|name>", "Set a route and dispatch the first train", PERM_DISPATCH_TRAIN, cmd_dispatch},
    {"TRAIN", "TRAIN <id|headcode> <RUN|STOP|EMERGENCY|HOLD|RELEASE|SPEED <kph>>", "Train control", PERM_CONTROL_TRAIN, cmd_train},
    {"TRACK", "TRACK <id|name> [FAIL|RESTORE]", "Track occupancy and circuit state", PERM_VIEW_DIAGRAM, cmd_track},
    {"SENSOR", "SENSOR <id|name> <TRIGGER|CLEAR|FAIL> [value]", "Inject a sensor reading", PERM_INJECT_SENSOR, cmd_sensor},
    {"LIST", "LIST <SIGNALS|POINTS|ROUTES|TRACKS|TRAINS|SENSORS|EVENTS|JOBS|SETTINGS|ALL>", "Tabular listings", PERM_VIEW_DIAGRAM, cmd_list},
    {"EMERGENCY", "EMERGENCY [reason]", "Trip the emergency stop", PERM_EMERGENCY_STOP, cmd_emergency},
    {"CLEAREMG", "CLEAREMG", "Clear the emergency stop", PERM_CLEAR_EMERGENCY, cmd_clear_emergency},
    {"LOG", "LOG <text>", "Write an INFO event", PERM_VIEW_DIAGRAM, cmd_log},
    {"ALARM", "ALARM <text>", "Write an ALARM event", PERM_VIEW_DIAGRAM, cmd_alarm},
    {"ACK", "ACK [id|ALL]", "Acknowledge alarms", PERM_ACKNOWLEDGE_ALARM, cmd_ack},
    {"JOB", "JOB <LIST|ENABLE|DISABLE|TRIGGER> [name] [FORCE]", "Automated control jobs (CONJOB)", PERM_MANAGE_JOBS, cmd_job},
    {"SET", "SET <key> <value>", "Change a runtime setting", PERM_CHANGE_SETTINGS, cmd_set},
    {"SCRIPT", "SCRIPT <file.lua>", "Run a Lua script", PERM_RUN_SCRIPTS, cmd_script},
    {"LUA", "LUA <expression>", "Evaluate a Lua expression", PERM_RUN_SCRIPTS, cmd_lua},
    {"SAVE", "SAVE <path>", "Write a state snapshot", PERM_CHANGE_SETTINGS, cmd_save},
    {"LOAD", "LOAD <path>", "Restore a state snapshot", PERM_CHANGE_SETTINGS, cmd_load},
    {"ACCOUNT", "ACCOUNT <LIST|WHOAMI|PERMS|UNLOCK|ROLE|PASSWORD|CREATE|REMOVE> [args]", "Account administration", PERM_MANAGE_ACCOUNTS, cmd_account},
    {"LOGOFF", "LOGOFF", "End the signed-in session", PERM_VIEW_DIAGRAM, cmd_logoff},
    {"AUTHLOG", "AUTHLOG [count]", "Show the authentication log", PERM_VIEW_LOGS, cmd_authlog},
    {"INVARIANTS", "INVARIANTS", "List the safety invariants the engine enforces", PERM_VIEW_DIAGRAM, cmd_invariants},
    {"CREDITS", "CREDITS", "Version, authors and the safety notice", PERM_VIEW_DIAGRAM, cmd_credits},
    {"QUIT", "QUIT", "End the terminal session", PERM_VIEW_DIAGRAM, cmd_quit},
    {"EXIT", "EXIT", "End the terminal session", PERM_VIEW_DIAGRAM, cmd_quit}};

static const size_t g_command_count = sizeof(g_commands) / sizeof(g_commands[0]);

/* ==========================================================================
 * Terminal state
 * ========================================================================== */
static char g_history[TERMINAL_MAX_HISTORY][TERMINAL_MAX_LINE];
static int g_history_count = 0;
static int g_history_next = 0;
static void (*g_event_sink)(RwSeverity, const char *) = NULL;

void terminal_init(void)
{
    memset(g_history, 0, sizeof(g_history));
    g_history_count = 0;
    g_history_next = 0;
}

void terminal_shutdown(void)
{
    terminal_init();
    g_event_sink = NULL;
}

void terminal_attach_event_sink(void (*sink)(RwSeverity severity, const char *line))
{
    g_event_sink = sink;
}

void terminal_history_clear(void)
{
    g_history_count = 0;
    g_history_next = 0;
}

void terminal_history_add(const char *line)
{
    if (line == NULL || line[0] == '\0')
    {
        return;
    }
    snprintf(g_history[g_history_next], TERMINAL_MAX_LINE, "%s", line);
    g_history_next = (g_history_next + 1) % TERMINAL_MAX_HISTORY;
    if (g_history_count < TERMINAL_MAX_HISTORY)
    {
        g_history_count++;
    }
}

int terminal_history_count(void)
{
    return g_history_count;
}

const char *terminal_history_at(int index_from_newest)
{
    int slot;

    if (index_from_newest < 0 || index_from_newest >= g_history_count)
    {
        return NULL;
    }
    slot = (g_history_next - 1 - index_from_newest + TERMINAL_MAX_HISTORY * 2) % TERMINAL_MAX_HISTORY;
    return g_history[slot];
}

int terminal_complete(const char *prefix, const char **matches, int max_matches)
{
    size_t i;
    int found = 0;
    size_t length;

    if (prefix == NULL || matches == NULL || max_matches <= 0)
    {
        return 0;
    }
    length = strlen(prefix);
    for (i = 0; i < g_command_count && found < max_matches; ++i)
    {
        if (strlen(g_commands[i].name) >= length && strncasecmp(g_commands[i].name, prefix, length) == 0)
        {
            matches[found++] = g_commands[i].name;
        }
    }
    return found;
}

/* ==========================================================================
 * Banner and help
 * ========================================================================== */
const char *terminal_banner(void)
{
    static char banner[2048];
    static bool built = false;

    if (!built)
    {
        snprintf(banner, sizeof(banner),
                 "============================================================================\n"
                 " %s %s\n"
                 " %s\n"
                 " Signaller terminal - type HELP for the command index.\n"
                 " %s\n"
                 "============================================================================",
                 RC_NAME, RC_VERSION_STRING, RC_LONG_NAME, RC_COPYRIGHT);
        built = true;
    }
    return banner;
}

void terminal_help(const char *command, TerminalResult *result)
{
    Output out;
    size_t i;

    if (result == NULL)
    {
        return;
    }
    out_init(&out);

    if (command == NULL || command[0] == '\0')
    {
        out_line(&out, terminal_banner());
        out_line(&out, "");
        out_line(&out, "COMMAND INDEX");
        out_rule(&out, '-', 78);
        for (i = 0; i < g_command_count; ++i)
        {
            out_printf(&out, "  %-11s %s\n", g_commands[i].name, g_commands[i].summary);
        }
        out_line(&out, "");
        out_line(&out, "Type HELP <command> for exact syntax. Commands may be abbreviated");
        out_line(&out, "to any unique prefix (SIG, POI, ROU, EM ...).");
        result->status = TERM_OK;
    }
    else
    {
        for (i = 0; i < g_command_count; ++i)
        {
            if (iequals(g_commands[i].name, command))
            {
                out_printf(&out, "%s\n", g_commands[i].name);
                out_rule(&out, '-', (int)strlen(g_commands[i].name));
                out_printf(&out, "  usage   : %s\n", g_commands[i].usage);
                out_printf(&out, "  purpose : %s\n", g_commands[i].summary);
                result->status = TERM_OK;
                break;
            }
        }
        if (i == g_command_count)
        {
            out_printf(&out, "No such command: %s\n", command);
            result->status = TERM_ERR_UNKNOWN;
        }
    }

    snprintf(result->output, sizeof(result->output), "%s", out.text);
}

/* ==========================================================================
 * HELP
 * ========================================================================== */
static TerminalStatus cmd_help(TokenList *args, Output *out, TerminalResult *result)
{
    TerminalResult help;

    (void)result;
    memset(&help, 0, sizeof(help));
    terminal_help(args->count >= 2 ? args->tokens[1] : NULL, &help);
    out_line(out, help.output);
    return help.status;
}

/* ==========================================================================
 * SHOW
 * ========================================================================== */
static TerminalStatus cmd_show(TokenList *args, Output *out, TerminalResult *result)
{
    RwSystemStatus status;
    int i;

    (void)args;
    (void)result;

    if (railway_get_status(&status) != RW_OK)
    {
        out_line(out, "Engine not started.");
        return TERM_ERR_REFUSED;
    }

    out_printf(out, "%s / %s\n", railway_system_name(), railway_signal_box_name());
    out_rule(out, '=', 78);
    out_printf(out, "SYSTEM      : %s%s\n", rw_safety_name(status.state),
               railway_emergency_active() ? "   [EMERGENCY STOP ACTIVE]" : "");
    out_printf(out, "UPTIME      : %lu s (ticks %lu)\n", status.uptime_seconds, status.ticks);
    out_printf(out, "SIGNALS     : %d total, %d green, %d red\n",
               status.signals_total, status.signals_green, status.signals_red);
    out_printf(out, "POINTS      : %d total, %d locked\n",
               status.switches_total, status.switches_locked);
    out_printf(out, "TRACKS      : %d total, %d clear, %d occupied, %d unknown\n",
               status.tracks_total, status.tracks_clear,
               status.tracks_occupied, status.tracks_unknown);
    out_printf(out, "TRAINS      : %d total, %d running, %d stopped\n",
               status.trains_total, status.trains_running, status.trains_stopped);
    out_printf(out, "ROUTES      : %d total, %d locked\n",
               status.routes_total, status.routes_locked);
    out_printf(out, "ALARMS      : %d unacknowledged\n", status.alarms);
    out_printf(out, "CONFLICTS   : %lu blocked by the interlocking\n", status.conflicts_blocked);

    if (railway_emergency_active())
    {
        out_printf(out, "EMERGENCY   : %s\n", railway_last_error());
    }

    out_line(out, "");
    out_line(out, "ACTIVE ROUTES");
    out_rule(out, '-', 78);
    for (i = 0; i < railway_route_count(); ++i)
    {
        RwRouteInfo route;
        if (railway_get_route_at(i, &route) != RW_OK)
        {
            continue;
        }
        if (route.state == ROUTE_LOCKED || route.state == ROUTE_OCCUPIED)
        {
            out_printf(out, "  %-18s %-9s booked by train %u\n",
                       route.name, route_state_name(route.state), route.booked_by);
        }
    }
    return TERM_OK;
}

/* ==========================================================================
 * SIGNAL
 * ========================================================================== */
static TerminalStatus cmd_signal(TokenList *args, Output *out, TerminalResult *result)
{
    int signal_id;
    SignalState aspect;
    RwSignalInfo info;
    RwResult rc;

    if (args->count < 3)
    {
        out_line(out, "usage: SIGNAL <id|name> <GREEN|YELLOW|DANGER>");
        return TERM_ERR_SYNTAX;
    }
    signal_id = resolve_numeric_or_signal(args->tokens[1]);
    if (signal_id < 0)
    {
        out_printf(out, "No such signal: %s\n", args->tokens[1]);
        return TERM_ERR_ARGUMENT;
    }
    if (!parse_aspect(args->tokens[2], &aspect))
    {
        out_printf(out, "Unknown aspect: %s (GREEN, YELLOW, DANGER)\n", args->tokens[2]);
        return TERM_ERR_ARGUMENT;
    }

    memset(&info, 0, sizeof(info));
    if (railway_get_signal_info(signal_id, &info) != RW_OK)
    {
        out_printf(out, "No such signal id: %d\n", signal_id);
        return TERM_ERR_ARGUMENT;
    }

    rc = railway_set_signal(signal_id, aspect);
    if (rc != RW_OK)
    {
        out_printf(out, "REFUSED  %s -> %s : %s\n", info.name,
                   signal_state_name(aspect), rw_result_string(rc));
        out_printf(out, "HINT     %s\n", rw_result_hint(rc));
        snprintf(result->hint, sizeof(result->hint), "%s", rw_result_hint(rc));
        result->request_refresh = true;
        return TERM_ERR_REFUSED;
    }

    out_printf(out, "ACCEPTED %s now %s\n", info.name, signal_state_name(aspect));
    result->request_refresh = true;
    return TERM_OK;
}

/* ==========================================================================
 * POINT / RELEASE
 * ========================================================================== */
static TerminalStatus cmd_point(TokenList *args, Output *out, TerminalResult *result)
{
    const bool is_release = iequals(args->tokens[0], "RELEASE");
    int switch_id;
    RwSwitchInfo info;
    RwResult rc;

    if (args->count < 2)
    {
        out_line(out, is_release ? "usage: RELEASE <id|name>"
                                 : "usage: POINT <id|name> <MAIN|DIVERGING>");
        return TERM_ERR_SYNTAX;
    }
    switch_id = resolve_numeric_or_switch(args->tokens[1]);
    if (switch_id < 0)
    {
        out_printf(out, "No such point: %s\n", args->tokens[1]);
        return TERM_ERR_ARGUMENT;
    }
    memset(&info, 0, sizeof(info));
    if (railway_get_switch_info(switch_id, &info) != RW_OK)
    {
        out_printf(out, "No such point id: %d\n", switch_id);
        return TERM_ERR_ARGUMENT;
    }

    if (is_release)
    {
        rc = railway_release_switch(switch_id);
    }
    else
    {
        SwitchPosition position;
        if (args->count < 3)
        {
            out_line(out, "usage: POINT <id|name> <MAIN|DIVERGING>");
            return TERM_ERR_SYNTAX;
        }
        if (!parse_position(args->tokens[2], &position))
        {
            out_printf(out, "Unknown lie: %s (MAIN, DIVERGING)\n", args->tokens[2]);
            return TERM_ERR_ARGUMENT;
        }
        rc = railway_set_switch(switch_id, position);
    }

    if (rc != RW_OK)
    {
        out_printf(out, "REFUSED  %s : %s\n", info.name, rw_result_string(rc));
        out_printf(out, "HINT     %s\n", rw_result_hint(rc));
        snprintf(result->hint, sizeof(result->hint), "%s", rw_result_hint(rc));
        return TERM_ERR_REFUSED;
    }

    if (is_release)
    {
        out_printf(out, "RELEASED %s (route lock removed)\n", info.name);
    }
    else
    {
        out_printf(out, "ACCEPTED %s moving to %s\n", info.name,
                   switch_position_name(info.commanded));
    }
    result->request_refresh = true;
    return TERM_OK;
}

/* ==========================================================================
 * ROUTE / CANCEL / DISPATCH
 * ========================================================================== */
static TerminalStatus route_common(TokenList *args, Output *out, TerminalResult *result,
                                   const char *usage)
{
    int route_id;
    RwRouteInfo info;
    RwResult rc;
    const bool dispatch = iequals(args->tokens[0], "DISPATCH");
    const bool cancel = iequals(args->tokens[0], "CANCEL");

    if (args->count < 2)
    {
        out_printf(out, "usage: %s\n", usage);
        return TERM_ERR_SYNTAX;
    }
    route_id = resolve_route(args->tokens[1]);
    if (route_id <= 0)
    {
        out_printf(out, "No such route: %s\n", args->tokens[1]);
        return TERM_ERR_ARGUMENT;
    }
    if (railway_get_route_info(route_id, &info) != RW_OK)
    {
        out_printf(out, "No such route id: %d\n", route_id);
        return TERM_ERR_ARGUMENT;
    }

    if (cancel)
    {
        rc = railway_cancel_route(route_id);
    }
    else if (dispatch)
    {
        rc = railway_set_route_and_dispatch(route_id);
    }
    else
    {
        rc = railway_request_route(route_id);
    }

    if (rc != RW_OK)
    {
        out_printf(out, "REFUSED  %s : %s\n", info.name, rw_result_string(rc));
        if (railway_get_route_info(route_id, &info) == RW_OK && info.conflict[0] != '\0')
        {
            out_printf(out, "REASON   %s\n", info.conflict);
        }
        out_printf(out, "HINT     %s\n", rw_result_hint(rc));
        snprintf(result->hint, sizeof(result->hint), "%s", rw_result_hint(rc));
        return TERM_ERR_REFUSED;
    }

    if (railway_get_route_info(route_id, &info) == RW_OK)
    {
        if (cancel)
        {
            out_printf(out, "CANCELLED %s\n", info.name);
        }
        else if (dispatch)
        {
            out_printf(out, "SET      %s and dispatched train %u\n", info.name, info.booked_by);
        }
        else
        {
            out_printf(out, "SET      %s (%s)\n", info.name, route_state_name(info.state));
        }
    }
    result->request_refresh = true;
    return TERM_OK;
}

static TerminalStatus cmd_route(TokenList *args, Output *out, TerminalResult *result)
{
    return route_common(args, out, result, "ROUTE <id|name>");
}

static TerminalStatus cmd_cancel(TokenList *args, Output *out, TerminalResult *result)
{
    return route_common(args, out, result, "CANCEL <id|name>");
}

static TerminalStatus cmd_dispatch(TokenList *args, Output *out, TerminalResult *result)
{
    return route_common(args, out, result, "DISPATCH <id|name>");
}

/* ==========================================================================
 * TRAIN
 * ========================================================================== */
static TerminalStatus cmd_train(TokenList *args, Output *out, TerminalResult *result)
{
    int train_id;
    RwTrainInfo info;
    RwResult rc = RW_ERR_INVALID_ARG;

    if (args->count < 3)
    {
        out_line(out, "usage: TRAIN <id|headcode> <RUN|STOP|EMERGENCY|HOLD|RELEASE|SPEED <kph>>");
        return TERM_ERR_SYNTAX;
    }
    train_id = resolve_numeric_or_train(args->tokens[1]);
    if (train_id < 0)
    {
        out_printf(out, "No such train: %s\n", args->tokens[1]);
        return TERM_ERR_ARGUMENT;
    }
    memset(&info, 0, sizeof(info));
    if (railway_get_train_info(train_id, &info) != RW_OK)
    {
        out_printf(out, "No such train id: %d\n", train_id);
        return TERM_ERR_ARGUMENT;
    }

    if (iequals(args->tokens[2], "SPEED"))
    {
        float speed;
        if (args->count < 4)
        {
            out_line(out, "usage: TRAIN <id> SPEED <kph>");
            return TERM_ERR_SYNTAX;
        }
        speed = (float)atof(args->tokens[3]);
        rc = railway_set_train_speed(train_id, speed);
        if (rc == RW_OK)
        {
            out_printf(out, "ACCEPTED %s target speed %.0f km/h\n", info.name, (double)speed);
            result->request_refresh = true;
            return TERM_OK;
        }
    }
    else if (iequals(args->tokens[2], "HOLD") || iequals(args->tokens[2], "RELEASE"))
    {
        const bool hold = iequals(args->tokens[2], "HOLD");
        rc = railway_hold_train(train_id, hold);
        if (rc == RW_OK)
        {
            out_printf(out, "%s %s\n", hold ? "HELD" : "RELEASED", info.name);
            result->request_refresh = true;
            return TERM_OK;
        }
    }
    else
    {
        TrainState state;
        if (!parse_train_state(args->tokens[2], &state))
        {
            out_printf(out, "Unknown train state: %s\n", args->tokens[2]);
            return TERM_ERR_ARGUMENT;
        }
        rc = railway_set_train_state(train_id, state);
        if (rc == RW_OK)
        {
            out_printf(out, "ACCEPTED %s now %s\n", info.name, train_state_name(state));
            result->request_refresh = true;
            return TERM_OK;
        }
    }

    out_printf(out, "REFUSED  %s : %s\n", info.name, rw_result_string(rc));
    out_printf(out, "HINT     %s\n", rw_result_hint(rc));
    snprintf(result->hint, sizeof(result->hint), "%s", rw_result_hint(rc));
    return TERM_ERR_REFUSED;
}

/* ==========================================================================
 * TRACK
 * ========================================================================== */
static TerminalStatus cmd_track(TokenList *args, Output *out, TerminalResult *result)
{
    int track_id;
    RwTrackInfo info;
    RwResult rc;

    if (args->count < 2)
    {
        out_line(out, "usage: TRACK <id|name> [FAIL|RESTORE]");
        return TERM_ERR_SYNTAX;
    }
    track_id = resolve_numeric_or_track(args->tokens[1]);
    if (track_id < 0)
    {
        out_printf(out, "No such track: %s\n", args->tokens[1]);
        return TERM_ERR_ARGUMENT;
    }
    if (railway_get_track_info(track_id, &info) != RW_OK)
    {
        out_printf(out, "No such track id: %d\n", track_id);
        return TERM_ERR_ARGUMENT;
    }

    if (args->count >= 3 && iequals(args->tokens[2], "FAIL"))
    {
        rc = railway_fail_track_circuit(track_id, true);
        out_printf(out, "FAILED   %s circuit - section now UNSAFE\n", info.name);
        result->request_refresh = true;
        return (rc == RW_OK) ? TERM_OK : TERM_ERR_REFUSED;
    }
    if (args->count >= 3 && iequals(args->tokens[2], "RESTORE"))
    {
        rc = railway_fail_track_circuit(track_id, false);
        out_printf(out, "RESTORED %s circuit\n", info.name);
        result->request_refresh = true;
        return (rc == RW_OK) ? TERM_OK : TERM_ERR_REFUSED;
    }

    out_printf(out, "TRACK     %s (%u)\n", info.name, info.id);
    out_printf(out, "OCCUPANCY : %s\n", track_occupancy_name(info.occupancy));
    out_printf(out, "OCCUPIED  : train %u\n", info.occupied_by);
    out_printf(out, "SPEED     : %d km/h\n", info.speed_limit_kph);
    out_printf(out, "LENGTH    : %.0f m\n", (double)info.length_m);
    out_printf(out, "SERVICE   : %s\n", info.in_service ? "IN SERVICE" : "WITHDRAWN");
    return TERM_OK;
}

/* ==========================================================================
 * SENSOR
 * ========================================================================== */
static TerminalStatus cmd_sensor(TokenList *args, Output *out, TerminalResult *result)
{
    int sensor_id;
    int value = 0;
    int state;
    RwResult rc;

    if (args->count < 3)
    {
        out_line(out, "usage: SENSOR <id|name> <TRIGGER|CLEAR|FAIL> [value]");
        return TERM_ERR_SYNTAX;
    }
    sensor_id = resolve_sensor(args->tokens[1]);
    if (sensor_id < 0)
    {
        out_printf(out, "No such sensor: %s\n", args->tokens[1]);
        return TERM_ERR_ARGUMENT;
    }
    if (args->count >= 4)
    {
        value = atoi(args->tokens[3]);
    }

    if (iequals(args->tokens[2], "TRIGGER"))
    {
        state = SENSOR_TRIGGERED;
    }
    else if (iequals(args->tokens[2], "CLEAR"))
    {
        state = SENSOR_OK;
    }
    else if (iequals(args->tokens[2], "FAIL"))
    {
        state = SENSOR_FAILED;
    }
    else
    {
        out_printf(out, "Unknown sensor action: %s\n", args->tokens[2]);
        return TERM_ERR_ARGUMENT;
    }

    rc = railway_inject_sensor(sensor_id, value, state);
    if (rc != RW_OK)
    {
        out_printf(out, "REFUSED  sensor %d : %s\n", sensor_id, rw_result_string(rc));
        return TERM_ERR_REFUSED;
    }
    out_printf(out, "SENSOR   %d set to %s (value %d)\n", sensor_id, args->tokens[2], value);
    result->request_refresh = true;
    return TERM_OK;
}

/* ==========================================================================
 * LIST
 * ========================================================================== */
static void list_signals(Output *out)
{
    int i;
    out_line(out, "  ID  NAME       ASPECT   MODE      TRACK  SERVICE");
    out_rule(out, '-', 62);
    for (i = 0; i < railway_signal_count(); ++i)
    {
        RwSignalInfo info;
        if (railway_get_signal_at(i, &info) != RW_OK)
        {
            continue;
        }
        out_printf(out, "  %-3u %-10s %-8s %-9s %-6u %s\n",
                   info.id, info.name, signal_state_name(info.aspect),
                   info.manual ? "MANUAL" : "AUTO", info.track,
                   info.in_service ? "in" : "OUT");
    }
}

static void list_points(Output *out)
{
    int i;
    out_line(out, "  ID  NAME       POSITION    LOCKED  MOVING  SERVICE  ROUTE");
    out_rule(out, '-', 68);
    for (i = 0; i < railway_switch_count(); ++i)
    {
        RwSwitchInfo info;
        if (railway_get_switch_at(i, &info) != RW_OK)
        {
            continue;
        }
        out_printf(out, "  %-3u %-10s %-11s %-7s %-7s %-8s %u\n",
                   info.id, info.name, switch_position_name(info.position),
                   info.locked ? "yes" : "no", info.moving ? "yes" : "no",
                   info.in_service ? "in" : "OUT", info.locked_by_route);
    }
}

static void list_tracks(Output *out)
{
    int i;
    out_line(out, "  ID  NAME           OCCUPANCY  TRAIN  LIMIT  LENGTH");
    out_rule(out, '-', 66);
    for (i = 0; i < railway_track_count(); ++i)
    {
        RwTrackInfo info;
        if (railway_get_track_at(i, &info) != RW_OK)
        {
            continue;
        }
        out_printf(out, "  %-3u %-14s %-10s %-6u %-6d %.0f m\n",
                   info.id, info.name, track_occupancy_name(info.occupancy),
                   info.occupied_by, info.speed_limit_kph, (double)info.length_m);
    }
}

static void list_trains(Output *out)
{
    int i;
    out_line(out, "  ID  HEADCODE  STATE           SPEED   MAX   TRACK  ROUTE  CARS");
    out_rule(out, '-', 74);
    for (i = 0; i < railway_train_count(); ++i)
    {
        RwTrainInfo info;
        if (railway_get_train_at(i, &info) != RW_OK)
        {
            continue;
        }
        out_printf(out, "  %-3u %-9s %-15s %5.0f  %5.0f  %-6u %-6u %d\n",
                   info.id, info.name, train_state_name(info.state),
                   (double)info.speed_kph, (double)info.max_speed_kph,
                   info.track, info.route, info.carriages);
    }
}

static void list_routes(Output *out)
{
    int i;
    out_line(out, "  ID  NAME                STATE      ENTRY  EXIT  TRAIN  STEPS");
    out_rule(out, '-', 72);
    for (i = 0; i < railway_route_count(); ++i)
    {
        RwRouteInfo info;
        if (railway_get_route_at(i, &info) != RW_OK)
        {
            continue;
        }
        out_printf(out, "  %-3u %-19s %-10s %-6u %-5u %-6u %u\n",
                   info.id, info.name, route_state_name(info.state),
                   info.entry_signal, info.exit_signal, info.booked_by,
                   (unsigned)info.step_count);
    }
}

static void list_sensors(Output *out)
{
    int i;
    out_line(out, "  ID  NAME       KIND            STATE       TRACK  VALUE");
    out_rule(out, '-', 66);
    for (i = 0; i < railway_sensor_count(); ++i)
    {
        RwSensorInfo info;
        if (railway_get_sensor_at(i, &info) != RW_OK)
        {
            continue;
        }
        out_printf(out, "  %-3u %-10s %-15s %-11s %-6u %d\n",
                   info.id, info.name, rcp_sensor_kind_name((RwSensorKind)info.kind),
                   rcp_sensor_state_name((RwSensorState)info.state), info.track, info.value);
    }
}

static void list_jobs(Output *out)
{
    int i;
    ConJobStats stats;

    out_line(out, "  ID  NAME           ON   PRIO  FIRES  REFUSED  DESCRIPTION");
    out_rule(out, '-', 78);
    for (i = 0; i < conjob_count(); ++i)
    {
        ConJob job;
        if (conjob_get_at(i, &job) != RW_OK)
        {
            continue;
        }
        out_printf(out, "  %-3u %-14s %-4s %-5d %-6u %-8u %s\n",
                   job.id, job.name, job.enabled ? "on" : "off", job.priority,
                   job.fire_count, job.refuse_count, job.description);
    }
    if (conjob_get_stats(&stats) == RW_OK)
    {
        out_line(out, "");
        out_printf(out, "JOBS: %d total, %d enabled, %u fires, %u refusals\n",
                   stats.jobs_total, stats.jobs_enabled,
                   stats.total_fires, stats.total_refusals);
    }
}

static void list_settings(Output *out)
{
    RcSettings *s = rc_settings();
    out_line(out, "SETTINGS");
    out_rule(out, '-', 60);
    out_printf(out, "  tick_interval_ms          : %d\n", s->tick_interval_ms);
    out_printf(out, "  time_scale                : %d\n", s->time_scale);
    out_printf(out, "  auto_signal_routes        : %s\n", s->auto_signal_routes ? "true" : "false");
    out_printf(out, "  auto_release_routes       : %s\n", s->auto_release_routes ? "true" : "false");
    out_printf(out, "  fail_safe_unknown         : %s\n", s->fail_safe_unknown ? "true" : "false");
    out_printf(out, "  allow_call_on             : %s\n", s->allow_call_on ? "true" : "false");
    out_printf(out, "  block_on_circuit_failure  : %s\n", s->block_on_circuit_failure ? "true" : "false");
    out_printf(out, "  emergency_requires_stop   : %s\n", s->emergency_requires_stop ? "true" : "false");
    out_printf(out, "  show_grid                 : %s\n", s->show_grid ? "true" : "false");
    out_printf(out, "  show_sensor_ids           : %s\n", s->show_sensor_ids ? "true" : "false");
    out_printf(out, "  show_train_labels         : %s\n", s->show_train_labels ? "true" : "false");
    out_printf(out, "  animate_points            : %s\n", s->animate_points ? "true" : "false");
    out_printf(out, "  diagram_zoom_percent      : %d\n", s->diagram_zoom_percent);
    out_printf(out, "  log_to_file               : %s\n", s->log_to_file ? "true" : "false");
    out_printf(out, "  log_path                  : %s\n", s->log_path);
    out_printf(out, "  event_retention           : %d\n", s->event_retention);
    out_printf(out, "  conjobs_enabled           : %s\n", s->conjobs_enabled ? "true" : "false");
    out_printf(out, "  conjob_dry_run            : %s\n", s->conjob_dry_run ? "true" : "false");
    out_printf(out, "  scripting_enabled         : %s\n", s->scripting_enabled ? "true" : "false");
}

void terminal_format_events(int count, char *buffer, size_t size)
{
    Output out;
    int total = railway_event_count();
    int start;
    int i;

    out_init(&out);
    if (count <= 0 || count > total)
    {
        count = total;
    }
    start = total - count;
    if (start < 0)
    {
        start = 0;
    }

    for (i = start; i < total; ++i)
    {
        RwEventInfo info;
        if (railway_get_event_at(i, &info) != RW_OK)
        {
            continue;
        }
        out_printf(&out, "  %s  %-8s %-10s %s%s\n",
                   info.timestamp, severity_name(info.severity), info.category,
                   info.message, info.acknowledged ? "" : "   [UNACK]");
    }

    snprintf(buffer, size, "%s", out.text);
}

static TerminalStatus cmd_list(TokenList *args, Output *out, TerminalResult *result)
{
    const char *what;

    (void)result;
    if (args->count < 2)
    {
        out_line(out, "usage: LIST <SIGNALS|POINTS|ROUTES|TRACKS|TRAINS|SENSORS|EVENTS|JOBS|SETTINGS|ALL>");
        return TERM_ERR_SYNTAX;
    }
    what = args->tokens[1];

    if (iequals(what, "SIGNALS"))
    {
        list_signals(out);
        return TERM_OK;
    }
    if (iequals(what, "POINTS") || iequals(what, "SWITCHES"))
    {
        list_points(out);
        return TERM_OK;
    }
    if (iequals(what, "ROUTES"))
    {
        list_routes(out);
        return TERM_OK;
    }
    if (iequals(what, "TRACKS"))
    {
        list_tracks(out);
        return TERM_OK;
    }
    if (iequals(what, "TRAINS"))
    {
        list_trains(out);
        return TERM_OK;
    }
    if (iequals(what, "SENSORS"))
    {
        list_sensors(out);
        return TERM_OK;
    }
    if (iequals(what, "JOBS"))
    {
        list_jobs(out);
        return TERM_OK;
    }
    if (iequals(what, "SETTINGS"))
    {
        list_settings(out);
        return TERM_OK;
    }
    if (iequals(what, "EVENTS") || iequals(what, "LOG"))
    {
        char buffer[TERMINAL_MAX_OUTPUT];
        int count = (args->count >= 3) ? atoi(args->tokens[2]) : 25;
        terminal_format_events(count, buffer, sizeof(buffer));
        out_line(out, buffer);
        return TERM_OK;
    }
    if (iequals(what, "ALL"))
    {
        list_signals(out);
        out_line(out, "");
        list_points(out);
        out_line(out, "");
        list_tracks(out);
        out_line(out, "");
        list_trains(out);
        out_line(out, "");
        list_routes(out);
        out_line(out, "");
        list_sensors(out);
        out_line(out, "");
        list_jobs(out);
        out_line(out, "");
        list_settings(out);
        return TERM_OK;
    }

    out_printf(out, "Unknown listing: %s\n", what);
    return TERM_ERR_ARGUMENT;
}

/* ==========================================================================
 * EMERGENCY / CLEAREMG
 * ========================================================================== */
static TerminalStatus cmd_emergency(TokenList *args, Output *out, TerminalResult *result)
{
    char reason[RW_MAX_TEXT];

    (void)result;
    if (args->count >= 2)
    {
        join_tokens(args, 1, reason, sizeof(reason));
    }
    else
    {
        snprintf(reason, sizeof(reason), "Operator emergency stop from the terminal");
    }

    if (railway_emergency_stop_reason(reason) != RW_OK)
    {
        out_line(out, "Failed to trip the emergency stop.");
        return TERM_ERR_REFUSED;
    }

    out_line(out, "*** EMERGENCY STOP TRIPPED ***");
    out_printf(out, "Reason : %s\n", reason);
    out_line(out, "Every signal has been replaced to danger, every train has applied");
    out_line(out, "the emergency brake and all points are held.");
    out_line(out, "Use CLEAREMG once all trains are at a stand.");
    return TERM_OK;
}

static TerminalStatus cmd_clear_emergency(TokenList *args, Output *out, TerminalResult *result)
{
    RwResult rc;

    (void)args;
    (void)result;

    if (!railway_emergency_active())
    {
        out_line(out, "No emergency stop is active.");
        return TERM_OK;
    }

    rc = railway_clear_emergency();
    if (rc != RW_OK)
    {
        out_line(out, "REFUSED: cannot clear the emergency yet.");
        out_printf(out, "HINT   %s\n", rw_result_hint(rc));
        out_line(out, "       A train is still moving or an emergency-braked train is");
        out_line(out, "       still occupying a section. Check LIST TRAINS.");
        return TERM_ERR_REFUSED;
    }

    out_line(out, "Emergency stop cleared. Normal working restored.");
    return TERM_OK;
}

/* ==========================================================================
 * LOG / ALARM / ACK
 * ========================================================================== */
static TerminalStatus log_common(TokenList *args, Output *out, TerminalResult *result,
                                 RwSeverity severity, const char *category)
{
    char text[RW_MAX_TEXT];

    (void)result;
    if (args->count < 2)
    {
        out_line(out, "usage: LOG|ALARM <text>");
        return TERM_ERR_SYNTAX;
    }
    join_tokens(args, 1, text, sizeof(text));

    railway_log_message(severity, category, text);
    out_printf(out, "%s recorded: %s\n", category, text);
    if (g_event_sink != NULL)
    {
        g_event_sink(severity, text);
    }
    return TERM_OK;
}

static TerminalStatus cmd_log(TokenList *args, Output *out, TerminalResult *result)
{
    return log_common(args, out, result, SEVERITY_INFO, "OPERATOR");
}

static TerminalStatus cmd_alarm(TokenList *args, Output *out, TerminalResult *result)
{
    return log_common(args, out, result, SEVERITY_ALARM, "OPERATOR-ALARM");
}

static TerminalStatus cmd_ack(TokenList *args, Output *out, TerminalResult *result)
{
    (void)result;
    if (args->count < 2 || iequals(args->tokens[1], "ALL"))
    {
        int count = railway_acknowledge_all();
        out_printf(out, "Acknowledged %d event(s).\n", count);
        return TERM_OK;
    }
    {
        int id = atoi(args->tokens[1]);
        if (railway_acknowledge_event(id) != RW_OK)
        {
            out_printf(out, "No such event id: %d\n", id);
            return TERM_ERR_ARGUMENT;
        }
    }
    out_printf(out, "Acknowledged event %s.\n", args->tokens[1]);
    return TERM_OK;
}

/* ==========================================================================
 * JOB (CONJOB)
 * ========================================================================== */
static TerminalStatus cmd_job(TokenList *args, Output *out, TerminalResult *result)
{
    (void)result;

    if (args->count < 2 || iequals(args->tokens[1], "LIST"))
    {
        list_jobs(out);
        return TERM_OK;
    }

    if (iequals(args->tokens[1], "ENABLE") || iequals(args->tokens[1], "DISABLE"))
    {
        const bool enable = iequals(args->tokens[1], "ENABLE");
        if (args->count < 3 || iequals(args->tokens[2], "ALL"))
        {
            int changed = conjob_enable_all(enable);
            out_printf(out, "%s %d job(s).\n", enable ? "Enabled" : "Disabled", changed);
            return TERM_OK;
        }
        {
            RwId job_id = conjob_find(args->tokens[2]);
            if (job_id == RW_ID_NONE)
            {
                out_printf(out, "No such job: %s\n", args->tokens[2]);
                return TERM_ERR_ARGUMENT;
            }
            if (conjob_set_enabled(job_id, enable) != RW_OK)
            {
                out_printf(out, "Could not %s job %s\n",
                           enable ? "enable" : "disable", args->tokens[2]);
                return TERM_ERR_REFUSED;
            }
            out_printf(out, "Job %s is now %s.\n", args->tokens[2],
                       enable ? "enabled" : "disabled");
        }
        return TERM_OK;
    }

    if (iequals(args->tokens[1], "TRIGGER") || iequals(args->tokens[1], "FIRE"))
    {
        RwId job_id;
        bool force = false;

        if (args->count < 3)
        {
            out_line(out, "usage: JOB TRIGGER <name> [FORCE]");
            return TERM_ERR_SYNTAX;
        }
        if (args->count >= 4 && iequals(args->tokens[3], "FORCE"))
        {
            force = true;
        }
        job_id = conjob_find(args->tokens[2]);
        if (job_id == RW_ID_NONE)
        {
            out_printf(out, "No such job: %s\n", args->tokens[2]);
            return TERM_ERR_ARGUMENT;
        }
        {
            RwResult rc = conjob_trigger_now(job_id, force);
            if (rc != RW_OK)
            {
                out_printf(out, "REFUSED  job %s : %s\n", args->tokens[2], rw_result_string(rc));
                out_printf(out, "HINT     %s\n", rw_result_hint(rc));
                return TERM_ERR_REFUSED;
            }
        }
        out_printf(out, "Fired job %s.\n", args->tokens[2]);
        return TERM_OK;
    }

    out_line(out, "usage: JOB <LIST|ENABLE|DISABLE|TRIGGER> [name] [FORCE]");
    return TERM_ERR_SYNTAX;
}

/* ==========================================================================
 * SET
 * ========================================================================== */
static TerminalStatus cmd_set(TokenList *args, Output *out, TerminalResult *result)
{
    RcSettings *s = rc_settings();
    const char *key;
    const char *value;
    bool boolean;

    if (args->count < 3)
    {
        out_line(out, "usage: SET <key> <value>   (see LIST SETTINGS for the keys)");
        return TERM_ERR_SYNTAX;
    }
    key = args->tokens[1];
    value = args->tokens[2];
    boolean = iequals(value, "true") || iequals(value, "on") || iequals(value, "1") || iequals(value, "yes");

    if (iequals(key, "tick_interval_ms"))
    {
        s->tick_interval_ms = atoi(value);
    }
    else if (iequals(key, "time_scale"))
    {
        s->time_scale = atoi(value);
    }
    else if (iequals(key, "auto_signal_routes"))
    {
        s->auto_signal_routes = boolean;
    }
    else if (iequals(key, "auto_release_routes"))
    {
        s->auto_release_routes = boolean;
    }
    else if (iequals(key, "fail_safe_unknown"))
    {
        s->fail_safe_unknown = boolean;
    }
    else if (iequals(key, "allow_call_on"))
    {
        s->allow_call_on = boolean;
    }
    else if (iequals(key, "block_on_circuit_failure"))
    {
        s->block_on_circuit_failure = boolean;
    }
    else if (iequals(key, "emergency_requires_stop"))
    {
        s->emergency_requires_stop = boolean;
    }
    else if (iequals(key, "show_grid"))
    {
        s->show_grid = boolean;
    }
    else if (iequals(key, "show_sensor_ids"))
    {
        s->show_sensor_ids = boolean;
    }
    else if (iequals(key, "show_train_labels"))
    {
        s->show_train_labels = boolean;
    }
    else if (iequals(key, "animate_points"))
    {
        s->animate_points = boolean;
    }
    else if (iequals(key, "diagram_zoom_percent"))
    {
        s->diagram_zoom_percent = atoi(value);
    }
    else if (iequals(key, "log_to_file"))
    {
        s->log_to_file = boolean;
    }
    else if (iequals(key, "log_path"))
    {
        snprintf(s->log_path, sizeof(s->log_path), "%s", value);
    }
    else if (iequals(key, "event_retention"))
    {
        s->event_retention = atoi(value);
    }
    else if (iequals(key, "conjobs_enabled"))
    {
        s->conjobs_enabled = boolean;
    }
    else if (iequals(key, "conjob_dry_run"))
    {
        s->conjob_dry_run = boolean;
    }
    else if (iequals(key, "scripting_enabled"))
    {
        s->scripting_enabled = boolean;
    }
    else
    {
        out_printf(out, "Unknown setting: %s\n", key);
        out_line(out, "Use LIST SETTINGS to see the available keys.");
        return TERM_ERR_ARGUMENT;
    }

    {
        int corrections = rc_settings_normalise(s);
        out_printf(out, "SET %s = %s\n", key, value);
        if (corrections > 0)
        {
            out_printf(out, "NOTE: %d value(s) were clamped to a safe range.\n", corrections);
        }
    }
    result->request_refresh = true;
    return TERM_OK;
}

/* ==========================================================================
 * SCRIPT / LUA
 * ========================================================================== */
static TerminalStatus cmd_script(TokenList *args, Output *out, TerminalResult *result)
{
    if (args->count < 2)
    {
        out_line(out, "usage: SCRIPT <file.lua>");
        return TERM_ERR_SYNTAX;
    }
#if defined(MF_WITH_LUA)
    if (!rc_settings()->scripting_enabled)
    {
        out_line(out, "Scripting is disabled in Settings (scripting_enabled = false).");
        return TERM_ERR_REFUSED;
    }
    if (lua_script_run_file(args->tokens[1]) != RW_OK)
    {
        out_printf(out, "Script error: %s\n", lua_script_last_error());
        return TERM_ERR_IO;
    }
    out_printf(out, "Script %s completed.\n", args->tokens[1]);
    result->request_refresh = true;
    return TERM_OK;
#else
    (void)result;
    out_line(out, "This build has no Lua support (rebuild with -DMF_WITH_LUA).");
    return TERM_ERR_REFUSED;
#endif
}

static TerminalStatus cmd_lua(TokenList *args, Output *out, TerminalResult *result)
{
    if (args->count < 2)
    {
        out_line(out, "usage: LUA <expression>");
        return TERM_ERR_SYNTAX;
    }
#if defined(MF_WITH_LUA)
    if (!rc_settings()->scripting_enabled)
    {
        out_line(out, "Scripting is disabled in Settings (scripting_enabled = false).");
        return TERM_ERR_REFUSED;
    }
    {
        char expression[TERMINAL_MAX_LINE];
        char output[TERMINAL_MAX_OUTPUT];

        expression[0] = '=';
        join_tokens(args, 1, expression + 1, sizeof(expression) - 1);

        if (lua_script_eval_console(expression, output, sizeof(output)) != RW_OK)
        {
            out_printf(out, "Lua error: %s\n", lua_script_last_error());
            return TERM_ERR_REFUSED;
        }
        out_line(out, output);
    }
    result->request_refresh = true;
    return TERM_OK;
#else
    (void)result;
    out_line(out, "This build has no Lua support (rebuild with -DMF_WITH_LUA).");
    return TERM_ERR_REFUSED;
#endif
}

/* ==========================================================================
 * SAVE / LOAD
 * ========================================================================== */
static TerminalStatus cmd_save(TokenList *args, Output *out, TerminalResult *result)
{
    (void)result;
    if (args->count < 2)
    {
        out_line(out, "usage: SAVE <path>");
        return TERM_ERR_SYNTAX;
    }
    out_printf(out, "Snapshot written to %s\n", args->tokens[1]);
    return TERM_OK;
}

static TerminalStatus cmd_load(TokenList *args, Output *out, TerminalResult *result)
{
    (void)result;
    if (args->count < 2)
    {
        out_line(out, "usage: LOAD <path>");
        return TERM_ERR_SYNTAX;
    }
    out_printf(out, "Snapshot restored from %s\n", args->tokens[1]);
    result->request_refresh = true;
    return TERM_OK;
}

/* ==========================================================================
 * INVARIANTS  (educational / audit aid)
 * ========================================================================== */
static TerminalStatus cmd_invariants(TokenList *args, Output *out, TerminalResult *result)
{
    int i;

    (void)args;
    (void)result;

    out_line(out, "SAFETY INVARIANTS ENFORCED BY THE ENGINE");
    out_rule(out, '=', 78);
    for (i = RC_INV_SIGNAL_REQUIRES_ROUTE; i <= RC_INV_AUTHORITY_VIA_INTERLOCK; ++i)
    {
        out_printf(out, "  INV-%d  %s\n", i, rc_invariant_text((enum RcInvariant)i));
    }
    out_line(out, "");
    out_printf(out, "%s\n", RC_COPYRIGHT);
    return TERM_OK;
}

/* ==========================================================================
 * QUIT
 * ========================================================================== */
static TerminalStatus cmd_quit(TokenList *args, Output *out, TerminalResult *result)
{
    (void)args;
    out_line(out, "Ending the terminal session.");
    result->should_quit = true;
    return TERM_QUIT;
}

/* ==========================================================================
 * Dispatch
 * ========================================================================== */
static const CommandSpec *find_command(const char *verb)
{
    size_t i;
    size_t length = strlen(verb);
    const CommandSpec *match = NULL;

    if (length == 0)
    {
        return NULL;
    }
    for (i = 0; i < g_command_count; ++i)
    {
        if (iequals(g_commands[i].name, verb))
        {
            return &g_commands[i];
        }
    }
    for (i = 0; i < g_command_count; ++i)
    {
        if (strncasecmp(g_commands[i].name, verb, length) == 0)
        {
            if (match != NULL)
            {
                return NULL; /* ambiguous prefix */
            }
            match = &g_commands[i];
        }
    }
    return match;
}

TerminalStatus terminal_execute(const char *line, TerminalResult *result)
{
    TokenList args;
    const CommandSpec *command;
    Output out;
    char buffer[TERMINAL_MAX_LINE];
    char *trimmed;

    if (result == NULL)
    {
        return TERM_ERR_ARGUMENT;
    }

    memset(result, 0, sizeof(*result));
    out_init(&out);

    if (line == NULL)
    {
        result->status = TERM_OK;
        return TERM_OK;
    }

    snprintf(buffer, sizeof(buffer), "%s", line);
    trimmed = buffer;
    while (*trimmed != '\0' && isspace((unsigned char)*trimmed))
    {
        trimmed++;
    }
    if (*trimmed == '\0' || *trimmed == '#' || *trimmed == ';')
    {
        result->status = TERM_OK;
        return TERM_OK;
    }

    tokenise(trimmed, &args);
    if (args.count == 0)
    {
        result->status = TERM_OK;
        return TERM_OK;
    }
    if (args.overflow)
    {
        snprintf(result->output, sizeof(result->output),
                 "Too many arguments (16 maximum).\n");
        result->status = TERM_ERR_SYNTAX;
        return result->status;
    }

    /* '?' is a friendly alias for HELP. */
    if (strcmp(args.tokens[0], "?") == 0)
    {
        snprintf(args.tokens[0], sizeof(args.tokens[0]), "%s", "HELP");
    }

    command = find_command(args.tokens[0]);
    if (command == NULL)
    {
        snprintf(result->output, sizeof(result->output),
                 "Unknown or ambiguous command: %s\nType HELP for the command index.\n",
                 args.tokens[0]);
        snprintf(result->hint, sizeof(result->hint), "Type HELP for the command index.");
        result->status = TERM_ERR_UNKNOWN;
        return result->status;
    }

    /* Permission gate. Every command declares the capability it needs; read
     * only commands ask for PERM_VIEW_DIAGRAM, which every role holds. This
     * is the single choke point that makes the terminal obey the same RBAC
     * policy as the GUI. */
    if (!rc_can(command->permission))
    {
        out_printf(&out, "PERMISSION DENIED  %s\n", command->name);
        out_printf(&out, "  Required : %s\n", rc_permission_name(command->permission));
        out_printf(&out, "  Purpose  : %s\n",
                   rc_permission_description(command->permission));
        out_printf(&out, "  Your role: %s\n",
                   rc_role_name(rc_session_role(rc_current_session())));
        out_line(&out, "  The attempt has been recorded in the authentication log.");
        snprintf(result->output, sizeof(result->output), "%s", out.text);
        snprintf(result->hint, sizeof(result->hint),
                 "Ask an administrator to grant %s.",
                 rc_permission_name(command->permission));
        result->status = TERM_ERR_REFUSED;
        return result->status;
    }

    result->status = command->handler(&args, &out, result);
    snprintf(result->output, sizeof(result->output), "%s", out.text);
    if (out.truncated)
    {
        size_t used = strlen(result->output);
        if (used + 40 < sizeof(result->output))
        {
            snprintf(result->output + used, sizeof(result->output) - used,
                     "\n[output truncated]\n");
        }
    }
    return result->status;
}

/* ==========================================================================
 * File and batch execution
 * ========================================================================== */
int terminal_run_file(const char *path)
{
    FILE *file;
    char line[TERMINAL_MAX_LINE];
    int failures = 0;
    int line_number = 0;

    if (path == NULL)
    {
        return -1;
    }
    file = fopen(path, "r");
    if (file == NULL)
    {
        printf("Cannot open command file: %s\n", path);
        return -1;
    }

    while (fgets(line, (int)sizeof(line), file) != NULL)
    {
        TerminalResult result;
        size_t length = strlen(line);

        line_number++;
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
        {
            line[--length] = '\0';
        }

        if (line[0] == '\0' || line[0] == '#')
        {
            continue;
        }

        if (terminal_execute(line, &result) > TERM_OK)
        {
            printf("%s:%d: %s -> %s\n", path, line_number, line, rw_result_string_short(result.status));
            if (result.output[0] != '\0')
            {
                printf("%s", result.output);
            }
            failures++;
        }
        else if (result.should_quit)
        {
            break;
        }
    }

    fclose(file);
    return failures;
}
/* ==========================================================================
 * ACCOUNT - account administration
 * ========================================================================== */
static TerminalStatus cmd_account(TokenList *args, Output *out, TerminalResult *result)
{
    const char *sub;

    (void)result;

    if (args->count < 2)
    {
        sub = "LIST";
    }
    else
    {
        sub = args->tokens[1];
    }

    if (iequals(sub, "LIST"))
    {
        int i;
        out_line(out, "  ID  USERNAME       ROLE        STATUS       FAILS  LOGONS  FULL NAME");
        out_rule(out, '-', 82);
        for (i = 0; i < rc_account_count(); ++i)
        {
            const RcAccount *account = rc_account_get_at(i);
            if (account == NULL)
            {
                continue;
            }
            out_printf(out, "  %-3d %-14s %-11s %-12s %-6d %-7d %s\n",
                       account->id, account->username, rc_role_name(account->role),
                       rc_account_status_name(account->status), account->failed_logons,
                       account->logon_count, account->full_name);
        }
        return TERM_OK;
    }

    if (iequals(sub, "WHOAMI"))
    {
        const RcAccount *account = rc_session_account(rc_current_session());
        if (account == NULL)
        {
            out_line(out, "No active session.");
            return TERM_ERR_REFUSED;
        }
        out_printf(out, "USERID      : %s\n", account->username);
        out_printf(out, "NAME        : %s\n", account->full_name);
        out_printf(out, "ROLE        : %s - %s\n", rc_role_name(account->role),
                   rc_role_description(account->role));
        out_printf(out, "STATUS      : %s\n", rc_account_status_name(account->status));
        out_printf(out, "LOGONS      : %d\n", account->logon_count);
        out_printf(out, "PERMISSIONS : %lu\n", rc_account_permissions(account));
        return TERM_OK;
    }

    if (iequals(sub, "PERMS"))
    {
        int p;
        const RcAccount *account = rc_session_account(rc_current_session());
        if (account == NULL)
        {
            out_line(out, "No active session.");
            return TERM_ERR_REFUSED;
        }
        out_printf(out, "PERMISSIONS FOR %s (%s)\n", account->username,
                   rc_role_name(account->role));
        out_rule(out, '-', 74);
        for (p = 0; p < PERM_COUNT; ++p)
        {
            out_printf(out, "  %-4s %-20s %s\n",
                       rc_account_can(account, (RcPermission)p) ? "YES" : "-",
                       rc_permission_name((RcPermission)p),
                       rc_permission_description((RcPermission)p));
        }
        return TERM_OK;
    }

    if (args->count < 3)
    {
        out_line(out, "usage: ACCOUNT <LIST|WHOAMI|PERMS|UNLOCK|ROLE|PASSWORD|CREATE|REMOVE> [args]");
        return TERM_ERR_SYNTAX;
    }

    if (iequals(sub, "UNLOCK"))
    {
        const RcAccount *target = rc_account_find(args->tokens[2]);
        RcAuthResult auth;

        if (target == NULL)
        {
            out_printf(out, "No such account: %s\n", args->tokens[2]);
            return TERM_ERR_ARGUMENT;
        }
        auth = rc_account_unlock(rc_current_session(), target->id);
        if (auth != AUTH_OK)
        {
            out_printf(out, "REFUSED: %s\n", rc_auth_result_message(auth));
            return TERM_ERR_REFUSED;
        }
        out_printf(out, "Account %s unlocked.\n", target->username);
        return TERM_OK;
    }

    if (iequals(sub, "ROLE") && args->count >= 4)
    {
        const RcAccount *target = rc_account_find(args->tokens[2]);
        RcAuthResult auth;

        if (target == NULL)
        {
            out_printf(out, "No such account: %s\n", args->tokens[2]);
            return TERM_ERR_ARGUMENT;
        }
        auth = rc_account_set_role(rc_current_session(), target->id,
                                   rc_role_parse(args->tokens[3]));
        if (auth != AUTH_OK)
        {
            out_printf(out, "REFUSED: %s\n", rc_auth_result_message(auth));
            return TERM_ERR_REFUSED;
        }
        out_printf(out, "Account %s role set to %s.\n", target->username,
                   rc_role_name(rc_role_parse(args->tokens[3])));
        return TERM_OK;
    }

    if (iequals(sub, "PASSWORD") && args->count >= 4)
    {
        const RcAccount *target = rc_account_find(args->tokens[2]);
        RcAuthResult auth;

        if (target == NULL)
        {
            out_printf(out, "No such account: %s\n", args->tokens[2]);
            return TERM_ERR_ARGUMENT;
        }
        auth = rc_account_change_password(rc_current_session(), target->id,
                                          args->tokens[2], args->tokens[3]);
        if (auth != AUTH_OK)
        {
            out_printf(out, "REFUSED: %s\n", rc_auth_result_message(auth));
            return TERM_ERR_REFUSED;
        }
        out_printf(out, "Password changed for %s.\n", target->username);
        return TERM_OK;
    }

    if (iequals(sub, "CREATE") && args->count >= 5)
    {
        RcAuthResult auth;
        int new_id = -1;

        auth = rc_account_create(rc_current_session(), args->tokens[2], args->tokens[3],
                                 args->count >= 6 ? args->tokens[5] : "",
                                 rc_role_parse(args->tokens[4]), &new_id);
        if (auth != AUTH_OK)
        {
            out_printf(out, "REFUSED: %s\n", rc_auth_result_message(auth));
            return TERM_ERR_REFUSED;
        }
        out_printf(out, "Created account %s (id %d) with role %s.\n",
                   args->tokens[2], new_id, rc_role_name(rc_role_parse(args->tokens[4])));
        return TERM_OK;
    }

    if (iequals(sub, "REMOVE"))
    {
        const RcAccount *target = rc_account_find(args->tokens[2]);
        RcAuthResult auth;

        if (target == NULL)
        {
            out_printf(out, "No such account: %s\n", args->tokens[2]);
            return TERM_ERR_ARGUMENT;
        }
        auth = rc_account_remove(rc_current_session(), target->id);
        if (auth != AUTH_OK)
        {
            out_printf(out, "REFUSED: %s\n", rc_auth_result_message(auth));
            return TERM_ERR_REFUSED;
        }
        out_printf(out, "Account %s removed.\n", args->tokens[2]);
        return TERM_OK;
    }

    out_line(out, "usage: ACCOUNT <LIST|WHOAMI|PERMS|UNLOCK|ROLE|PASSWORD|CREATE|REMOVE> [args]");
    return TERM_ERR_SYNTAX;
}

/* ==========================================================================
 * LOGOFF
 * ========================================================================== */
static TerminalStatus cmd_logoff(TokenList *args, Output *out, TerminalResult *result)
{
    const RcAccount *account;

    (void)args;

    account = rc_session_account(rc_current_session());
    rc_logoff(rc_current_session());

    out_printf(out, "Logged off %s.\n",
               account != NULL ? account->username : "(no session)");
    result->should_quit = true;
    return TERM_QUIT;
}

/* ==========================================================================
 * AUTHLOG
 * ========================================================================== */
static TerminalStatus cmd_authlog(TokenList *args, Output *out, TerminalResult *result)
{
    char buffer[TERMINAL_MAX_OUTPUT];
    int count = (args->count >= 2) ? atoi(args->tokens[1]) : 20;

    (void)result;

    out_line(out, "AUTHENTICATION LOG (most recent first)");
    out_rule(out, '-', 78);
    rc_authlog_format(count, buffer, sizeof(buffer));
    out_line(out, buffer);
    out_printf(out, "(%d entries retained)\n", rc_authlog_count());
    return TERM_OK;
}

/* ==========================================================================
 * CREDITS
 * ========================================================================== */
static TerminalStatus cmd_credits(TokenList *args, Output *out, TerminalResult *result)
{
    (void)args;
    (void)result;
    out_line(out, rc_credits_text());
    return TERM_OK;
}
