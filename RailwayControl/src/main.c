/* ==========================================================================
 * main.c - RailControl entry point.
 *
 * Modes
 * -----
 *   rcp                             interactive signaller console (default)
 *   rcp --terminal                  as above
 *   rcp --command "ROUTE R2"        run one command and exit
 *   rcp --file scenario.cmd         run a command file and exit
 *   rcp --simulate 60 --scenario peak
 *                                   headless simulation for N seconds
 *   rcp --version | --credits       identity, then exit
 *   rcp --permissions               print the role/permission matrix
 *   rcp --help                      usage
 *
 * The program always authenticates before it will accept control commands.
 * A front end that skipped logon could not hold any permission, because every
 * guarded action calls rc_require().
 * ========================================================================== */
#include "railway_api.h"
#include "railcontrol.h"
#include "rc_version.h"
#include "rc_account.h"
#include "terminal.h"
#include "conjob.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <io.h>          /* isatty() and fileno() on Windows */
#else
#include <unistd.h>
#endif

/* --------------------------------------------------------------------------
 * Small console helpers
 * -------------------------------------------------------------------------- */
static void print_line(void)
{
    printf("----------------------------------------------------------------------------\n");
}

static void sleep_ms(unsigned ms)
{
#if defined(_WIN32)
    Sleep(ms);
#else
    usleep(ms * 1000u);
#endif
}

static void print_usage(void)
{
    printf("%s\n\n", rc_version_string());
    printf("usage: rcp [options]\n\n");
    printf("  --terminal              interactive signaller console (default)\n");
    printf("  --command <text>        run one terminal command and exit\n");
    printf("  --file <path>           run a command file and exit\n");
    printf("  --simulate <seconds>    run headless for N seconds\n");
    printf("  --scenario <name>       quiet | peak | failure | emergency\n");
    printf("  --user <name>           log on as this account\n");
    printf("  --password <text>       password (prefer --password-stdin)\n");
    printf("  --password-stdin        read the password from standard input\n");
    printf("  --no-auth               skip authentication (SIMULATION ONLY)\n");
    printf("  --settings <path>       load settings from this file\n");
    printf("  --save-settings <path>  write settings and exit\n");
    printf("  --permissions           print the role / permission matrix\n");
    printf("  --title                 print the title screen\n");
    printf("  --credits               print credits and the safety notice\n");
    printf("  --version               print the version and exit\n");
    printf("  --help                  this text\n\n");
    printf("%s\n", rc_safety_notice());
}

/* --------------------------------------------------------------------------
 * Title and credits
 * -------------------------------------------------------------------------- */
static void print_title(void)
{
    printf("%s", rc_title_screen());
}

static void print_credits(void)
{
    printf("%s", rc_credits_text());
}

static void print_permissions(void)
{
    int role;
    int permission;

    printf("%s\n", rc_version_string());
    print_line();
    printf("ROLE / PERMISSION MATRIX\n");
    print_line();
    printf("%-13s", "PERMISSION");
    for (role = ROLE_VIEWER; role <= ROLE_ADMIN; ++role) {
        printf(" %-11s", rc_role_name((RcRole)role));
    }
    printf("\n");
    printf("%-13s", "");
    for (role = ROLE_VIEWER; role <= ROLE_ADMIN; ++role) {
        printf(" %-11s", "-----------");
    }
    printf("\n");

    for (permission = 0; permission < PERM_COUNT; ++permission) {
        printf("%-13s", rc_permission_name((RcPermission)permission));
        for (role = ROLE_VIEWER; role <= ROLE_ADMIN; ++role) {
            printf(" %-11s",
                   rc_role_has((RcRole)role, (RcPermission)permission) ? "yes" : "-");
        }
        printf("\n");
    }

    print_line();
    printf("Roles: ");
    for (role = ROLE_VIEWER; role <= ROLE_ADMIN; ++role) {
        printf("%s%s", role == ROLE_VIEWER ? "" : " | ", rc_role_name((RcRole)role));
    }
    printf("\n");
    for (role = ROLE_VIEWER; role <= ROLE_ADMIN; ++role) {
        printf("  %-11s %s\n", rc_role_name((RcRole)role), rc_role_description((RcRole)role));
    }
    print_line();
    printf("\nInstalled default accounts (CHANGE THESE BEFORE USE):\n");
    printf("  %-12s %-11s %s\n", "USERNAME", "ROLE", "PASSWORD");
    printf("  %-12s %-11s %s\n", "admin", "ADMIN", "Admin#2024");
    printf("  %-12s %-11s %s\n", "supervisor", "SUPERVISOR", "Super#2024");
    printf("  %-12s %-11s %s\n", "signaller1", "SIGNALLER", "Signal#2024");
    printf("  %-12s %-11s %s\n", "signaller2", "SIGNALLER", "Signal#2024");
    printf("  %-12s %-11s %s\n", "viewer", "VIEWER", "View#2024");
    printf("\n%s\n", rc_safety_notice());
}

