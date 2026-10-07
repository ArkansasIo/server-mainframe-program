/* ==========================================================================
 * win32_main.c - 🖥️ Win32 window lifecycle, menus and the message loop.
 *
 * This file owns the window and the timer. It deliberately does no drawing of
 * railway geometry itself: WM_PAINT sets up a double-buffered DC and hands it
 * to the panel and status modules, so the window never talks to the engine's
 * internals and the drawing code stays in one place per panel.
 *
 * Why double buffering matters here
 * ---------------------------------
 * The diagram repaints ten times a second. Painting straight to the window DC
 * makes a 96-signal layout flicker visibly, and a flickering signalling
 * display is worse than useless - it hides changes. Every frame is therefore
 * composed into an off-screen bitmap and blitted in one operation.
 * ========================================================================== */
#include "win32.h"

#include "railway_api.h"
#include "railcontrol.h"
#include "rc_version.h"
#include "rc_account.h"
#include "terminal.h"
#include "conjob.h"

#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * The single application instance
 * -------------------------------------------------------------------------- */
static RcWinApp g_app;

RcWinApp *rcwin_app(void)
{
    return &g_app;
}

/* --------------------------------------------------------------------------
 * Status line and operator feedback
 *
 * Every message an operator sees about a command goes through here, including
 * refusals. A UI that silently swallowed a refused command would be reporting
 * success for something the interlocking blocked.
 * -------------------------------------------------------------------------- */
static wchar_t g_status_text[256];

void rcwin_set_status_text(const char *text)
{
    if (text == NULL)
    {
        g_status_text[0] = L'\0';
    }
    else
    {
        MultiByteToWideChar(CP_ACP, 0, text, -1, g_status_text,
                            (int)(sizeof(g_status_text) / sizeof(g_status_text[0])));
    }
    if (g_app.window != NULL)
    {
        InvalidateRect(g_app.window, NULL, FALSE);
    }
}

/** Write a line to the terminal pane as operator feedback. */
void rcwin_post_message(const char *text)
{
    if (text == NULL)
    {
        return;
    }
    rcwin_terminal_append(text);
}

void rcwin_request_repaint(void)
{
    if (g_app.window != NULL)
    {
        InvalidateRect(g_app.window, NULL, FALSE);
    }
}

/* --------------------------------------------------------------------------
 * Fonts and brushes
 * -------------------------------------------------------------------------- */
static HFONT create_mono_font(int points, bool bold)
{
    LOGFONTW lf;
    HDC screen = GetDC(NULL);
    int height;

    memset(&lf, 0, sizeof(lf));
    height = -MulDiv(points, GetDeviceCaps(screen, LOGPIXELSY), 72);
    ReleaseDC(NULL, screen);

    lf.lfHeight = height;
    lf.lfWeight = bold ? FW_BOLD : FW_NORMAL;
    lf.lfCharSet = ANSI_CHARSET;
    /* Consolas is the closest thing Windows has to a panel font; the diagram
     * is built from box characters, so a fixed pitch matters. */
    wcscpy_s(lf.lfFaceName, LF_FACESIZE, L"Consolas");
    return CreateFontIndirectW(&lf);
}

static void create_resources(void)
{
    g_app.font = create_mono_font(9, false);
    g_app.font_bold = create_mono_font(10, true);

    g_app.back_brush = CreateSolidBrush(RGB(12, 16, 22));
    g_app.panel_brush = CreateSolidBrush(RGB(20, 27, 36));
    g_app.rail_pen = CreatePen(PS_SOLID, 2, RGB(120, 140, 165));
    g_app.rail_occupied = CreatePen(PS_SOLID, 3, RGB(240, 190, 70));
    g_app.rail_failed = CreatePen(PS_SOLID, 3, RGB(220, 60, 60));
}

