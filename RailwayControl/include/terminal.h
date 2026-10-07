/* ==========================================================================
 * terminal.h - 💻 Built-in control terminal (signaller command line).
 *
 * A SCADA-style textual interface to the same public API the GUI uses. It is
 * used in three places:
 *
 *   1. inside the GUI, as the "Terminal" pane at the bottom of the window
 *   2. standalone, when RailControl is started with --terminal
 *   3. in scripts / batch, when started with --command "..." for automation
 *
 * The command set mirrors what a signaller actually types at a control desk:
 *
 *   SIGNAL S3 GREEN          set an aspect (subject to the interlocking)
 *   SIGNAL S3 DANGER
 *   POINT P1 REVERSE         set a point
 *   RELEASE P1               release a point from its route lock
 *   ROUTE R2                 request (set) a route
 *   CANCEL R2                cancel a route
 *   DISPATCH R2              set a route and give authority to the first train
 *   TRAIN 1A34 RUN           set a train state
 *   TRAIN 1A34 SPEED 60      set a target speed
 *   TRAIN 1A34 HOLD          hold / RELEASE
 *   EMERGENCY [reason]       trip the emergency stop
 *   CLEAREMG                 clear the emergency stop
 *   TRACK T2                 show one track
 *   TRACK T2 FAIL / RESTORE  fail or restore a track circuit
 *   SENSOR TC3 TRIGGER       inject a sensor reading
 *   LIST SIGNALS|POINTS|ROUTES|TRACKS|TRAINS|SENSORS|EVENTS|JOBS
 *   SHOW                     overall status
 *   LOG <text>               write an INFO event
 *   ALARM <text>             write an ALARM event
 *   ACK [id|ALL]             acknowledge alarms
 *   JOB LIST|ENABLE|DISABLE|TRIGGER <name>
 *   SET <key> <value>        change a setting
 *   SCRIPT <file>            run a Lua script
 *   LUA <expression>         evaluate a Lua expression
 *   SAVE <path> / LOAD <path> state snapshot
 *   HELP [command] / ?       help
 *   QUIT / EXIT              leave (GUI terminal refuses)
 *
 * Every command that changes state goes through railway_api.h, so the
 * terminal can never bypass an interlocking proof.
 * ========================================================================== */
#ifndef TERMINAL_H
#define TERMINAL_H

#include "railway_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define TERMINAL_MAX_LINE 512
#define TERMINAL_MAX_HISTORY 128
#define TERMINAL_MAX_OUTPUT 8192

    /* Result of running one command line. */
    typedef enum
    {
        TERM_OK = 0,
        TERM_ERR_SYNTAX,   /* could not parse */
        TERM_ERR_UNKNOWN,  /* unknown command */
        TERM_ERR_REFUSED,  /* the engine refused (interlocking / emergency) */
        TERM_ERR_ARGUMENT, /* bad argument */
        TERM_ERR_IO,       /* file could not be read/written */
        TERM_QUIT          /* the command asked the session to end */
    } TerminalStatus;

    typedef struct
    {
        TerminalStatus status;
        char output[TERMINAL_MAX_OUTPUT];
        char hint[RW_MAX_TEXT];
        bool should_quit;
        bool request_refresh; /* GUI should repaint the diagram */
    } TerminalResult;

    /* --------------------------------------------------------------------------
     * Command execution
     * -------------------------------------------------------------------------- */

    /* Prepare the terminal (history table, command table). */
    void terminal_init(void);
    void terminal_shutdown(void);

    /* Run one line. `result` must be non-NULL and is fully initialised.
     * This is the single entry point used by the GUI pane, the standalone REPL
     * and the --command batch mode. */
    TerminalStatus terminal_execute(const char *line, TerminalResult *result);

    /* --------------------------------------------------------------------------
     * History and completion (used by the GUI pane's up/down keys)
     * -------------------------------------------------------------------------- */
    int terminal_history_count(void);
    const char *terminal_history_at(int index_from_newest);
    void terminal_history_add(const char *line);
    void terminal_history_clear(void);

    /* Command name completion. Returns the number of matches written into
     * `matches`; each match is a static string owned by the command table. */
    int terminal_complete(const char *prefix, const char **matches, int max_matches);

    /* --------------------------------------------------------------------------
     * Session helpers
     * -------------------------------------------------------------------------- */

    /* The multi-line welcome banner shown when the terminal starts. */
    const char *terminal_banner(void);

    /* Help text. `command` may be NULL or "" for the command index. */
    void terminal_help(const char *command, TerminalResult *result);

    /* Run a whole file of commands, one per line, ignoring blank lines and
     * comments starting with '#'. Returns the number of failed commands. */
    int terminal_run_file(const char *path);

    /* --------------------------------------------------------------------------
     * Event sink: the terminal mirrors every alarm into the GUI pane.
     * -------------------------------------------------------------------------- */
    void terminal_attach_event_sink(void (*sink)(RwSeverity severity, const char *line));

    /* Render the last `count` events as terminal text (used by LIST EVENTS). */
    void terminal_format_events(int count, char *buffer, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* TERMINAL_H */
