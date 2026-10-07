/* ==========================================================================
 * win32_entry.c - 🖥️ WinMain entry point for the GUI executable.
 *
 * This is the entry point for rcp-gui.exe. It is deliberately separate from
 * main.c's main(), because the console build and the windowed build want
 * different subsystem settings and different first actions:
 *
 *   main.c      parses arguments, authenticates, then picks a mode. It is
 *               built as a console application (it prints to stdout).
 *   win32_entry parses the same arguments, brings the engine up, and hands
 *               control to the window. Built as a WINDOWS subsystem binary,
 *               so launching it from Explorer does not flash a console.
 *
 * Both share every line of engine code through railway_api.h. The GUI is a
 * front end, not a second implementation.
 *
 * Command line (mirrors main.c so the two are interchangeable for scripting):
 *   rcp-gui.exe                          open the console at the default size
 *   rcp-gui.exe --maximized              open maximised
 *   rcp-gui.exe --settings <path>        load settings before starting
 *   rcp-gui.exe --user <id> --password <pw>
 *                                     log on, so control actions are permitted
 *   rcp-gui.exe --no-auth              start unattended (SIMULATION ONLY)
 *
 * Authentication is not optional in spirit: if no operator is signed on, the
 * window still opens and shows the live railway, but every control action is
 * refused by rc_require(), exactly as it would be in the console. A GUI that
 * could work the layout without a logon would be a hole in the audit trail.
 * ========================================================================== */
#include "win32.h"

#include "railway_api.h"
#include "railcontrol.h"
#include "rc_version.h"
#include "rc_account.h"
#include "terminal.h"
#include "conjob.h"
#include "rc_audio.h"

/* CommandLineToArgvW lives in shellapi.h. */
#include <shellapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Argument parsing
 *
 * Kept in step with main.c's parser. The GUI accepts a subset (there is no
 * --command or --simulate here, those belong to the console build) but the
 * options it does accept mean exactly the same thing.
 * -------------------------------------------------------------------------- */
typedef struct
{
    const char *user;
    const char *password;
    const char *settings_path;
    bool maximized;
    bool allow_no_auth;
} GuiOptions;

static void parse_arguments(GuiOptions *options, const char *command_line)
{
    /* CommandLineToArgvW is the only correct way to split a Windows command
     * line; strtok would break on a quoted password containing a space. */
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int i;

    memset(options, 0, sizeof(*options));

    if (argv == NULL) {
        return;
    }

    for (i = 1; i < argc; ++i) {
        char arg[512];

        WideCharToMultiByte(CP_ACP, 0, argv[i], -1, arg, sizeof(arg), NULL, NULL);

        if (strcmp(arg, "--maximized") == 0 || strcmp(arg, "--max") == 0) {
            options->maximized = true;
        } else if (strcmp(arg, "--no-auth") == 0) {
            options->allow_no_auth = true;
        } else if (i + 1 < argc) {
            /* The option takes a value. Copy it into the caller's arena rather
             * than pointing at this loop's buffer, which the next iteration
             * would overwrite. */
            static char value_store[8][512];
            static int value_slot = 0;
            char *value = value_store[value_slot];

            value_slot = (value_slot + 1) % 8;
            WideCharToMultiByte(CP_ACP, 0, argv[i + 1], -1, value, 512, NULL, NULL);

            if (strcmp(arg, "--user") == 0) {
                options->user = value;
                i++;
            } else if (strcmp(arg, "--password") == 0) {
                options->password = value;
                i++;
            } else if (strcmp(arg, "--settings") == 0) {
                options->settings_path = value;
                i++;
            }
        }
        /* Unknown arguments are ignored rather than fatal: a shortcut with a
         * stale option should still open the window. */
    }

    LocalFree(argv);
    (void)command_line;
}

/* --------------------------------------------------------------------------
 * Bringing the subsystems up
 *
 * Order matters and is the same as main.c:
 *   1. settings  - so the engine reads the operator's tick rate
 *   2. accounts  - so a logon failure can stop us before the layout is built
 *   3. engine    - builds the layout and the fleet
 *   4. terminal  - registers the command table the pane will use
 *   5. automation - CONJOB needs the engine to exist first
 * -------------------------------------------------------------------------- */
static bool bring_up_engine(const GuiOptions *options)
{
    RcSettings *settings = rc_settings();

    if (options->settings_path != NULL)
    {
        if (rc_settings_load(settings, options->settings_path) != 0)
        {
            MessageBoxW(NULL, L"Could not read the settings file - using defaults.",
                        RCWIN_WINDOW_TITLE, MB_ICONWARNING | MB_OK);
        }
    }

    rc_account_init();
    terminal_init();
    rc_audio_defaults(rc_audio_settings());

    if (railway_init() != RW_OK)
    {
        MessageBoxW(NULL, L"Failed to initialise the railway engine.",
                    RCWIN_WINDOW_TITLE, MB_ICONERROR | MB_OK);
        return false;
    }

    conjob_init();
    return true;
}

/**
 * Log on if credentials were supplied.
 *
 * The window opens either way. Without a logon the operator can watch the
 * railway but cannot work it - which is the correct behaviour for a control
 * system, and is the same rule the console applies.
 */
static void sign_on(const GuiOptions *options)
{
    int session_id = 0;

    if (options->user == NULL || options->user[0] == '\0')
    {
        return;
    }

    if (rc_logon(options->user, options->password != NULL ? options->password : "",
                 "GUI", "local", &session_id) == AUTH_OK)
    {
        rc_set_current_session(session_id);
    }
    else
    {
        MessageBoxW(NULL,
                    L"Logon was rejected. The window will open read-only; "
                    L"every control action will be refused.",
                    RCWIN_WINDOW_TITLE, MB_ICONWARNING | MB_OK);
    }
}

/* --------------------------------------------------------------------------
 * WinMain
 * -------------------------------------------------------------------------- */
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, LPWSTR command_line,
                    int show_command)
{
    GuiOptions options;
    int result;

    (void)previous;
    (void)command_line;

    /* A single instance only. Two consoles against one engine would each
     * believe they were the truth. */
    {
        HANDLE mutex = CreateMutexW(NULL, TRUE, L"RailControl.DispatchConsole.SingleInstance");
        if (mutex != NULL && GetLastError() == ERROR_ALREADY_EXISTS)
        {
            MessageBoxW(NULL, L"RailControl is already running.",
                        RCWIN_WINDOW_TITLE, MB_ICONINFORMATION | MB_OK);
            return 0;
        }
    }

    parse_arguments(&options, NULL);

    if (!bring_up_engine(&options))
    {
        railway_shutdown();
        return 1;
    }

    sign_on(&options);

    /* Show the window maximised when asked, otherwise at the default size
     * passed to ShowWindow. */
    result = rcwin_run(instance, options.maximized ? SW_SHOWMAXIMIZED : show_command);

    if (rc_current_session() > 0)
    {
        (void)rc_logoff(rc_current_session());
    }
    conjob_shutdown();
    rc_account_shutdown();
    railway_shutdown();

    return result;
}
