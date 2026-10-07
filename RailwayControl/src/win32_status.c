/* ==========================================================================
 * win32_status.c - 📝 Header, event log and status bar.
 *
 * Three small panes, all read-only:
 *
 *   header      one line at the top: system, state, alarms, clock
 *   event log   the most recent engine events, newest first
 *   status bar  operator, role, tick count, and whatever the last command
 *               reported - including its refusal
 *
 * The status bar is the important one. Every command the operator issues
 * reports its outcome here, and a refused command says so in the same place a
 * successful one would. A UI that only surfaced success would train the
 * operator to believe the interlocking obeyed them.
 * ========================================================================== */
#include "win32.h"

#include "railway_api.h"
#include "railcontrol.h"
#include "rc_account.h"

#include <stdio.h>
#include <string.h>

/* Palette, kept identical to the diagram so the window reads as one panel. */
#define COL_BACK RGB(12, 16, 22)
#define COL_PANEL RGB(20, 27, 36)
#define COL_HEADER RGB(26, 36, 48)
#define COL_STATUS RGB(26, 36, 48)
#define COL_GRID RGB(30, 40, 52)
#define COL_TEXT RGB(215, 227, 238)
#define COL_TEXT_DIM RGB(125, 143, 163)
#define COL_INFO RGB(120, 205, 255)
#define COL_WARN RGB(235, 190, 70)
#define COL_ALARM RGB(235, 90, 90)
#define COL_CRITICAL RGB(255, 60, 60)
#define COL_OK RGB(70, 210, 110)

static void draw_text(HDC dc, int x, int y, COLORREF colour, const char *text)
{
    wchar_t wide[512];

    SetTextColor(dc, colour);
    MultiByteToWideChar(CP_ACP, 0, text, -1, wide, 512);
    TextOutW(dc, x, y, wide, (int)wcslen(wide));
}

static void fill_rect(HDC dc, const RECT *rect, COLORREF colour)
{
    HBRUSH brush = CreateSolidBrush(colour);
    FillRect(dc, rect, brush);
    DeleteObject(brush);
}

static COLORREF severity_colour(RwSeverity severity)
{
    switch (severity)
    {
    case SEVERITY_CRITICAL:
        return COL_CRITICAL;
    case SEVERITY_ALARM:
        return COL_ALARM;
    case SEVERITY_WARNING:
        return COL_WARN;
    case SEVERITY_INFO:
    default:
        return COL_INFO;
    }
}

/* The status line text posted by the last command. */
extern const wchar_t *rcwin_last_status(void);

/* --------------------------------------------------------------------------
 * Header
 * -------------------------------------------------------------------------- */
void rcwin_draw_header(HDC dc, const RECT *area)
{
    const RcWinApp *app = rcwin_app();
    char left[256];
    char right[256];
    SIZE size;
    COLORREF state_colour;
    const char *state_name = "UNKNOWN";

    fill_rect(dc, area, COL_HEADER);

    if (railway_is_initialized())
    {
        state_name = rw_safety_name(app->status.state);
    }

    switch (app->status.state)
    {
    case SYSTEM_ONLINE:
        state_colour = COL_OK;
        break;
    case SYSTEM_DEGRADED:
        state_colour = COL_WARN;
        break;
    case SYSTEM_EMERGENCY:
        state_colour = COL_CRITICAL;
        break;
    case SYSTEM_SHUTDOWN:
        state_colour = COL_ALARM;
        break;
    case SYSTEM_INITIALISING:
    default:
        state_colour = COL_TEXT_DIM;
        break;
    }

    snprintf(left, sizeof(left), "%s  /  %s  /  %s",
             railway_is_initialized() ? railway_system_name() : "(no engine)",
             railway_is_initialized() ? railway_signal_box_name() : "-",
             state_name);

    snprintf(right, sizeof(right),
             "signals %2d/%2d  points %2d/%2d  tracks %2d occ  trains %2d run  routes %2d  ALARMS %d",
             app->status.signals_green, app->status.signals_total,
             app->status.switches_locked, app->status.switches_total,
             app->status.tracks_occupied, app->status.trains_running,
             app->status.routes_locked, app->status.alarms);

    draw_text(dc, area->left + 10, area->top + 7, state_colour, left);

    GetTextExtentPoint32A(dc, right, (int)strlen(right), &size);
    draw_text(dc, area->right - size.cx - 10, area->top + 7,
              app->status.alarms > 0 ? COL_ALARM : COL_TEXT_DIM, right);
}

