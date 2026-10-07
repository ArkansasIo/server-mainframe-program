/* ==========================================================================
 * win32_panel.c - 🛤️ The GDI track diagram.
 *
 * This is the picture a signaller actually works from: a schematic of the
 * layout with signals, points and trains drawn on it. It is the visual
 * equivalent of the panel in a signal box.
 *
 * Coordinates
 * -----------
 * The engine stores every asset with a normalised position (0..1) in
 * start_lat/start_lon etc. "lat" runs the length of the panel and "lon"
 * across it, so a track is a line from one point to the other. Drawing scales
 * those directly, which means the picture is generated from the same numbers
 * the train controller integrates against. There is no separate "diagram
 * layout" to fall out of step with the simulation - a track that moves in the
 * engine moves on the screen.
 *
 * Rendering rules that matter
 * --------------------------
 *   - Track occupancy drives colour: clear is grey, occupied is amber (a
 *     train is there), failed is red. Colour is never the sole indicator -
 *     occupied sections are also drawn thicker and failed ones dashed - so
 *     the diagram still reads in monochrome or to a colour-blind operator.
 *   - Signals show the real aspect, not a commanded one. A signal drawn green
 *     while the lamp is red would be a lie about the safety state.
 *   - Locked points are drawn with a filled centre.
 * ========================================================================== */
#include "win32.h"

#include "railway_api.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Colour palette
 * -------------------------------------------------------------------------- */
#define COL_BACK RGB(12, 16, 22)
#define COL_PANEL RGB(20, 27, 36)
#define COL_GRID RGB(30, 40, 52)
#define COL_TRACK RGB(120, 140, 165)
#define COL_OCCUPIED RGB(240, 190, 70)
#define COL_FAILED RGB(220, 60, 60)
#define COL_TEXT RGB(215, 227, 238)
#define COL_TEXT_DIM RGB(125, 143, 163)
#define COL_SIGNAL_RED RGB(235, 60, 60)
#define COL_SIGNAL_YEL RGB(235, 200, 60)
#define COL_SIGNAL_GRN RGB(70, 210, 110)
#define COL_SIGNAL_CAL RGB(150, 200, 235)
#define COL_TRAIN RGB(120, 205, 255)
#define COL_POINT RGB(200, 170, 90)
#define COL_POINT_LOCK RGB(240, 120, 120)

/* --------------------------------------------------------------------------
 * Scaling helpers
 *
 * The engine's "lat"/"lon" are interchangeable normalised axes. They are
 * mapped so that the longer window axis takes the longer layout axis, which
 * keeps the panel landscape even in a tall window.
 * -------------------------------------------------------------------------- */
typedef struct
{
    RECT area;
    int inset;
} PanelView;

static int panel_x(const PanelView *view, float lon)
{
    const int width = (view->area.right - view->area.left) - view->inset * 2;
    return view->area.left + view->inset + (int)(lon * (float)width);
}

static int panel_y(const PanelView *view, float lat)
{
    const int height = (view->area.bottom - view->area.top) - view->inset * 2;
    return view->area.top + view->inset + (int)(lat * (float)height);
}

/* A train's diagram position is a single lat/lon pair, already interpolated
 * by the controller along its track. */
static int train_x(const PanelView *view, const RwTrainInfo *train)
{
    return panel_x(view, train->longitude);
}

static int train_y(const PanelView *view, const RwTrainInfo *train)
{
    return panel_y(view, train->latitude);
}

/* --------------------------------------------------------------------------
 * Small drawing primitives
 * -------------------------------------------------------------------------- */
static void draw_text(HDC dc, int x, int y, COLORREF colour, const char *text)
{
    wchar_t wide[128];

    SetTextColor(dc, colour);
    MultiByteToWideChar(CP_ACP, 0, text, -1, wide, 128);
    TextOutW(dc, x, y, wide, (int)wcslen(wide));
}

static void fill_box(HDC dc, int x, int y, int size, COLORREF colour)
{
    RECT box;
    HBRUSH brush = CreateSolidBrush(colour);
    HBRUSH old;

    box.left = x - size / 2;
    box.top = y - size / 2;
    box.right = box.left + size;
    box.bottom = box.top + size;

    old = (HBRUSH)SelectObject(dc, brush);
    /* A signal is drawn as a head with a visible aspect, so a filled circle
     * reads better than a square at the sizes this panel is drawn at. */
    Ellipse(dc, box.left, box.top, box.right, box.bottom);
    SelectObject(dc, old);
    DeleteObject(brush);
}