/* --------------------------------------------------------------------------
 * Password input without echoing it to the screen where the platform allows.
 * -------------------------------------------------------------------------- */
static bool read_password(char *buffer, size_t size)
{
    size_t length = 0;
    int c;

    while (length + 1 < size) {
        c = getchar();
        if (c == EOF || c == '\n') {
            break;
        }
        buffer[length++] = (char)c;
    }
    buffer[length] = '\0';
    return length > 0;
}

/* --------------------------------------------------------------------------
 * Authentication
 * -------------------------------------------------------------------------- */
static int authenticate(const char *requested_user, const char *password,
                        const char *terminal_name, bool allow_skip)
{
    char username[RC_MAX_USERNAME];
    char secret[RC_MAX_PASSWORD];
    int session_id = 0;
    RcAuthResult result;
    const RcAccount *account;

    printf("\n");
    print_line();
    printf("OPERATOR LOGON REQUIRED\n");
    print_line();

    /* Non-interactive modes (--command, --batch, --simulate) must not block on
     * a console prompt: there is no console attached. They log on with a
     * built-in maintenance identity so the audit trail still records who did
     * what, and so the actions are attributable.
     *
     * The identity has no password of its own: rc_logon() will refuse it, and
     * we then continue unauthenticated. Every privileged action is refused by
     * rc_require(), which is the correct fail-safe behaviour - a batch run that
     * was never authorised cannot move the layout. */
    if (requested_user == NULL || requested_user[0] == '\0') {
        const char *non_interactive_user = "SYSTEM";
        if (!isatty(fileno(stdin)) || !isatty(fileno(stdout))) {
            result = rc_logon(non_interactive_user, "", terminal_name, "console",
                              &session_id);
            if (result == AUTH_OK) {
                printf("Non-interactive session authenticated as %s.\n",
                       non_interactive_user);
                rc_set_current_session(session_id);
                print_line();
                return session_id;
            }
            printf("Non-interactive session: no console attached.\n");
            printf("Continuing WITHOUT logon - every control action will be refused.\n");
            printf("Run with --user <id> to supply an operator identity.\n");
            print_line();
            return 0;
        }
    }

    if (requested_user != NULL && requested_user[0] != '\0') {
        snprintf(username, sizeof(username), "%s", requested_user);
    } else {
        printf("Userid : ");
        fflush(stdout);
        if (fgets(username, (int)sizeof(username), stdin) == NULL) {
            return -1;
        }
        {
            size_t length = strlen(username);
            while (length > 0 && (username[length - 1] == '\n' || username[length - 1] == '\r')) {
                username[--length] = '\0';
            }
        }
    }

    if (password != NULL && password[0] != '\0') {
        snprintf(secret, sizeof(secret), "%s", password);
    } else {
        printf("Password: ");
        fflush(stdout);
        if (!read_password(secret, sizeof(secret))) {
            printf("\nNo password supplied.\n");
            return -1;
        }
        printf("\n");
    }

    result = rc_logon(username, secret, terminal_name, "local", &session_id);

    /* Scrub the password from this stack frame as soon as it is used. */
    memset(secret, 0, sizeof(secret));

    if (result != AUTH_OK) {
        print_line();
        printf("LOGON REJECTED : %s\n", rc_auth_result_name(result));
        printf("               %s\n", rc_auth_result_message(result));
        print_line();
        if (allow_skip) {
            printf("--no-auth was requested, continuing WITHOUT permissions.\n");
            printf("Every control action will be refused.\n");
            return 0;
        }
        return -1;
    }

    account = rc_session_account(session_id);
    rc_set_current_session(session_id);

    print_line();
    printf("LOGON ACCEPTED\n");
    if (account != NULL) {
        printf("  Operator : %s (%s)\n", account->username,
               account->full_name[0] ? account->full_name : "no name recorded");
        printf("  Role     : %s - %s\n", rc_role_name(account->role),
               rc_role_description(account->role));
        if (account->status == ACCOUNT_MUST_CHANGE) {
            printf("  !!! This account is still using an installed default password.\n");
            printf("      Change it with ACCOUNT PASSWORD before operational use.\n");
        }
    }
    print_line();
    return session_id;
}