/* --------------------------------------------------------------------------
 * Event log
 * -------------------------------------------------------------------------- */
void rcwin_draw_event_log(HDC dc, const RECT *area)
{
    int count;
    int i;
    int y;
    const int line_height = 15;
    const int max_lines = (area->bottom - area->top - 26) / line_height;
    char line[320];

    fill_rect(dc, area, COL_PANEL);

    /* Frame. */
    {
        HPEN pen = CreatePen(PS_SOLID, 1, COL_GRID);
        HPEN old = (HPEN)SelectObject(dc, pen);
        HBRUSH old_brush = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, area->left, area->top, area->right - 1, area->bottom - 1);
        SelectObject(dc, old_brush);
        SelectObject(dc, old);
        DeleteObject(pen);
    }

    draw_text(dc, area->left + 8, area->top + 4, COL_TEXT_DIM, "EVENT LOG");

    if (!railway_is_initialized())
    {
        return;
    }

    count = railway_event_count();
    y = area->top + 20;

    /* Newest first: the operator cares about what just happened. */
    for (i = 0; i < count && i < max_lines; ++i)
    {
        RwEventInfo event;

        if (railway_get_event_at(count - 1 - i, &event) != RW_OK)
        {
            break;
        }

        snprintf(line, sizeof(line), "%-8s %-9s %s%s",
                 event.timestamp, severity_name(event.severity),
                 event.message,
                 event.acknowledged ? "" : "   [unack]");

        draw_text(dc, area->left + 8, y, severity_colour(event.severity), line);
        y += line_height;
    }

    if (count == 0)
    {
        draw_text(dc, area->left + 8, y, COL_TEXT_DIM, "(no events)");
    }
}

/* --------------------------------------------------------------------------
 * Status bar
 * -------------------------------------------------------------------------- */
void rcwin_draw_status_bar(HDC dc, const RECT *area)
{
    const RcWinApp *app = rcwin_app();
    const RcAccount *account = rc_session_account(rc_current_session());
    char operator_line[192];
    char right[192];
    SIZE size;

    fill_rect(dc, area, COL_STATUS);

    /* Left: who is signed on, and what the last command reported. */
    snprintf(operator_line, sizeof(operator_line), "%-12s %-11s |",
             account != NULL ? account->username : "(not signed on)",
             account != NULL ? rc_role_name(account->role) : "-");
    draw_text(dc, area->left + 10, area->top + 4, COL_TEXT, operator_line);

    GetTextExtentPoint32A(dc, operator_line, (int)strlen(operator_line), &size);
    {
        const wchar_t *status = rcwin_last_status();
        char narrow[256];

        if (status != NULL && status[0] != L'\0') {
            WideCharToMultiByte(CP_ACP, 0, status, -1, narrow, (int)sizeof(narrow),
                                NULL, NULL);
            draw_text(dc, area->left + 10 + size.cx + 8, area->top + 4,
                      COL_TEXT, narrow);
        }
    }

    /* Right: run counters. */
    snprintf(right, sizeof(right), "uptime %lus  ticks %lu%s",
             app->status.uptime_seconds, app->status.ticks,
             app->paused ? "  PAUSED" : "");

    GetTextExtentPoint32A(dc, right, (int)strlen(right), &size);
    draw_text(dc, area->right - size.cx - 10, area->top + 4,
              COL_TEXT_DIM, right);
}

/* --------------------------------------------------------------------------
 * Modal confirmation
 *
 * Used before an irreversible action (closing the console). Everything that
 * changes railway state goes through the interlocking instead and needs no
 * confirmation prompt: the interlocking is the thing that decides.
 * -------------------------------------------------------------------------- */
bool rcwin_confirm(const wchar_t *question)
{
    const int answer = MessageBoxW(rcwin_app()->window, question, RCWIN_WINDOW_TITLE,
                                   MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2);
    return answer == IDYES;
}