static COLORREF aspect_colour(SignalState aspect)
{
    switch (aspect)
    {
    case SIGNAL_GREEN:
        return COL_SIGNAL_GRN;
    case SIGNAL_YELLOW:
        return COL_SIGNAL_YEL;
    case SIGNAL_CALLON:
        return COL_SIGNAL_CAL;
    case SIGNAL_RED:
    default:
        return COL_SIGNAL_RED;
    }
}

static const char *aspect_char(SignalState aspect)
{
    switch (aspect)
    {
    case SIGNAL_GREEN:
        return "G";
    case SIGNAL_YELLOW:
        return "Y";
    case SIGNAL_CALLON:
        return "C";
    case SIGNAL_RED:
    default:
        return "R";
    }
}

static const char *occupancy_name(TrackOccupancy occupancy)
{
    switch (occupancy)
    {
    case TRACK_CLEAR:
        return "CLEAR";
    case TRACK_OCCUPIED:
        return "OCCUPIED";
    case TRACK_UNKNOWN:
        return "UNKNOWN";
    }
    return "UNKNOWN";
}

/* --------------------------------------------------------------------------
 * Grid
 * -------------------------------------------------------------------------- */
static void draw_grid(HDC dc, const PanelView *view)
{
    const RcWinApp *app = rcwin_app();
    HPEN pen;
    HPEN old;
    int i;
    const int divisions = 10;

    if (!app->show_grid)
    {
        return;
    }

    pen = CreatePen(PS_SOLID, 1, COL_GRID);
    old = (HPEN)SelectObject(dc, pen);

    for (i = 1; i < divisions; ++i)
    {
        const float fraction = (float)i / (float)divisions;
        const int x = panel_x(view, fraction);
        const int y = panel_y(view, fraction);

        MoveToEx(dc, x, view->area.top + view->inset, NULL);
        LineTo(dc, x, view->area.bottom - view->inset);
        MoveToEx(dc, view->area.left + view->inset, y, NULL);
        LineTo(dc, view->area.right - view->inset, y);
    }

    SelectObject(dc, old);
    DeleteObject(pen);
}

/* --------------------------------------------------------------------------
 * Pane frame - the diagram sits inside a bordered box like a real panel.
 * -------------------------------------------------------------------------- */
static void draw_frame(HDC dc, const RECT *area, const char *title)
{
    HBRUSH brush = CreateSolidBrush(COL_PANEL);
    HPEN pen = CreatePen(PS_SOLID, 1, COL_GRID);
    HBRUSH old_brush = (HBRUSH)SelectObject(dc, brush);
    HPEN old_pen = (HPEN)SelectObject(dc, pen);
    RECT inner = *area;

    Rectangle(dc, area->left, area->top, area->right - 1, area->bottom - 1);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);

    if (title != NULL)
    {
        draw_text(dc, area->left + 8, area->top + 4, COL_TEXT_DIM, title);
    }
    (void)inner;
}

/* --------------------------------------------------------------------------
 * Tracks
 * -------------------------------------------------------------------------- */
void rcwin_draw_track(HDC dc, const RECT *area, const RwTrackInfo *track)
{
    PanelView view;
    HPEN pen;
    HPEN old;
    int x1;
    int y1;
    int x2;
    int y2;
    COLORREF colour;
    int width = 2;
    int style = PS_SOLID;

    view.area = *area;
    view.inset = 16;

    x1 = panel_x(&view, track->start_lon);
    y1 = panel_y(&view, track->start_lat);
    x2 = panel_x(&view, track->end_lon);
    y2 = panel_y(&view, track->end_lat);

    switch (track->occupancy)
    {
    case TRACK_OCCUPIED:
        colour = COL_OCCUPIED;
        width = 4;
        break;
    case TRACK_UNKNOWN:
        /* Dashed as well as red: the fail-safe state must be obvious even
         * without colour. */
        colour = COL_FAILED;
        width = 3;
        style = PS_DASH;
        break;
    case TRACK_CLEAR:
    default:
        colour = track->in_service ? COL_TRACK : COL_TEXT_DIM;
        break;
    }

    pen = CreatePen(style, width, colour);
    old = (HPEN)SelectObject(dc, pen);

    MoveToEx(dc, x1, y1, NULL);
    LineTo(dc, x2, y2);

    SelectObject(dc, old);
    DeleteObject(pen);

    if (rcwin_app()->show_labels)
    {
        const int mx = (x1 + x2) / 2;
        const int my = (y1 + y2) / 2;

        draw_text(dc, mx + 4, my - 14, COL_TEXT_DIM, track->name);

        if (track->occupancy != TRACK_CLEAR)
        {
            draw_text(dc, mx + 4, my + 2, colour, occupancy_name(track->occupancy));
        }
    }
}