static void destroy_resources(void)
{
    if (g_app.buffer_dc != NULL)
    {
        SelectObject(g_app.buffer_dc, GetStockObject(NULL_BRUSH));
    }
    if (g_app.buffer_bitmap != NULL)
        DeleteObject(g_app.buffer_bitmap);
    if (g_app.buffer_dc != NULL)
        DeleteDC(g_app.buffer_dc);
    if (g_app.font != NULL)
        DeleteObject(g_app.font);
    if (g_app.font_bold != NULL)
        DeleteObject(g_app.font_bold);
    if (g_app.back_brush != NULL)
        DeleteObject(g_app.back_brush);
    if (g_app.panel_brush != NULL)
        DeleteObject(g_app.panel_brush);
    if (g_app.rail_pen != NULL)
        DeleteObject(g_app.rail_pen);
    if (g_app.rail_occupied != NULL)
        DeleteObject(g_app.rail_occupied);
    if (g_app.rail_failed != NULL)
        DeleteObject(g_app.rail_failed);
}

/* --------------------------------------------------------------------------
 * Double buffer
 *
 * Sized to the client area. Resizing recreates it once per size change rather
 * than per frame, which is what keeps the repaint cheap.
 * -------------------------------------------------------------------------- */
static void ensure_buffer(HDC reference_dc, int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return;
    }
    if (g_app.buffer_dc != NULL && g_app.buffer_width == width && g_app.buffer_height == height)
    {
        return;
    }

    if (g_app.buffer_bitmap != NULL)
    {
        DeleteObject(g_app.buffer_bitmap);
        g_app.buffer_bitmap = NULL;
    }
    if (g_app.buffer_dc == NULL)
    {
        g_app.buffer_dc = CreateCompatibleDC(reference_dc);
    }

    g_app.buffer_bitmap = CreateCompatibleBitmap(reference_dc, width, height);
    SelectObject(g_app.buffer_dc, g_app.buffer_bitmap);
    g_app.buffer_width = width;
    g_app.buffer_height = height;
}

/* --------------------------------------------------------------------------
 * Window layout
 *
 * One place decides where each pane lives, so the drawing code and the hit
 * testing for the terminal pane cannot disagree about where the pane is.
 * -------------------------------------------------------------------------- */
static void layout_panes(HWND window)
{
    RECT client;
    int width;
    int height;
    int header;

    GetClientRect(window, &client);
    width = client.right - client.left;
    height = client.bottom - client.top;
    header = RCWIN_HEADER_HEIGHT;

    g_app.diagram_rect.left = 0;
    g_app.diagram_rect.top = header;
    g_app.diagram_rect.right = width;
    g_app.diagram_rect.bottom = height - RCWIN_BOTTOM_HEIGHT - RCWIN_STATUS_HEIGHT;

    /* Event log on the left half, terminal on the right. */
    g_app.event_rect.left = 0;
    g_app.event_rect.top = g_app.diagram_rect.bottom;
    g_app.event_rect.right = width / 2;
    g_app.event_rect.bottom = height - RCWIN_STATUS_HEIGHT;

    g_app.terminal_rect.left = width / 2;
    g_app.terminal_rect.top = g_app.diagram_rect.bottom;
    g_app.terminal_rect.right = width;
    g_app.terminal_rect.bottom = height - RCWIN_STATUS_HEIGHT;

    /* Guard against a window clipped so small the panes invert. */
    if (g_app.diagram_rect.bottom < g_app.diagram_rect.top)
    {
        g_app.diagram_rect.bottom = g_app.diagram_rect.top + 1;
    }
    if (g_app.event_rect.bottom < g_app.event_rect.top)
    {
        g_app.event_rect.bottom = g_app.event_rect.top + 1;
    }
}

/* --------------------------------------------------------------------------
 * Painting
 * -------------------------------------------------------------------------- */