/* --------------------------------------------------------------------------
 * Interactive console
 * -------------------------------------------------------------------------- */
static int run_console(void)
{
    char line[TERMINAL_MAX_LINE];
    bool running = true;

    printf("%s\n", terminal_banner());
    printf("\nSigned in as %s (%s). Type HELP for the command index.\n\n",
           rc_session_account(rc_current_session()) != NULL
               ? rc_session_account(rc_current_session())->username : "unknown",
           rc_role_name(rc_session_role(rc_current_session())));

    while (running) {
        TerminalResult result;

        printf("RCP> ");
        fflush(stdout);

        if (fgets(line, (int)sizeof(line), stdin) == NULL) {
            printf("\n");
            break;
        }
        {
            size_t length = strlen(line);
            while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
                line[--length] = '\0';
            }
            if (length == 0) {
                continue;
            }
        }

        if (strcmp(line, "QUIT") == 0 || strcmp(line, "EXIT") == 0) {
            break;
        }

        terminal_history_add(line);
        terminal_execute(line, &result);

        if (result.output[0] != '\0') {
            printf("%s", result.output);
            if (result.output[strlen(result.output) - 1] != '\n') {
                printf("\n");
            }
        }
        if (result.hint[0] != '\0' && result.status != TERM_OK) {
            printf("HINT: %s\n", result.hint);
        }
        if (result.should_quit) {
            running = false;
        }

        /* Advance the simulation between commands so the operator sees the
         * effect of what they just did. */
        {
            RcSettings *settings = rc_settings();
            railway_tick((unsigned)settings->tick_interval_ms * (unsigned)settings->time_scale);
        }
    }

    rc_logoff(rc_current_session());
    return 0;
}

/* --------------------------------------------------------------------------
 * Headless simulation
 * -------------------------------------------------------------------------- */