/* --------------------------------------------------------------------------
 * Signals
 * -------------------------------------------------------------------------- */
void rcwin_draw_signal(HDC dc, const RECT *area, const RwSignalInfo *signal)
{
    PanelView view;
    int x;
    int y;
    char label[64];

    view.area = *area;
    view.inset = 16;

    x = panel_x(&view, signal->longitude);
    y = panel_y(&view, signal->latitude);

    /* A signal at danger is the normal state, so it is drawn smaller than a
     * cleared one - the eye should be drawn to what has changed. */
    fill_box(dc, x, y, signal->aspect == SIGNAL_RED ? 10 : 14,
             aspect_colour(signal->aspect));

    snprintf(label, sizeof(label), "%s %s%s",
             signal->name, aspect_char(signal->aspect),
             signal->manual ? " M" : "");
    draw_text(dc, x + 10, y - 8, COL_TEXT, label);
}

/* --------------------------------------------------------------------------
 * Points
 * -------------------------------------------------------------------------- */
void rcwin_draw_point(HDC dc, const RECT *area, const RwSwitchInfo *point)
{
    PanelView view;
    int x;
    int y;
    RECT box;
    HBRUSH brush;
    HBRUSH old;
    char label[64];

    view.area = *area;
    view.inset = 16;

    x = panel_x(&view, point->longitude);
    y = panel_y(&view, point->latitude);

    box.left = x - 7;
    box.top = y - 7;
    box.right = x + 7;
    box.bottom = y + 7;

    brush = CreateSolidBrush(point->locked ? COL_POINT_LOCK : COL_POINT);
    old = (HBRUSH)SelectObject(dc, brush);
    Rectangle(dc, box.left, box.top, box.right, box.bottom);
    SelectObject(dc, old);
    DeleteObject(brush);

    /* A filled centre means locked; a hollow one means it can be worked. The
     * lie is shown as an arm so the operator can see which way it is set. */
    if (!point->locked)
    {
        brush = CreateSolidBrush(COL_PANEL);
        old = (HBRUSH)SelectObject(dc, brush);
        Ellipse(dc, x - 3, y - 3, x + 3, y + 3);
        SelectObject(dc, old);
        DeleteObject(brush);
    }

    snprintf(label, sizeof(label), "%s %s",
             point->name,
             point->position == SWITCH_MAIN ? "N" : "R");
    draw_text(dc, x + 10, y - 8, COL_TEXT, label);
}

/* --------------------------------------------------------------------------
 * Trains
 * -------------------------------------------------------------------------- */