static void paint_window(HWND window)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(window, &ps);
    RECT client;
    RECT status_rect;

    GetClientRect(window, &client);
    ensure_buffer(dc, client.right - client.left, client.bottom - client.top);

    if (g_app.buffer_dc != NULL)
    {
        HDC target = g_app.buffer_dc;

        FillRect(target, &client, g_app.back_brush);

        SelectObject(target, g_app.font);

        /* Diagram. */
        SetBkMode(target, TRANSPARENT);
        rcwin_draw_diagram(target, &g_app.diagram_rect);

        /* Event log and terminal panes. */
        rcwin_draw_event_log(target, &g_app.event_rect);
        rcwin_draw_terminal(target, &g_app.terminal_rect);

        /* Header and status bar last: they overpaint the pane edges. */
        {
            RECT header = client;
            header.bottom = g_app.diagram_rect.top;
            rcwin_draw_header(target, &header);
        }
        status_rect = client;
        status_rect.top = g_app.event_rect.bottom;
        rcwin_draw_status_bar(target, &status_rect);

        BitBlt(dc, 0, 0, client.right - client.left, client.bottom - client.top,
               target, 0, 0, SRCCOPY);
    }

    EndPaint(window, &ps);
}

/* --------------------------------------------------------------------------
 * The engine tick
 * -------------------------------------------------------------------------- */
void rcwin_refresh_state(void)
{
    if (!railway_is_initialized())
    {
        return;
    }
    (void)railway_get_status(&g_app.status);
}

void rcwin_on_tick(void)
{
    RcSettings *settings = rc_settings();

    if (!g_app.paused && railway_is_initialized())
    {
        const unsigned step = (unsigned)settings->tick_interval_ms * (unsigned)(settings->time_scale > 0 ? settings->time_scale : 1);
        (void)railway_tick(step);
    }

    rcwin_refresh_state();
    rcwin_request_repaint();
}

/* --------------------------------------------------------------------------
 * Keyboard: the terminal pane takes typing, everything else is a command
 * -------------------------------------------------------------------------- */
static bool terminal_has_focus(void)
{
    /* The terminal pane is the only text input, so it holds focus whenever the
     * window does and the diagram is not being panned. */
    return true;
}

static void on_key_down(HWND window, WPARAM key)
{
    if (key == VK_ESCAPE)
    {
        /* Escape always clears the terminal input - the signaller's "start
         * again" key, matching the real desks. */
        g_app.terminal_input[0] = '\0';
        g_app.terminal_cursor = 0;
        rcwin_request_repaint();
        return;
    }
    if (key == VK_RETURN)
    {
        rcwin_terminal_execute();
        return;
    }
    if (key == VK_BACK)
    {
        rcwin_terminal_key(key);
        return;
    }
    if (key == VK_F1)
    {
        (void)rcwin_handle_command(RCWIN_CMD_HELP);
        return;
    }
    if (key == VK_F2)
    {
        (void)rcwin_handle_command(RCWIN_CMD_SHORTCUTS);
        return;
    }
    if (key == VK_F5)
    {
        g_app.paused = !g_app.paused;
        rcwin_set_status_text(g_app.paused ? "Engine clock PAUSED" : "Engine clock running");
        return;
    }
    rcwin_terminal_key(key);
    (void)window;
}

static void on_char(HWND window, WPARAM ch)
{
    if (ch < 32 || ch > 126)
    {
        return;
    }
    if (!terminal_has_focus())
    {
        return;
    }
    if (g_app.terminal_cursor < RCWIN_MAX_LINE - 1)
    {
        g_app.terminal_input[g_app.terminal_cursor++] = (char)ch;
        g_app.terminal_input[g_app.terminal_cursor] = '\0';
        rcwin_request_repaint();
    }
    (void)window;
}

/* --------------------------------------------------------------------------
 * Window procedure
 * -------------------------------------------------------------------------- */
