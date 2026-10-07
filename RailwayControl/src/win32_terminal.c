/* ==========================================================================
 * win32_terminal.c - 💻 The embedded terminal pane.
 *
 * A signaller who has learned the command set should not have to reach for the
 * mouse. This pane is the same command language as the standalone console
 * (terminal.h), so anything a script or a batch file can do, the operator can
 * type here - and vice versa.
 *
 * The pane is deliberately not a general text editor. It holds:
 *   - a scrollback of output, capped so the window cannot grow without bound
 *   - one input line, edited with the usual keys
 *   - the command history, shared with terminal.h so the up-arrow behaves the
 *     same in both front ends
 *
 * Every command is executed through terminal_execute(), which routes through
 * railway_api.h. A refused command comes back as TERM_ERR_REFUSED and is
 * echoed in red with the interlocking's reason - the pane never reports
 * success for something that was blocked.
 * ========================================================================== */
#include "win32.h"

#include "terminal.h"
#include "railcontrol.h"
#include "rc_version.h"

#include <stdio.h>
#include <string.h>

#define COL_PANEL RGB(20, 27, 36)
#define COL_GRID RGB(30, 40, 52)
#define COL_TEXT RGB(215, 227, 238)
#define COL_TEXT_DIM RGB(125, 143, 163)
#define COL_PROMPT RGB(120, 205, 255)
#define COL_ERROR RGB(235, 90, 90)
#define COL_OK RGB(70, 210, 110)

static void draw_text(HDC dc, int x, int y, COLORREF colour, const char *text)
{
    wchar_t wide[RCWIN_MAX_LINE * 2];

    SetTextColor(dc, colour);
    MultiByteToWideChar(CP_ACP, 0, text, -1, wide, RCWIN_MAX_LINE * 2);
    TextOutW(dc, x, y, wide, (int)wcslen(wide));
}

static void fill_rect(HDC dc, const RECT *rect, COLORREF colour)
{
    HBRUSH brush = CreateSolidBrush(colour);
    FillRect(dc, rect, brush);
    DeleteObject(brush);
}

/* --------------------------------------------------------------------------
 * Scrollback
 *
 * A fixed ring of lines. When it is full the oldest line is discarded, so the
 * pane has a hard memory bound no matter how long the console runs - which is
 * exactly what an operator wants from a scrollback: the recent past, not an
 * ever-growing file.
 * -------------------------------------------------------------------------- */
