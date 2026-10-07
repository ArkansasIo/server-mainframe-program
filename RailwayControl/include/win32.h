/* ==========================================================================
 * win32.h - 🖥️ Win32 GUI dispatch console.
 *
 * The windowed front end. It is a thin view over railway_api.h: the window
 * owns no railway state of its own, it reads the engine on a timer and sends
 * commands back through the same public API the terminal uses. A GUI that
 * kept its own copy of the layout would be a GUI that could disagree with the
 * interlocking, which is the one thing a signalling display must never do.
 *
 * Window layout
 * -------------
 *   +------------------------------------------------------------------------+
 *   | menu bar    System  View  Control  Tools  Help                          |
 *   +------------------------------------------------------------------------+
 *   | header      system name, state, clock, alarm count                      |
 *   +------------------------------------------------------------------------+
 *   |                                                                        |
 *   |   TRACK DIAGRAM  (GDI)                                                 |
 *   |   tracks, signals, points, trains, drawn from the engine's own         |
 *   |   normalised coordinates so the picture cannot drift from the logic    |
 *   |                                                                        |
 *   +------------------------------------------------------------------------+
 *   | event log                  | terminal pane                             |
 *   +------------------------------------------------------------------------+
 *   | status bar   operator, role, uptime, tick count, last error            |
 *   +------------------------------------------------------------------------+
 *
 * Threading
 * ---------
 * There is exactly one UI thread. The engine is driven by a WM_TIMER tick on
 * that same thread, so no locking is needed beyond what railway_api.h already
 * takes. That is deliberate: a signalling display that updates from a worker
 * thread is a display that can paint a half-applied interlocking state.
 *
 * Safety notice
 * -------------
 * A cosmetic front end over a simulator. Every control action it issues goes
 * through the interlocking and can be refused, and the refusal is shown to
 * the operator in the status bar - the UI never assumes a command worked.
 * ========================================================================== */
#ifndef RC_WIN32_H
#define RC_WIN32_H

#if !defined(_WIN32)
/* The GUI is Win32 only. Including this header elsewhere is a build mistake,
 * so fail loudly rather than silently compiling a stub. */
#error "win32.h is only available on Windows builds"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#include "railway_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* --------------------------------------------------------------------------
 * Identity
 * -------------------------------------------------------------------------- */
#define RCWIN_CLASS_NAME L"RailControlDispatchConsole"
#define RCWIN_WINDOW_TITLE L"RailControl - Dispatch Console"

/* Default window metrics, in pixels. */
#define RCWIN_DEFAULT_WIDTH 1280
#define RCWIN_DEFAULT_HEIGHT 860
#define RCWIN_MIN_WIDTH 900
#define RCWIN_MIN_HEIGHT 640
#define RCWIN_HEADER_HEIGHT 30
#define RCWIN_STATUS_HEIGHT 24
#define RCWIN_BOTTOM_HEIGHT 190 /* event log + terminal panes */

/* The engine tick. 100 ms matches RcSettings.tick_interval_ms. */
#define RCWIN_TICK_TIMER_ID 1
#define RCWIN_TICK_INTERVAL_MS 100

#define RCWIN_MAX_LINE          512
#define RCWIN_MAX_EVENTS         64