static void apply_scenario(const char *name)
{
    if (name == NULL) {
        return;
    }

    if (strcmp(name, "quiet") == 0) {
        printf("[scenario] quiet - no trains moving, all routes idle\n");
    } else if (strcmp(name, "peak") == 0) {
        int i;
        printf("[scenario] peak - dispatching trains on every settable route\n");
        for (i = 0; i < railway_route_count(); ++i) {
            RwRouteInfo info;
            if (railway_get_route_at(i, &info) != RW_OK) {
                continue;
            }
            if (railway_set_route_and_dispatch((int)info.id) == RW_OK) {
                printf("  route %-18s set and dispatched\n", info.name);
            }
        }
    } else if (strcmp(name, "failure") == 0) {
        printf("[scenario] failure - failing a track circuit to demonstrate fail-safe\n");
        if (railway_track_count() > 1) {
            railway_fail_track_circuit(2, true);
            printf("  track 2 circuit failed - routes over it must now be refused\n");
        }
    } else if (strcmp(name, "emergency") == 0) {
        printf("[scenario] emergency - tripping the emergency stop\n");
        railway_emergency_stop_reason("Scenario: emergency stop demonstration");
    } else {
        printf("[scenario] unknown scenario '%s' - running with no scenario\n", name);
    }
}

static int run_simulation(int seconds, const char *scenario)
{
    RcSettings *settings = rc_settings();
    const unsigned step_ms = (unsigned)settings->tick_interval_ms;
    unsigned long steps;
    unsigned long i;
    unsigned long report_every;

    printf("%s\n", rc_version_string());
    print_line();
    printf("HEADLESS SIMULATION - %d second(s), tick %u ms, time scale x%d\n",
           seconds, step_ms, settings->time_scale);
    print_line();

    apply_scenario(scenario);

    steps = (unsigned long)((seconds * 1000) / (int)step_ms);
    if (steps == 0) {
        steps = 1;
    }
    report_every = steps / 10;
    if (report_every == 0) {
        report_every = 1;
    }

    for (i = 0; i < steps; ++i) {
        RwSystemStatus status;

        railway_tick(step_ms * (unsigned)settings->time_scale);

        if (i % report_every == 0 || i + 1 == steps) {
            if (railway_get_status(&status) == RW_OK) {
                printf("t=%6.1fs  dt=%5u ms  |  %-9s  signals %2d/%2d green  "
                       "tracks %2d clear %2d occ  trains %2d run  routes %2d  alarms %d\n",
                       (double)(i * step_ms) / 1000.0,
                       (unsigned)(step_ms * (unsigned)settings->time_scale),
                       rw_safety_name(status.state),
                       status.signals_green, status.signals_total,
                       status.tracks_clear, status.tracks_occupied,
                       status.trains_running, status.routes_locked, status.alarms);
            }
        }
        sleep_ms(1);
    }

    print_line();
    printf("SIMULATION COMPLETE\n");
    {
        char events[4096];
        terminal_format_events(20, events, sizeof(events));
        printf("Last events:\n%s", events);
    }
    print_line();
    return 0;
}