static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case WM_CREATE:
        g_app.window = window;
        SetTimer(window, RCWIN_TICK_TIMER_ID, RCWIN_TICK_INTERVAL_MS, NULL);
        return 0;

    case WM_SIZE:
        layout_panes(window);
        {
            HDC dc = GetDC(window);
            ensure_buffer(dc, LOWORD(lparam), HIWORD(lparam));
            ReleaseDC(window, dc);
        }
        rcwin_request_repaint();
        return 0;

    case WM_TIMER:
        if (wparam == RCWIN_TICK_TIMER_ID)
        {
            rcwin_on_tick();
        }
        return 0;

    case WM_PAINT:
        paint_window(window);
        return 0;

    case WM_ERASEBKGND:
        /* The double-buffered paint covers every pixel, so erasing here would
         * only cause the flicker the buffer exists to prevent. */
        return 1;

    case WM_KEYDOWN:
        on_key_down(window, wparam);
        return 0;

    case WM_CHAR:
        on_char(window, wparam);
        return 0;

    case WM_COMMAND:
        if (rcwin_handle_command(LOWORD(wparam)))
        {
            return 0;
        }
        break;

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO *info = (MINMAXINFO *)lparam;
        info->ptMinTrackSize.x = RCWIN_MIN_WIDTH;
        info->ptMinTrackSize.y = RCWIN_MIN_HEIGHT;
    }
        return 0;

    case WM_CLOSE:
        if (rcwin_confirm(L"Close the dispatch console?"))
        {
            g_app.running = false;
            DestroyWindow(window);
        }
        return 0;

    case WM_DESTROY:
        KillTimer(window, RCWIN_TICK_TIMER_ID);
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

/* --------------------------------------------------------------------------
 * Menus
 * -------------------------------------------------------------------------- */
static HMENU build_menu(void)
{
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    HMENU view = CreatePopupMenu();
    HMENU control = CreatePopupMenu();
    HMENU tools = CreatePopupMenu();
    HMENU help = CreatePopupMenu();

    AppendMenuW(file, MF_STRING, RCWIN_CMD_SAVE_STATE, L"&Save state...\tCtrl+S");
    AppendMenuW(file, MF_STRING, RCWIN_CMD_LOAD_STATE, L"&Load state...\tCtrl+O");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, RCWIN_CMD_SAVE_SETTINGS, L"Save &settings...");
    AppendMenuW(file, MF_STRING, RCWIN_CMD_LOAD_SETTINGS, L"Load se&ttings...");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, RCWIN_CMD_EXIT, L"E&xit\tAlt+F4");

    AppendMenuW(view, MF_STRING, RCWIN_CMD_ZOOM_IN, L"Zoom &in\tCtrl++");
    AppendMenuW(view, MF_STRING, RCWIN_CMD_ZOOM_OUT, L"Zoom &out\tCtrl+-");
    AppendMenuW(view, MF_STRING, RCWIN_CMD_ZOOM_RESET, L"Zoom &reset\tCtrl+0");
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view, MF_STRING | MF_CHECKED, RCWIN_CMD_TOGGLE_GRID, L"Show &grid");
    AppendMenuW(view, MF_STRING | MF_CHECKED, RCWIN_CMD_TOGGLE_LABELS, L"Show train &labels");
    AppendMenuW(view, MF_STRING, RCWIN_CMD_TOGGLE_SENSORS, L"Show &sensor ids");
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view, MF_STRING, RCWIN_CMD_SHOW_EVENTS, L"Show &events");
    AppendMenuW(view, MF_STRING, RCWIN_CMD_SHOW_TERMINAL, L"Show &terminal");

    AppendMenuW(control, MF_STRING, RCWIN_CMD_EMERGENCY_STOP, L"&Emergency stop\tF12");
    AppendMenuW(control, MF_STRING, RCWIN_CMD_CLEAR_EMERGENCY, L"&Clear emergency");
    AppendMenuW(control, MF_SEPARATOR, 0, NULL);
    AppendMenuW(control, MF_STRING, RCWIN_CMD_ACK_ALL, L"&Acknowledge all alarms");
    AppendMenuW(control, MF_STRING, RCWIN_CMD_EARLY_RELEASE, L"&Early route release");
    AppendMenuW(control, MF_SEPARATOR, 0, NULL);
    AppendMenuW(control, MF_STRING, RCWIN_CMD_HOLD_ALL_TRAINS, L"&Hold all trains");
    AppendMenuW(control, MF_STRING, RCWIN_CMD_RELEASE_ALL_TRAINS, L"&Release all trains");
    AppendMenuW(control, MF_SEPARATOR, 0, NULL);
    AppendMenuW(control, MF_STRING, RCWIN_CMD_ACTIVATE_CONJOBS, L"Activate &CONJOBs");
    AppendMenuW(control, MF_STRING, RCWIN_CMD_SUSPEND_CONJOBS, L"&Suspend CONJOBs");

    AppendMenuW(tools, MF_STRING, RCWIN_CMD_PERMISSIONS, L"&Permissions...");
    AppendMenuW(tools, MF_STRING, RCWIN_CMD_ACCOUNTS, L"&Accounts...");
    AppendMenuW(tools, MF_STRING, RCWIN_CMD_JOBS, L"Automation &jobs...");
    AppendMenuW(tools, MF_STRING, RCWIN_CMD_SETTINGS, L"&Settings...");
    AppendMenuW(tools, MF_SEPARATOR, 0, NULL);
    AppendMenuW(tools, MF_STRING, RCWIN_CMD_SCRIPT_CONSOLE, L"&Script console...");
    AppendMenuW(tools, MF_STRING, RCWIN_CMD_AUDIO_TEST, L"Test &audio");

    AppendMenuW(help, MF_STRING, RCWIN_CMD_HELP, L"&Help\tF1");
    AppendMenuW(help, MF_STRING, RCWIN_CMD_SHORTCUTS, L"&Keyboard shortcuts\tF2");
    AppendMenuW(help, MF_STRING, RCWIN_CMD_CREDITS, L"&Credits");
    AppendMenuW(help, MF_STRING, RCWIN_CMD_SAFETY_NOTICE, L"&Safety notice");
    AppendMenuW(help, MF_SEPARATOR, 0, NULL);
    AppendMenuW(help, MF_STRING, RCWIN_CMD_ABOUT, L"&About RailControl");

    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&File");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"&View");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)control, L"&Control");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)tools, L"&Tools");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Help");

    return bar;
}