static void scrollback_append(const char *text)
{
    RcWinApp *app = rcwin_app();
    char buffer[RCWIN_MAX_LINE];
    const char *cursor = text;

    if (text == NULL)
    {
        return;
    }

    /* Split multi-line output so each scrollback entry is one display line. */
    while (*cursor != '\0')
    {
        const char *newline = strchr(cursor, '\n');
        size_t length = (newline != NULL)
                            ? (size_t)(newline - cursor)
                            : strlen(cursor);

        if (length >= sizeof(buffer))
        {
            length = sizeof(buffer) - 1;
        }
        memcpy(buffer, cursor, length);
        buffer[length] = '\0';
        /* Trim a trailing carriage return from CRLF output. */
        if (length > 0 && buffer[length - 1] == '\r')
        {
            buffer[length - 1] = '\0';
        }

        if (app->terminal_line_count < TERMINAL_PANE_LINES)
        {
            snprintf(app->terminal_scrollback[app->terminal_line_count],
                     RCWIN_MAX_LINE, "%s", buffer);
            app->terminal_line_count++;
        }
        else
        {
            /* Shift up by one and drop the oldest line. */
            memmove(&app->terminal_scrollback[0],
                    &app->terminal_scrollback[1],
                    (TERMINAL_PANE_LINES - 1) * RCWIN_MAX_LINE);
            snprintf(app->terminal_scrollback[TERMINAL_PANE_LINES - 1],
                     RCWIN_MAX_LINE, "%s", buffer);
        }

        if (newline == NULL)
        {
            break;
        }
        cursor = newline + 1;
    }
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */
void rcwin_terminal_init(void)
{
    RcWinApp *app = rcwin_app();

    app->terminal_input[0] = '\0';
    app->terminal_cursor = 0;
    app->terminal_line_count = 0;

    scrollback_append(terminal_banner());
    scrollback_append("");
    scrollback_append("Dispatch console ready. Type HELP for the command index.");
    scrollback_append("");
}

void rcwin_terminal_append(const char *text)
{
    scrollback_append(text);
    rcwin_request_repaint();
}

void rcwin_terminal_focus(void)
{
    /* The terminal is the only text input, so it always holds focus. This
     * exists so a future toolbar button can call it explicitly. */
    rcwin_request_repaint();
}

/* --------------------------------------------------------------------------
 * Execution
 * -------------------------------------------------------------------------- */
void rcwin_terminal_execute(void)
{
    RcWinApp *app = rcwin_app();
    TerminalResult result;
    char echo[RCWIN_MAX_LINE + 4];

    if (app->terminal_input[0] == '\0')
    {
        return;
    }

    /* Echo what was typed, prefixed with the prompt, exactly as a real console
     * does - the operator must be able to read back what they sent. */
    snprintf(echo, sizeof(echo), "RCP> %s", app->terminal_input);
    scrollback_append(echo);

    terminal_history_add(app->terminal_input);
    terminal_execute(app->terminal_input, &result);

    if (result.output[0] != '\0')
    {
        scrollback_append(result.output);
    }

    /* A refusal is the interesting case: it means the interlocking stopped the
     * action, and the operator needs the reason, not just "failed". */
    if (result.status == TERM_ERR_REFUSED)
    {
        char note[TERMINAL_MAX_LINE];
        snprintf(note, sizeof(note), "REFUSED: %s",
                 result.hint[0] != '\0' ? result.hint : "the interlocking refused this action");
        scrollback_append(note);
        rcwin_set_status_text(note);
    }
    else if (result.status == TERM_ERR_UNKNOWN || result.status == TERM_ERR_SYNTAX)
    {
        char note[TERMINAL_MAX_LINE];
        snprintf(note, sizeof(note), "ERROR: %s",
                 result.hint[0] != '\0' ? result.hint : "unknown command");
        scrollback_append(note);
        rcwin_set_status_text(note);
    }
    else if (result.status == TERM_OK)
    {
        rcwin_set_status_text("OK");
    }

    /* Clear the input line and redraw. */
    app->terminal_input[0] = '\0';
    app->terminal_cursor = 0;

    if (result.should_quit)
    {
        /* QUIT/EXIT from the console pane closes the window, matching the
         * behaviour of the standalone console. */
        PostMessageW(app->window, WM_CLOSE, 0, 0);
    }

    rcwin_request_repaint();
}

/* --------------------------------------------------------------------------
 * Keyboard editing
 *
 * Backspace and the history keys. Character insertion is handled by
 * win32_main.c's WM_CHAR so ordinary typing works through the normal message
 * path rather than being reinvented here.
 * -------------------------------------------------------------------------- */
void rcwin_terminal_key(WPARAM key)
{
    RcWinApp *app = rcwin_app();

    switch (key)
    {
    case VK_BACK:
        if (app->terminal_cursor > 0)
        {
            app->terminal_cursor--;
            app->terminal_input[app->terminal_cursor] = '\0';
        }
        break;

    case VK_UP:
        /* Walk back through the history, the way a shell does. */
        {
            const int count = terminal_history_count();
            if (count > 0)
            {
                const char *entry = terminal_history_at(0);
                if (entry != NULL)
                {
                    snprintf(app->terminal_input, sizeof(app->terminal_input),
                             "%s", entry);
                    app->terminal_cursor = (int)strlen(app->terminal_input);
                }
            }
        }
        break;

    case VK_DOWN:
        app->terminal_input[0] = '\0';
        app->terminal_cursor = 0;
        break;

    default:
        break;
    }

    rcwin_request_repaint();
}

/* --------------------------------------------------------------------------
 * Drawing
 * -------------------------------------------------------------------------- */
void rcwin_draw_terminal(HDC dc, const RECT *area)
{
    RcWinApp *app = rcwin_app();
    const int line_height = 15;
    const int input_height = 24;
    const int max_lines = (area->bottom - area->top - 26 - input_height) / line_height;
    int first;
    int i;
    int y;

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

    draw_text(dc, area->left + 8, area->top + 4, COL_TEXT_DIM,
              "TERMINAL  (HELP for commands, F5 pause, ESC clear line)");

    /* Show the newest lines that fit. */
    first = app->terminal_line_count - max_lines;
    if (first < 0)
    {
        first = 0;
    }

    y = area->top + 20;
    for (i = first; i < app->terminal_line_count; ++i)
    {
        COLORREF colour = COL_TEXT;
        const char *line = app->terminal_scrollback[i];

        /* Colour-code the lines the operator needs to notice. */
        if (strncmp(line, "REFUSED", 7) == 0)
        {
            colour = COL_ERROR;
        }
        else if (strncmp(line, "ERROR", 5) == 0)
        {
            colour = COL_ERROR;
        }
        else if (strncmp(line, "RCP>", 4) == 0)
        {
            colour = COL_PROMPT;
        }
        else if (strncmp(line, "OK", 2) == 0)
        {
            colour = COL_OK;
        }

        draw_text(dc, area->left + 8, y, colour, line);
        y += line_height;
    }

    /* Input line. */
    {
        const int input_y = area->bottom - input_height - 4;
        RECT input_rect;

        input_rect.left = area->left + 4;
        input_rect.top = input_y;
        input_rect.right = area->right - 4;
        input_rect.bottom = area->bottom - 4;
        draw_text(dc, input_rect.left + 4, input_y + 3, COL_PROMPT, "RCP>");
        draw_text(dc, input_rect.left + 46, input_y + 3, COL_TEXT, app->terminal_input);

        /* A caret, so the operator can see where the next character lands. */
        {
            SIZE size;
            GetTextExtentPoint32A(dc, app->terminal_input,
                                  (int)strlen(app->terminal_input), &size);
            draw_text(dc, input_rect.left + 46 + size.cx + 1, input_y + 3,
                      COL_PROMPT, "_");
        }
    }
}