/* Number of scrollback lines in the embedded terminal pane. */
#define TERMINAL_PANE_LINES     200

    /* --------------------------------------------------------------------------
     * Command identifiers - menus, buttons and accelerators all share them.
     * -------------------------------------------------------------------------- */
    enum RcWinCommand
    {
        RCWIN_CMD_NONE = 0,

        /* File */
        RCWIN_CMD_SAVE_STATE = 100,
        RCWIN_CMD_LOAD_STATE,
        RCWIN_CMD_SAVE_SETTINGS,
        RCWIN_CMD_LOAD_SETTINGS,
        RCWIN_CMD_EXPORT_PNG,
        RCWIN_CMD_EXIT,

        /* View */
        RCWIN_CMD_ZOOM_IN = 200,
        RCWIN_CMD_ZOOM_OUT,
        RCWIN_CMD_ZOOM_RESET,
        RCWIN_CMD_TOGGLE_GRID,
        RCWIN_CMD_TOGGLE_LABELS,
        RCWIN_CMD_TOGGLE_SENSORS,
        RCWIN_CMD_SHOW_EVENTS,
        RCWIN_CMD_SHOW_TERMINAL,

        /* Control */
        RCWIN_CMD_EMERGENCY_STOP = 300,
        RCWIN_CMD_CLEAR_EMERGENCY,
        RCWIN_CMD_ACK_ALL,
        RCWIN_CMD_EARLY_RELEASE,
        RCWIN_CMD_HOLD_ALL_TRAINS,
        RCWIN_CMD_RELEASE_ALL_TRAINS,
        RCWIN_CMD_ACTIVATE_CONJOBS,
        RCWIN_CMD_SUSPEND_CONJOBS,

        /* Tools */
        RCWIN_CMD_PERMISSIONS = 400,
        RCWIN_CMD_ACCOUNTS,
        RCWIN_CMD_JOBS,
        RCWIN_CMD_SETTINGS,
        RCWIN_CMD_SCRIPT_CONSOLE,
        RCWIN_CMD_AUDIO_TEST,

        /* Help */
        RCWIN_CMD_HELP = 500,
        RCWIN_CMD_ABOUT,
        RCWIN_CMD_CREDITS,
        RCWIN_CMD_SAFETY_NOTICE,
        RCWIN_CMD_SHORTCUTS
    };

    /* --------------------------------------------------------------------------
     * The application state. One instance, owned by win32_main.c.
     * -------------------------------------------------------------------------- */
    typedef struct RcWinApp
    {
        HINSTANCE instance;
        HWND window;
        HMENU menu;
        HFONT font; /* fixed-width font for the panes */
        HFONT font_bold;
        HBRUSH back_brush; /* panel background */
        HBRUSH panel_brush;
        HPEN rail_pen;      /* plain track */
        HPEN rail_occupied; /* track with a train on it */
        HPEN rail_failed;   /* failed circuit */
        HDC buffer_dc;      /* double-buffer to stop the diagram flickering */
        HBITMAP buffer_bitmap;
        int buffer_width;
        int buffer_height;

        /* Cached system state, refreshed on the tick and never written back. */
        RwSystemStatus status;
        unsigned long last_tick_ms;

        /* Terminal pane */
        char terminal_input[RCWIN_MAX_LINE];
        int terminal_cursor;
        char terminal_scrollback[TERMINAL_PANE_LINES][RCWIN_MAX_LINE];
        int terminal_line_count;

        /* Layout of the child panes, recomputed on WM_SIZE. */
        RECT diagram_rect;
        RECT event_rect;
        RECT terminal_rect;

        int zoom_percent;
        bool show_grid;
        bool show_labels;
        bool show_sensors;
        bool paused; /* stop the engine clock while inspecting */

            bool      running;       /* set false to leave the message loop */
        } RcWinApp;

        /* The single application instance. */
        RcWinApp *rcwin_app(void);

    /* --------------------------------------------------------------------------
     * win32_main.c - lifecycle and message handling
     * -------------------------------------------------------------------------- */
    int rcwin_run(HINSTANCE instance, int show_command);
    void rcwin_request_repaint(void);
    void rcwin_set_status_text(const char *text);
    void rcwin_post_message(const char *text);
    void rcwin_refresh_state(void);

    /* --------------------------------------------------------------------------
     * win32_panel.c - the GDI track diagram  (🛤️)
     * -------------------------------------------------------------------------- */

    /* Draw the whole diagram into `dc`, scaled into `area`. */
    void rcwin_draw_diagram(HDC dc, const RECT *area);

    /* Draw one track as a double line, coloured by its occupancy. */
    void rcwin_draw_track(HDC dc, const RECT *area, const RwTrackInfo *track);

    /* Draw a signal head at its layout position, showing its aspect. */
    void rcwin_draw_signal(HDC dc, const RECT *area, const RwSignalInfo *signal);

    /* Draw a point, showing its lie and whether it is locked. */
    void rcwin_draw_point(HDC dc, const RECT *area, const RwSwitchInfo *point);

    /* Draw a train at its position along its track, with its headcode. */
    void rcwin_draw_train(HDC dc, const RECT *area, const RwTrainInfo *train);

    /* --------------------------------------------------------------------------
     * win32_status.c - the header, event list and status bar  (📝)
     * -------------------------------------------------------------------------- */
    void rcwin_draw_header(HDC dc, const RECT *area);
    void rcwin_draw_status_bar(HDC dc, const RECT *area);
    void rcwin_draw_event_log(HDC dc, const RECT *area);

    /* --------------------------------------------------------------------------
     * win32_terminal.c - the embedded terminal pane  (💻)
     * -------------------------------------------------------------------------- */
    void rcwin_terminal_init(void);
    void rcwin_terminal_append(const char *text);
    void rcwin_terminal_execute(void);   /* run terminal_input */
    void rcwin_terminal_key(WPARAM key); /* editing keys */
    void rcwin_draw_terminal(HDC dc, const RECT *area);
    void rcwin_terminal_focus(void);

    /* --------------------------------------------------------------------------
     * win32_actions.c - menu command handling  (🎛️)
     * -------------------------------------------------------------------------- */

    /* Run one command id. Returns true when the command was handled. Every
     * engine-calling command goes through railway_api.h and reports a refusal
     * rather than assuming success. */
    bool rcwin_handle_command(int command_id);

    /* Reset every zoom/display toggle to its default. */
    void rcwin_reset_view(void);

    /* Refresh the cached status and ask for a repaint. */
    void rcwin_on_tick(void);

    /* A modal yes/no question, used before an irreversible action. */
    bool rcwin_confirm(const wchar_t *question);

#ifdef __cplusplus
}
#endif

#endif /* RC_WIN32_H */