/* --------------------------------------------------------------------------
 * Registration and creation
 * -------------------------------------------------------------------------- */
static bool register_window_class(HINSTANCE instance)
{
    WNDCLASSEXW wc;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hIconSm = LoadIconW(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL; /* the paint handler fills the background */
    wc.lpszClassName = RCWIN_CLASS_NAME;

    if (RegisterClassExW(&wc) == 0)
    {
        return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    return true;
}

static HWND create_main_window(HINSTANCE instance, int show_command)
{
    HWND window;

    g_app.menu = build_menu();

    window = CreateWindowExW(
        0,
        RCWIN_CLASS_NAME,
        RCWIN_WINDOW_TITLE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        RCWIN_DEFAULT_WIDTH, RCWIN_DEFAULT_HEIGHT,
        NULL, g_app.menu, instance, NULL);

    if (window == NULL)
    {
        return NULL;
    }

    ShowWindow(window, show_command);
    UpdateWindow(window);
    SetFocus(window);
    return window;
}

/* --------------------------------------------------------------------------
 * Entry point
 * -------------------------------------------------------------------------- */
int rcwin_run(HINSTANCE instance, int show_command)
{
    MSG message;
    int exit_code = 0;

    memset(&g_app, 0, sizeof(g_app));
    g_app.instance = instance;
    g_app.zoom_percent = 100;
    g_app.show_grid = true;
    g_app.show_labels = true;
    g_app.running = true;

    create_resources();
    rcwin_terminal_init();

    if (!register_window_class(instance))
    {
        MessageBoxW(NULL, L"Could not register the window class.", RCWIN_WINDOW_TITLE,
                    MB_ICONERROR | MB_OK);
        return 1;
    }

    if (create_main_window(instance, show_command) == NULL)
    {
        MessageBoxW(NULL, L"Could not create the main window.", RCWIN_WINDOW_TITLE,
                    MB_ICONERROR | MB_OK);
        destroy_resources();
        return 1;
    }

    layout_panes(g_app.window);
    rcwin_refresh_state();

    while (g_app.running && GetMessageW(&message, NULL, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    destroy_resources();
    return exit_code;
}