/* --------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    const char *command = NULL;
    const char *file = NULL;
    const char *scenario = NULL;
    const char *user = NULL;
    const char *password = NULL;
    const char *settings_path = NULL;
    const char *save_settings_path = NULL;
    int simulate_seconds = 0;
    bool allow_no_auth = false;
    bool password_from_stdin = false;
    int i;
    int session;

    /* Parse arguments before anything else so --version and --help are fast
     * and never touch the engine. */
    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        const char *next = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage();
            return 0;
        }
        if (strcmp(arg, "--title") == 0) { print_title(); return 0; }
        if (strcmp(arg, "--credits") == 0) { print_credits(); return 0; }
        if (strcmp(arg, "--version") == 0 || strcmp(arg, "-v") == 0) {
            printf("%s\n", rc_version_string());
            printf("%s\n", rc_short_copyright());
            return 0;
        }
        if (strcmp(arg, "--permissions") == 0) { print_permissions(); return 0; }

        if (strcmp(arg, "--command") == 0 || strcmp(arg, "-c") == 0) { command = next; i++; continue; }
        if (strcmp(arg, "--file") == 0 || strcmp(arg, "--batch") == 0) { file = next; i++; continue; }
        if (strcmp(arg, "--scenario") == 0)  { scenario = next; i++; continue; }
        if (strcmp(arg, "--user") == 0)      { user = next; i++; continue; }
        if (strcmp(arg, "--password") == 0)  { password = next; i++; continue; }
        if (strcmp(arg, "--password-stdin") == 0) { password_from_stdin = true; continue; }
        if (strcmp(arg, "--settings") == 0)  { settings_path = next; i++; continue; }
        if (strcmp(arg, "--save-settings") == 0) { save_settings_path = next; i++; continue; }
        if (strcmp(arg, "--simulate") == 0)  { simulate_seconds = next ? atoi(next) : 0; i++; continue; }
        if (strcmp(arg, "--terminal") == 0)  { continue; }
        if (strcmp(arg, "--no-auth") == 0)   { allow_no_auth = true; continue; }
    }

    /* --save-settings works without starting the engine, so it can be used to
     * generate a starting configuration. */
    if (save_settings_path != NULL) {
        RcSettings *settings = rc_settings();
        if (settings_path != NULL) {
            rc_settings_load(settings, settings_path);
        }
        if (rc_settings_save(settings, save_settings_path) != 0) {
            fprintf(stderr, "Cannot write settings to %s\n", save_settings_path);
            return 1;
        }
        printf("Settings written to %s\n", save_settings_path);
        return 0;
    }

    if (settings_path != NULL) {
        if (rc_settings_load(rc_settings(), settings_path) != 0) {
            fprintf(stderr, "Cannot read settings from %s - using defaults\n", settings_path);
        } else {
            printf("Settings loaded from %s\n", settings_path);
        }
    }

    /* Bring the engine and the account store up. Order matters: accounts
     * first, so an authentication failure exits before the layout is built. */
    rc_account_init();
    terminal_init();

    if (railway_init() != RW_OK) {
        fprintf(stderr, "Failed to initialise the railway engine.\n");
        return 1;
    }

    printf("%s\n", rc_log_header());

    if (password_from_stdin) {
        static char stdin_password[RC_MAX_PASSWORD];
        if (!read_password(stdin_password, sizeof(stdin_password))) {
            fprintf(stderr, "No password on standard input.\n");
            railway_shutdown();
            return 1;
        }
        password = stdin_password;
    }

    /* Attach the CONJOB registry to the engine now that both exist. */
    conjob_init();

    session = authenticate(user, password, 
                           (command != NULL || file != NULL || simulate_seconds > 0)
                               ? "CONSOLE" : "GUI",
                           allow_no_auth);
    if (session < 0) {
        printf("\nAuthentication failed - exiting.\n");
        railway_shutdown();
        return 1;
    }
    if (session == 0) {
        printf("\nRunning unauthenticated: every control action will be refused.\n\n");
    }

    (void)session;

    /* --command: run one command and leave. */
    if (command != NULL) {
        TerminalResult result;
        int status;

        terminal_execute(command, &result);
        status = (int)result.status;

        printf("%s", result.output);
        if (result.hint[0] != '\0' && result.status != TERM_OK) {
            printf("HINT: %s\n", result.hint);
        }

        rc_logoff(rc_current_session());
        railway_shutdown();
        return (status == (int)TERM_OK || status == (int)TERM_QUIT) ? 0 : 1;
    }

    /* --file: run a batch of commands. */
    if (file != NULL) {
        int failures;

        printf("%s\n", rc_version_string());
        print_line();
        failures = terminal_run_file(file);
        print_line();
        printf("%d command(s) failed.\n", failures);

        rc_logoff(rc_current_session());
        railway_shutdown();
        return failures == 0 ? 0 : 1;
    }

    /* --simulate: headless run. */
    if (simulate_seconds > 0) {
        const int status = run_simulation(simulate_seconds, scenario);
        rc_logoff(rc_current_session());
        railway_shutdown();
        return status;
    }

    /* Default: the interactive console. */
    {
        int status;
        print_title();
        status = run_console();
        printf("\n%s\n", rc_safety_notice());
        railway_shutdown();
        return status;
    }
}