void rcwin_draw_train(HDC dc, const RECT *area, const RwTrainInfo *train)
{
    PanelView view;
    int x;
    int y;
    int length_px;
    RECT body;
    HBRUSH brush;
    HBRUSH old;
    COLORREF colour;
    char label[64];

    view.area = *area;
    view.inset = 16;

    x = train_x(&view, train);
    y = train_y(&view, train);

    /* Scale the drawn length by the formation size so a 9-car set looks
     * longer than a light engine - the diagram should show what the train is,
     * not just where it is. */
    length_px = 14 + train->carriages * 3;
    if (length_px > 90)
    {
        length_px = 90;
    }

    switch (train->state)
    {
    case TRAIN_RUNNING:
        colour = COL_TRAIN;
        break;
    case TRAIN_EMERGENCY_STOP:
        colour = COL_SIGNAL_RED;
        break;
    case TRAIN_STOPPED:
    default:
        colour = COL_TEXT_DIM;
        break;
    }

    body.left = x - length_px / 2;
    body.top = y - 5;
    body.right = body.left + length_px;
    body.bottom = y + 5;

    brush = CreateSolidBrush(colour);
    old = (HBRUSH)SelectObject(dc, brush);
    Rectangle(dc, body.left, body.top, body.right, body.bottom);
    SelectObject(dc, old);
    DeleteObject(brush);

    if (rcwin_app()->show_labels)
    {
        snprintf(label, sizeof(label), "%s %s %.0f",
                 train->name,
                 train->state == TRAIN_EMERGENCY_STOP ? "EMG" : train->state == TRAIN_RUNNING ? "RUN"
                                                                                              : "STP",
                 train->speed_kph);
        draw_text(dc, x - length_px / 2, y + 7, colour, label);
    }
}

/* --------------------------------------------------------------------------
 * The whole diagram
 * -------------------------------------------------------------------------- */
void rcwin_draw_diagram(HDC dc, const RECT *area)
{
    PanelView view;
    int i;
    char caption[128];
    const RcWinApp *app = rcwin_app();

    view.area = *area;
    view.inset = 16;

    draw_frame(dc, area, "TRACK DIAGRAM");
    draw_grid(dc, &view);

    if (!railway_is_initialized())
    {
        draw_text(dc, area->left + 16, area->top + 30, COL_FAILED,
                  "ENGINE NOT INITIALISED");
        return;
    }

    /* Draw order matters: tracks, then points, then signals, then trains.
     * Trains on top means a train is never hidden behind the track it sits
     * on, which is the one thing the operator must always be able to see. */
    for (i = 0; i < railway_track_count(); ++i)
    {
        RwTrackInfo track;
        if (railway_get_track_at(i, &track) == RW_OK)
        {
            rcwin_draw_track(dc, area, &track);
        }
    }
    for (i = 0; i < railway_switch_count(); ++i)
    {
        RwSwitchInfo point;
        if (railway_get_switch_at(i, &point) == RW_OK)
        {
            rcwin_draw_point(dc, area, &point);
        }
    }
    for (i = 0; i < railway_signal_count(); ++i)
    {
        RwSignalInfo signal;
        if (railway_get_signal_at(i, &signal) == RW_OK)
        {
            rcwin_draw_signal(dc, area, &signal);
        }
    }
    for (i = 0; i < railway_train_count(); ++i)
    {
        RwTrainInfo train;
        if (railway_get_train_at(i, &train) == RW_OK)
        {
            rcwin_draw_train(dc, area, &train);
        }
    }

    /* A persistent banner across the panel while the emergency stop is active:
     * this state must be impossible to miss. */
    if (app->status.state == SYSTEM_EMERGENCY)
    {
        char reason[RW_MAX_TEXT];
        RECT banner = *area;
        HBRUSH brush;
        HBRUSH old;

        if (railway_emergency_active())
        {
            snprintf(reason, sizeof(reason), "EMERGENCY STOP ACTIVE");
        }
        else
        {
            snprintf(reason, sizeof(reason), "EMERGENCY");
        }

        banner.top = area->bottom - 34;
        banner.bottom = area->bottom - 6;
        banner.left = area->left + 16;
        banner.right = area->right - 16;

        brush = CreateSolidBrush(COL_SIGNAL_RED);
        old = (HBRUSH)SelectObject(dc, brush);
        Rectangle(dc, banner.left, banner.top, banner.right, banner.bottom);
        SelectObject(dc, old);
        DeleteObject(brush);

        draw_text(dc, banner.left + 10, banner.top + 7, RGB(255, 255, 255), reason);
    }

    /* Diagram caption, bottom-left of the panel. */
    snprintf(caption, sizeof(caption),
             "%d tracks  %d signals  %d points  %d trains   zoom %d%%%s",
             railway_track_count(), railway_signal_count(),
             railway_switch_count(), railway_train_count(),
             app->zoom_percent, app->paused ? "   [PAUSED]" : "");
    draw_text(dc, area->left + 16, area->bottom - 22, COL_TEXT_DIM, caption);
}
