/* ==========================================================================
 * persist.c - 💾 Save and restore the railway, and the working timetable.
 *
 * Read persist.h first. The two things that shape this implementation are:
 *
 *   1. The snapshot IS the interchange format. The same JSON shape is what the
 *      hand-written files in data/ use and what SAVE writes back. One loader,
 *      so a hand-edited layout and a saved state cannot take different paths
 *      through the code and drift apart.
 *
 *   2. A load validates before it applies. The file is parsed in full, checked
 *      against the engine, and only then committed. A snapshot referring to a
 *      track that does not exist is refused intact. Half-applying a layout is
 *      precisely the state an interlocking must never be left in.
 *
 * There is no JSON library in this project, so there is a small reader below.
 * It is deliberately a *validator* rather than a lenient parser: it understands
 * the subset the format uses, rejects anything it does not recognise, and never
 * guesses. A permissive parser that silently skips what it cannot read is how
 * a settings file loses half its contents without anybody noticing.
 * ========================================================================== */
#include "persist.h"
#include "railway_internal.h"
#include "rc_version.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ==========================================================================
 * Small JSON reader
 *
 * A cursor over a NUL-terminated buffer. Helpers skip whitespace, read a
 * scalar, and match keys. Anything unexpected sets the error flag, and every
 * subsequent read becomes a no-op, so a caller can parse a whole file and
 * check one flag at the end rather than testing every line.
 * ========================================================================== */
typedef struct
{
    const char *cursor;
    const char *end;
    bool failed;
    char error[RC_PERSIST_MAX_TEXT];
} JsonReader;

static void json_fail(JsonReader *r, const char *reason)
{
    if (!r->failed)
    {
        r->failed = true;
        snprintf(r->error, sizeof(r->error), "%s", reason);
    }
}

static void json_skip_ws(JsonReader *r)
{
    while (r->cursor < r->end)
    {
        const char c = *r->cursor;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
        {
            ++r->cursor;
            continue;
        }
        break;
    }
}

static bool json_at(JsonReader *r, char c)
{
    json_skip_ws(r);
    return r->cursor < r->end && *r->cursor == c;
}

static bool json_accept(JsonReader *r, char c)
{
    if (json_at(r, c))
    {
        ++r->cursor;
        return true;
    }
    return false;
}

static void json_expect(JsonReader *r, char c)
{
    if (!json_accept(r, c))
    {
        char reason[96];
        snprintf(reason, sizeof(reason), "expected '%c' in the JSON", c);
        json_fail(r, reason);
    }
}

/** Read a quoted string into `out`. Escapes are decoded; \u is left as-is
 *  because the format does not use it. */
static void json_string(JsonReader *r, char *out, size_t size)
{
    size_t used = 0;

    if (out != NULL && size > 0)
    {
        out[0] = '\0';
    }
    if (r->failed || !json_accept(r, '"'))
    {
        json_fail(r, "expected a quoted string");
        return;
    }

    while (r->cursor < r->end && *r->cursor != '"')
    {
        char c = *r->cursor++;

        if (c == '\\' && r->cursor < r->end)
        {
            const char escaped = *r->cursor++;
            switch (escaped)
            {
            case 'n':
                c = '\n';
                break;
            case 't':
                c = '\t';
                break;
            case 'r':
                c = '\r';
                break;
            case '"':
                c = '"';
                break;
            case '\\':
                c = '\\';
                break;
            case '/':
                c = '/';
                break;
            default:
                c = escaped;
                break;
            }
        }
        if (out != NULL && used + 1 < size)
        {
            out[used++] = c;
        }
    }

    if (!json_accept(r, '"'))
    {
        json_fail(r, "unterminated string");
        return;
    }
    if (out != NULL && size > 0)
    {
        out[used < size ? used : size - 1] = '\0';
    }
}

/** Skip one complete value of any type. Used for keys the loader does not
 *  consume, so an extra field in the file is tolerated without being parsed. */
static void json_skip_value(JsonReader *r)
{
    int depth = 0;

    json_skip_ws(r);
    if (r->cursor >= r->end)
    {
        return;
    }

    if (*r->cursor == '"')
    {
        char scratch[8];
        json_string(r, scratch, sizeof(scratch));
        return;
    }
    if (*r->cursor == '{' || *r->cursor == '[')
    {
        const char open = *r->cursor;
        const char close = (open == '{') ? '}' : ']';

        ++r->cursor;
        depth = 1;
        while (r->cursor < r->end && depth > 0)
        {
            const char c = *r->cursor;
            if (c == '"')
            {
                char scratch[8];
                json_string(r, scratch, sizeof(scratch));
                continue;
            }
            if (c == open)
            {
                depth++;
            }
            else if (c == close)
            {
                depth--;
            }
            ++r->cursor;
        }
        return;
    }

    /* A bare literal: number, true, false, null. */
    while (r->cursor < r->end)
    {
        const char c = *r->cursor;
        if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\n' || c == '\r' || c == '\t')
        {
            break;
        }
        ++r->cursor;
    }
}

/**
 * Drain the remainder of an array once the elements we wanted have been read.
 *
 * This MUST terminate. The obvious `while (!json_at(r, ']')) json_skip_value(r);`
 * spins forever on a truncated file: at end-of-input json_at() returns false
 * without advancing, so json_skip_value() has nothing to consume and the loop
 * never exits. The cursor-progress check below turns a malformed file into a
 * failed parse instead of a hung program.
 */
static void json_drain_array(JsonReader *r)
{
    while (r->cursor < r->end && !json_at(r, ']'))
    {
        const char *before = r->cursor;

        json_skip_value(r);
        if (r->cursor == before)
        {
            break;   /* nothing consumed: refuse rather than spin */
        }
    }
    if (!json_accept(r, ']'))
    {
        json_fail(r, "unterminated array");
    }
}

static double json_number(JsonReader *r)
{
    char buffer[32];
    size_t used = 0;
    char *endptr = NULL;
    double value;

    /* A JSON null is a legitimate value for an optional numeric field (a
     * freight service has no platform). Treat it as 0 so a file that spells
     * "no value" the JSON way is not rejected. */
    json_skip_ws(r);
    if (r->cursor + 4 <= r->end && strncmp(r->cursor, "null", 4) == 0)
    {
        r->cursor += 4;
        return 0.0;
    }

    while (r->cursor < r->end && used + 1 < sizeof(buffer))
    {
        const char c = *r->cursor;
        if (isdigit((unsigned char)c) || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E')
        {
            buffer[used++] = c;
            ++r->cursor;
            continue;
        }
        break;
    }
    buffer[used] = '\0';

    if (used == 0)
    {
        json_fail(r, "expected a number");
        return 0.0;
    }
    value = strtod(buffer, &endptr);
    if (endptr == buffer)
    {
        json_fail(r, "malformed number");
        return 0.0;
    }
    return value;
}

static bool json_bool(JsonReader *r)
{
    json_skip_ws(r);
    if (r->cursor + 4 <= r->end && strncmp(r->cursor, "true", 4) == 0)
    {
        r->cursor += 4;
        return true;
    }
    if (r->cursor + 5 <= r->end && strncmp(r->cursor, "false", 5) == 0)
    {
        r->cursor += 5;
        return false;
    }
    json_fail(r, "expected true or false");
    return false;
}

/* --------------------------------------------------------------------------
 * Object-walking helpers
 *
 * The pattern throughout: enter an object, then loop over its keys, consuming
 * the ones this loader understands and skipping the rest. Skipping rather than
 * failing is what lets one snapshot format carry sections a given build does
 * not use yet.
 * -------------------------------------------------------------------------- */
static bool json_enter_object(JsonReader *r)
{
    return json_accept(r, '{');
}

static bool json_next_member(JsonReader *r, bool *first)
{
    if (r->failed)
    {
        return false;
    }
    if (*first)
    {
        *first = false;
    }
    else if (!json_accept(r, ','))
    {
        return false;
    }
    return json_at(r, '"');
}

static bool json_enter_array(JsonReader *r)
{
    return json_accept(r, '[');
}

/** Read the next element of an array. Returns false at the closing bracket. */
static bool json_next_element(JsonReader *r, bool *first)
{
    if (r->failed)
    {
        return false;
    }
    if (*first)
    {
        *first = false;
        return !json_at(r, ']');
    }
    if (json_accept(r, ','))
    {
        return true;
    }
    return false;
}

/* ==========================================================================
 * Writing
 * ========================================================================== */
static FILE *g_out = NULL;

static void write_indent(int depth)
{
    int i;
    for (i = 0; i < depth * 2; ++i)
    {
        fputc(' ', g_out);
    }
}

static void write_string(const char *text)
{
    fputc('"', g_out);
    for (; text != NULL && *text != '\0'; ++text)
    {
        switch (*text)
        {
        case '"':
            fputs("\\\"", g_out);
            break;
        case '\\':
            fputs("\\\\", g_out);
            break;
        case '\n':
            fputs("\\n", g_out);
            break;
        case '\t':
            fputs("\\t", g_out);
            break;
        case '\r':
            fputs("\\r", g_out);
            break;
        default:
            fputc(*text, g_out);
            break;
        }
    }
    fputc('"', g_out);
}

static const char *train_class_code(TrainClass cls)
{
    switch (cls)
    {
    case TRAIN_CLASS_PASSENGER:
        return "passenger";
    case TRAIN_CLASS_FREIGHT:
        return "freight";
    case TRAIN_CLASS_ENGINEERING:
        return "engineering";
    case TRAIN_CLASS_LIGHT_ENGINE:
        return "light-engine";
    }
    return "passenger";
}

static const char *car_type_code(RcpCarType type)
{
    switch (type)
    {
    case CAR_CLASS_LOCOMOTIVE:
        return "locomotive";
    case CAR_CLASS_PASSENGER:
        return "passenger";
    case CAR_CLASS_FREIGHT_BOX:
        return "freight-box";
    case CAR_CLASS_FREIGHT_FLAT:
        return "freight-flat";
    case CAR_CLASS_TANK:
        return "tank";
    case CAR_CLASS_HOPPER:
        return "hopper";
    case CAR_CLASS_BRAKE_VAN:
        return "brake-van";
    case CAR_CLASS_DINING:
        return "dining";
    case CAR_CLASS_SLEEPER:
        return "sleeper";
    case CAR_CLASS_ENGINEERING:
        return "engineering";
    }
    return "passenger";
}

static const char *sensor_kind_code(RwSensorKind kind)
{
    switch (kind)
    {
    case SENSOR_TRACK_CIRCUIT:
        return "track-circuit";
    case SENSOR_AXLE_COUNTER:
        return "axle-counter";
    case SENSOR_TREADLE:
        return "treadle";
    case SENSOR_HOT_BOX:
        return "hot-box";
    case SENSOR_LEVEL_CROSSING:
        return "level-crossing";
    }
    return "track-circuit";
}

static RwSensorKind sensor_kind_parse(const char *text)
{
    if (text == NULL)
        return SENSOR_TRACK_CIRCUIT;
    if (strcmp(text, "axle-counter") == 0)
        return SENSOR_AXLE_COUNTER;
    if (strcmp(text, "treadle") == 0)
        return SENSOR_TREADLE;
    if (strcmp(text, "hot-box") == 0)
        return SENSOR_HOT_BOX;
    if (strcmp(text, "level-crossing") == 0)
        return SENSOR_LEVEL_CROSSING;
    return SENSOR_TRACK_CIRCUIT;
}

static const char *occupancy_code(TrackOccupancy occupancy)
{
    switch (occupancy)
    {
    case TRACK_CLEAR:
        return "clear";
    case TRACK_OCCUPIED:
        return "occupied";
    case TRACK_UNKNOWN:
        return "unknown";
    }
    return "unknown";
}

static TrackOccupancy occupancy_parse(const char *text)
{
    if (text == NULL)
        return TRACK_CLEAR;
    if (strcmp(text, "occupied") == 0)
        return TRACK_OCCUPIED;
    if (strcmp(text, "unknown") == 0)
        return TRACK_UNKNOWN;
    return TRACK_CLEAR;
}

static const char *aspect_code(SignalState aspect)
{
    switch (aspect)
    {
    case SIGNAL_RED:
        return "red";
    case SIGNAL_YELLOW:
        return "yellow";
    case SIGNAL_GREEN:
        return "green";
    case SIGNAL_CALLON:
        return "call-on";
    }
    return "red";
}

static SignalState aspect_parse(const char *text)
{
    if (text == NULL)
        return SIGNAL_RED;
    if (strcmp(text, "green") == 0)
        return SIGNAL_GREEN;
    if (strcmp(text, "yellow") == 0)
        return SIGNAL_YELLOW;
    if (strcmp(text, "call-on") == 0)
        return SIGNAL_CALLON;
    return SIGNAL_RED;
}

static const char *point_position_code(SwitchPosition position)
{
    return position == SWITCH_DIVERGING ? "reverse" : "normal";
}

static SwitchPosition point_position_parse(const char *text)
{
    return (text != NULL && strcmp(text, "reverse") == 0) ? SWITCH_DIVERGING : SWITCH_MAIN;
}

/* ==========================================================================
 * The engine, and the error slot
 * ========================================================================== */
static char g_last_error[RC_PERSIST_MAX_TEXT];

const char *rc_persist_last_error(void)
{
    return g_last_error;
}

static void set_error(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(g_last_error, sizeof(g_last_error), format, args);
    va_end(args);
}

bool rc_persist_file_exists(const char *path)
{
    FILE *file;

    if (path == NULL || path[0] == '\0')
    {
        return false;
    }
    file = fopen(path, "rb");
    if (file == NULL)
    {
        return false;
    }
    fclose(file);
    return true;
}

const char *rc_persist_default_path(void)
{
    /* Relative to the working directory, matching how the other data files
     * are addressed. The caller can always pass an explicit path. */
    return "data/snapshot.json";
}

/* ==========================================================================
 * Saving
 * ========================================================================== */
static void write_meta(int depth, const char *name, const char *description)
{
    write_indent(depth);
    fputs("\"meta\": {\n", g_out);
    write_indent(depth + 1);
    fputs("\"name\": ", g_out);
    write_string(name);
    fputs(",\n", g_out);
    write_indent(depth + 1);
    fputs("\"version\": ", g_out);
    write_string(RC_VERSION_SHORT);
    fputs(",\n", g_out);
    write_indent(depth + 1);
    fputs("\"generatedBy\": ", g_out);
    write_string(RC_PRODUCT_NAME);
    fputs(",\n", g_out);
    write_indent(depth + 1);
    fputs("\"description\": ", g_out);
    write_string(description);
    fputs("\n", g_out);
    write_indent(depth);
    fputs("}", g_out);
}

static void write_layout_section(RailwayEngine *e, int depth, int *counts)
{
    size_t i;

    /* ---- tracks ---- */
    write_indent(depth);
    fputs("\"tracks\": [\n", g_out);
    for (i = 0; i < e->track_count; ++i)
    {
        const TrackState *t = &e->tracks[i];

        write_indent(depth + 1);
        fputs("{\n", g_out);
        write_indent(depth + 2);
        fprintf(g_out, "\"id\": %u,\n", (unsigned)t->id);
        write_indent(depth + 2);
        fputs("\"name\": ", g_out);
        write_string(t->name);
        fputs(",\n", g_out);
        write_indent(depth + 2);
        fprintf(g_out, "\"lat1\": %.3f, \"lon1\": %.3f, ", (double)t->start_lat, (double)t->start_lon);
        fprintf(g_out, "\"lat2\": %.3f, \"lon2\": %.3f,\n", (double)t->end_lat, (double)t->end_lon);
        write_indent(depth + 2);
        fprintf(g_out, "\"speed\": %d, \"length\": %.0f,\n",
                t->speed_limit_kph, (double)t->length_m);
        write_indent(depth + 2);
        fputs("\"occupancy\": ", g_out);
        write_string(occupancy_code(t->occupancy));
        fputs(",\n", g_out);
        write_indent(depth + 2);
        fprintf(g_out, "\"occupiedBy\": %u,\n", (unsigned)t->occupied_by);
        write_indent(depth + 2);
        fprintf(g_out, "\"circuitFailed\": %s,\n",
                t->circuit_failed ? "true" : "false");
        write_indent(depth + 2);
        fprintf(g_out, "\"inService\": %s,\n",
                t->in_service ? "true" : "false");
        write_indent(depth + 2);
        fprintf(g_out, "\"pointUpstream\": %u,\n", (unsigned)t->point_upstream);
        write_indent(depth + 2);
        fprintf(g_out, "\"pointDownstream\": %u\n", (unsigned)t->point_downstream);
        write_indent(depth + 1);
        fprintf(g_out, "}%s\n", (i + 1 < e->track_count) ? "," : "");
        counts[0]++;
    }
    write_indent(depth);
    fputs("],\n", g_out);

    /* ---- points ---- */
    write_indent(depth);
    fputs("\"points\": [\n", g_out);
    for (i = 0; i < e->point_count; ++i)
    {
        const PointState *p = &e->points[i];

        write_indent(depth + 1);
        fputs("{\n", g_out);
        write_indent(depth + 2);
        fprintf(g_out, "\"id\": %u,\n", (unsigned)p->id);
        write_indent(depth + 2);
        fputs("\"name\": ", g_out);
        write_string(p->name);
        fputs(",\n", g_out);
        write_indent(depth + 2);
        fprintf(g_out, "\"trackUpstream\": %u, \"trackNormal\": %u, \"trackReverse\": %u,\n",
                (unsigned)p->track_upstream, (unsigned)p->track_normal,
                (unsigned)p->track_reverse);
        write_indent(depth + 2);
        fputs("\"position\": ", g_out);
        write_string(point_position_code(p->position));
        fputs(",\n", g_out);
        write_indent(depth + 2);
        fprintf(g_out, "\"lat\": %.3f, \"lon\": %.3f,\n",
                (double)p->latitude, (double)p->longitude);
        write_indent(depth + 2);
        fprintf(g_out, "\"locked\": %s, \"fault\": %s, \"inService\": %s\n",
                p->locked ? "true" : "false",
                p->fault ? "true" : "false",
                p->in_service ? "true" : "false");
        write_indent(depth + 1);
        fprintf(g_out, "}%s\n", (i + 1 < e->point_count) ? "," : "");
        counts[1]++;
    }
    write_indent(depth);
    fputs("],\n", g_out);

    /* ---- signals ---- */
    write_indent(depth);
    fputs("\"signals\": [\n", g_out);
    for (i = 0; i < e->signal_count; ++i)
    {
        const SignalStateInternal *s = &e->signals[i];

        write_indent(depth + 1);
        fputs("{\n", g_out);
        write_indent(depth + 2);
        fprintf(g_out, "\"id\": %u,\n", (unsigned)s->id);
        write_indent(depth + 2);
        fputs("\"name\": ", g_out);
        write_string(s->name);
        fputs(",\n", g_out);
        write_indent(depth + 2);
        fprintf(g_out, "\"track\": %u,\n", (unsigned)s->track);
        write_indent(depth + 2);
        fputs("\"aspect\": ", g_out);
        write_string(aspect_code(s->aspect));
        fputs(",\n", g_out);
        write_indent(depth + 2);
        fprintf(g_out, "\"atExit\": %s, \"manual\": %s, \"inService\": %s\n",
                s->at_exit ? "true" : "false",
                s->manual ? "true" : "false",
                s->in_service ? "true" : "false");
        write_indent(depth + 1);
        fprintf(g_out, "}%s\n", (i + 1 < e->signal_count) ? "," : "");
        counts[2]++;
    }
    write_indent(depth);
    fputs("],\n", g_out);

    /* ---- sensors ---- */
    write_indent(depth);
    fputs("\"sensors\": [\n", g_out);
    for (i = 0; i < e->sensor_count; ++i)
    {
        const SensorState *s = &e->sensors[i];

        write_indent(depth + 1);
        fputs("{\n", g_out);
        write_indent(depth + 2);
        fprintf(g_out, "\"id\": %u,\n", (unsigned)s->id);
        write_indent(depth + 2);
        fputs("\"name\": ", g_out);
        write_string(s->name);
        fputs(",\n", g_out);
        write_indent(depth + 2);
        fputs("\"kind\": ", g_out);
        write_string(sensor_kind_code(s->kind));
        fputs(",\n", g_out);
        write_indent(depth + 2);
        fprintf(g_out, "\"track\": %u, \"point\": %u,\n",
                (unsigned)s->track, (unsigned)s->point);
        write_indent(depth + 2);
        fprintf(g_out, "\"value\": %d, \"inService\": %s\n",
                s->value, s->in_service ? "true" : "false");
        write_indent(depth + 1);
        fprintf(g_out, "}%s\n", (i + 1 < e->sensor_count) ? "," : "");
        counts[3]++;
    }
    write_indent(depth);
    fputs("],\n", g_out);

    /* ---- routes ---- */
    write_indent(depth);
    fputs("\"routes\": [\n", g_out);
    for (i = 0; i < e->route_count; ++i)
    {
        const RouteStateInternal *r = &e->routes[i];
        size_t s;

        write_indent(depth + 1);
        fputs("{\n", g_out);
        write_indent(depth + 2);
        fprintf(g_out, "\"id\": %u,\n", (unsigned)r->id);
        write_indent(depth + 2);
        fputs("\"name\": ", g_out);
        write_string(r->name);
        fputs(",\n", g_out);
        write_indent(depth + 2);
        fprintf(g_out, "\"entrySignal\": %u, \"exitSignal\": %u,\n",
                (unsigned)r->entry_signal, (unsigned)r->exit_signal);
        write_indent(depth + 2);
        fputs("\"state\": ", g_out);
        write_string(route_state_name(r->state));
        fputs(",\n", g_out);
        write_indent(depth + 2);
        fprintf(g_out, "\"bookedBy\": %u,\n", (unsigned)r->booked_by);
        write_indent(depth + 2);
        fputs("\"steps\": [", g_out);
        for (s = 0; s < r->step_count; ++s)
        {
            fprintf(g_out, "%s{\"track\": %u, \"point\": %u, \"pointRequired\": ",
                    s ? ", " : "",
                    (unsigned)r->steps[s].track, (unsigned)r->steps[s].point);
            write_string(point_position_code(r->steps[s].point_required));
            fputs("}", g_out);
        }
        fputs("]\n", g_out);
        write_indent(depth + 1);
        fprintf(g_out, "}%s\n", (i + 1 < e->route_count) ? "," : "");
        counts[4]++;
    }
    write_indent(depth);
    fputs("]\n", g_out);
}

static void write_fleet_section(RailwayEngine *e, int depth, int *counts)
{
    size_t i;

    write_indent(depth);
    fputs("\"trains\": [\n", g_out);
    for (i = 0; i < e->train_count; ++i)
    {
        const TrainStateInternal *t = &e->trains[i];
        size_t c;
        int written = 0;

        write_indent(depth + 1);
        fputs("{\n", g_out);
        write_indent(depth + 2);
        fprintf(g_out, "\"id\": %u,\n", (unsigned)t->id);
        write_indent(depth + 2);
        fputs("\"headcode\": ", g_out);
        write_string(t->headcode);
        fputs(",\n", g_out);
        write_indent(depth + 2);
        fputs("\"description\": ", g_out);
        write_string(t->description);
        fputs(",\n", g_out);
        write_indent(depth + 2);
        fputs("\"class\": ", g_out);
        write_string(train_class_code(t->cls));
        fputs(",\n", g_out);
        write_indent(depth + 2);
        fprintf(g_out, "\"track\": %u, \"route\": %u,\n",
                (unsigned)t->track, (unsigned)t->route);
        write_indent(depth + 2);
        fprintf(g_out, "\"maxSpeedKph\": %.0f, \"speedKph\": %.1f,\n",
                (double)t->max_speed_kph, (double)t->speed_kph);
        write_indent(depth + 2);
        fprintf(g_out, "\"positionM\": %.1f, \"lengthM\": %u,\n",
                (double)t->position_m, t->length_m);
        write_indent(depth + 2);
        fputs("\"state\": ", g_out);
        write_string(train_state_name(t->state));
        fputs(",\n", g_out);
        write_indent(depth + 2);
        fprintf(g_out, "\"emergencyBrake\": %s, \"held\": %s, \"delaySeconds\": %u,\n",
                t->emergency_brake ? "true" : "false",
                t->held ? "true" : "false",
                t->delay_seconds);

        write_indent(depth + 2);
        fputs("\"cars\": [\n", g_out);
        for (c = 0; c < e->car_count; ++c)
        {
            const CarStateInternal *car = &e->cars[c];

            if (car->train != t->id)
            {
                continue;
            }
            write_indent(depth + 3);
            fputs("{\n", g_out);
            write_indent(depth + 4);
            fputs("\"number\": ", g_out);
            write_string(car->number);
            fputs(",\n", g_out);
            write_indent(depth + 4);
            fputs("\"type\": ", g_out);
            write_string(car_type_code(car->type));
            fputs(",\n", g_out);
            write_indent(depth + 4);
            fputs("\"designation\": ", g_out);
            write_string(car->designation);
            fputs(",\n", g_out);
            write_indent(depth + 4);
            fprintf(g_out, "\"lengthM\": %.1f, \"tareTonnes\": %.1f,\n",
                    (double)car->length_m, (double)car->tare_tonnes);
            write_indent(depth + 4);
            fprintf(g_out, "\"loadPercent\": %.0f, \"occupied\": %s,\n",
                    (double)car->load_percent,
                    car->occupied ? "true" : "false");
            write_indent(depth + 4);
            fprintf(g_out, "\"inService\": %s\n",
                    car->in_service ? "true" : "false");
            write_indent(depth + 3);
            fputs("}", g_out);
            written++;
            counts[6]++;

            /* A comma is needed unless this is the last car of this train. */
            {
                size_t look;
                bool more = false;
                for (look = c + 1; look < e->car_count; ++look)
                {
                    if (e->cars[look].train == t->id)
                    {
                        more = true;
                        break;
                    }
                }
                fputs(more ? ",\n" : "\n", g_out);
            }
        }
        write_indent(depth + 2);
        fputs("]\n", g_out);
        write_indent(depth + 1);
        fprintf(g_out, "}%s\n", (i + 1 < e->train_count) ? "," : "");
        counts[5]++;
        (void)written;
    }
    write_indent(depth);
    fputs("]\n", g_out);
}

RwResult rc_save_snapshot_scoped(const char *path, RcSaveScope scope,
                                 RcPersistResult *out)
{
    RailwayEngine *e = rw_engine();
    int counts[8] = {0};
    bool first_section = true;

    if (out != NULL)
    {
        memset(out, 0, sizeof(*out));
        if (path != NULL)
        {
            snprintf(out->path, sizeof(out->path), "%s", path);
        }
    }
    if (e == NULL || !e->initialized)
    {
        set_error("the engine is not initialised");
        return RW_ERR_NOT_INITIALIZED;
    }
    if (path == NULL || path[0] == '\0')
    {
        set_error("no path given");
        return RW_ERR_INVALID_ARG;
    }

    g_out = fopen(path, "w");
    if (g_out == NULL)
    {
        set_error("cannot open %s for writing", path);
        return RW_ERR_IO;
    }

    fputs("{\n", g_out);
    write_meta(1, e->name, "RailControl state snapshot");
    fputs(",\n", g_out);

    if (scope & RC_SAVE_LAYOUT)
    {
        write_indent(1);
        fputs("\"layout\": {\n", g_out);
        write_layout_section(e, 2, counts);
        write_indent(1);
        fputs("}", g_out);
        first_section = false;
    }
    if (scope & RC_SAVE_FLEET)
    {
        if (!first_section)
            fputs(",\n", g_out);
        write_indent(1);
        fputs("\"fleet\": {\n", g_out);
        write_fleet_section(e, 2, counts);
        write_indent(1);
        fputs("}", g_out);
        first_section = false;
    }
    if (scope & RC_SAVE_RUNNING)
    {
        if (!first_section)
            fputs(",\n", g_out);
        /* The running state is carried inside the layout and fleet sections -
         * aspects, occupancy and authority are fields of those objects, not a
         * separate list. This section records the engine-level counters so a
         * restore can confirm what it replaced. */
        write_indent(1);
        fputs("\"running\": {\n", g_out);
        write_indent(2);
        fprintf(g_out, "\"clockMs\": %lu,\n", e->clock_ms);
        write_indent(2);
        fprintf(g_out, "\"ticks\": %lu,\n", e->tick_count);
        write_indent(2);
        fprintf(g_out, "\"routeSets\": %lu,\n", e->route_sets);
        write_indent(2);
        fprintf(g_out, "\"conflictsBlocked\": %lu,\n", e->conflicts_blocked);
        write_indent(2);
        fprintf(g_out, "\"emergencyStops\": %lu,\n", e->emergency_stops);
        write_indent(2);
        fprintf(g_out, "\"emergency\": %s\n", e->emergency ? "true" : "false");
        write_indent(1);
        fputs("}", g_out);
        first_section = false;
    }

    fputs("\n}\n", g_out);
    fclose(g_out);
    g_out = NULL;

    if (out != NULL)
    {
        out->ok = true;
        out->tracks = counts[0];
        out->points = counts[1];
        out->signals = counts[2];
        out->sensors = counts[3];
        out->routes = counts[4];
        out->trains = counts[5];
        out->cars = counts[6];
        snprintf(out->message, sizeof(out->message),
                 "%d track(s), %d point(s), %d signal(s), %d sensor(s), "
                 "%d route(s), %d train(s), %d car(s)",
                 counts[0], counts[1], counts[2], counts[3],
                 counts[4], counts[5], counts[6]);
    }

    rw_event(SEVERITY_INFO, "PERSIST", "Snapshot saved to %s", path);
    return RW_OK;
}

RwResult rc_save_snapshot(const char *path, RcPersistResult *out)
{
    return rc_save_snapshot_scoped(path, RC_SAVE_STANDARD, out);
}

RwResult rc_save_layout(const char *path, RcPersistResult *out)
{
    return rc_save_snapshot_scoped(path, RC_SAVE_LAYOUT, out);
}

RwResult rc_save_fleet(const char *path, RcPersistResult *out)
{
    return rc_save_snapshot_scoped(path, RC_SAVE_FLEET, out);
}

/* ==========================================================================
 * Loading
 *
 * The shape of a load is the same for every section:
 *
 *     parse the whole file first   - nothing is touched yet
 *     validate every reference     - a bad id fails the load outright
 *     apply                        - only once the file is known good
 *
 * The validate-then-apply split is the point. Applying as we parse would leave
 * the engine half-changed when the tenth track turned out to name a point that
 * does not exist, and a half-changed layout is the one state an interlocking
 * must never be in.
 * ========================================================================== */

/* A parsed-but-not-yet-applied track. */
typedef struct
{
    RwId id;
    char name[RW_MAX_NAME];
    float lat1, lon1, lat2, lon2;
    int speed;
    float length;
    TrackOccupancy occupancy;
    RwId occupied_by;
    bool circuit_failed;
    bool in_service;
    RwId point_upstream;
    RwId point_downstream;
} ParsedTrack;

typedef struct
{
    RwId id;
    char name[RW_MAX_NAME];
    RwId track;
    SignalState aspect;
    bool at_exit;
    bool manual;
    bool in_service;
} ParsedSignal;

typedef struct
{
    RwId id;
    char name[RW_MAX_NAME];
    RwId track_upstream, track_normal, track_reverse;
    SwitchPosition position;
    float lat, lon;
    bool locked, fault, in_service;
} ParsedPoint;

typedef struct
{
    RwId id;
    char name[RW_MAX_NAME];
    RwSensorKind kind;
    RwId track, point;
    int value;
    bool in_service;
} ParsedSensor;

typedef struct
{
    char station[RW_MAX_NAME];
    int platform;
    char arrive[8];
    char depart[8];
    int dwell_seconds;
    char booking[RW_MAX_NAME];
} ParsedCall;

typedef struct
{
    char headcode[RW_MAX_NAME];
    char description[RC_PERSIST_MAX_TEXT];
    char service_class[RW_MAX_NAME];
    char origin[RW_MAX_NAME];
    char destination[RW_MAX_NAME];
    int priority;
    float planned_speed_kph;
    ParsedCall calls[RC_TIMETABLE_MAX_CALLS];
    int call_count;
} ParsedService;

/** Everything a load can carry, held together until it is known to be good. */
typedef struct
{
    ParsedTrack tracks[RW_MAX_TRACKS];
    int track_count;
    ParsedSignal signals[RW_MAX_SIGNALS];
    int signal_count;
    ParsedPoint points[RW_MAX_SWITCHES];
    int point_count;
    ParsedSensor sensors[RW_MAX_SENSORS];
    int sensor_count;

    ParsedService services[RC_TIMETABLE_MAX_SERVICES];
    int service_count;
    RcTimetablePlatform platforms[RC_TIMETABLE_MAX_PLATFORMS];
    int platform_count;
    RcTimetableRules rules;
    bool have_rules;
    char station[RW_MAX_NAME];
    char valid_from[16];
    char valid_to[16];

    /* Fleet. Trains are matched to the existing fleet by headcode rather than
     * by index: a snapshot taken after a train was added must still line up. */
    char train_headcode[RW_MAX_TRAINS][RW_MAX_NAME];
    float train_speed[RW_MAX_TRAINS];
    float train_position[RW_MAX_TRAINS];
    int train_route[RW_MAX_TRAINS];
    bool train_emergency[RW_MAX_TRAINS];
    bool train_held[RW_MAX_TRAINS];
    int train_count;
} ParsedSnapshot;

/* ---- section parsers ------------------------------------------------------ */

static void parse_track(JsonReader *r, ParsedTrack *out)
{
    bool first = true;

    memset(out, 0, sizeof(*out));
    out->in_service = true;
    out->occupancy = TRACK_CLEAR;

    if (!json_enter_object(r)) { json_fail(r, "a track entry is not an object"); return; }

    while (json_next_member(r, &first))
    {
        char key[64];
        json_string(r, key, sizeof(key));
        json_expect(r, ':');

        if (strcmp(key, "id") == 0)                 out->id = (RwId)json_number(r);
        else if (strcmp(key, "name") == 0)          json_string(r, out->name, sizeof(out->name));
        else if (strcmp(key, "lat1") == 0)          out->lat1 = (float)json_number(r);
        else if (strcmp(key, "lon1") == 0)          out->lon1 = (float)json_number(r);
        else if (strcmp(key, "lat2") == 0)          out->lat2 = (float)json_number(r);
        else if (strcmp(key, "lon2") == 0)          out->lon2 = (float)json_number(r);
        else if (strcmp(key, "speed") == 0)         out->speed = (int)json_number(r);
        else if (strcmp(key, "length") == 0)        out->length = (float)json_number(r);
        else if (strcmp(key, "occupancy") == 0)
        {
            char text[32]; json_string(r, text, sizeof(text));
            out->occupancy = occupancy_parse(text);
        }
        else if (strcmp(key, "occupiedBy") == 0)    out->occupied_by = (RwId)json_number(r);
        else if (strcmp(key, "circuitFailed") == 0) out->circuit_failed = json_bool(r);
        else if (strcmp(key, "inService") == 0)     out->in_service = json_bool(r);
        else if (strcmp(key, "pointUpstream") == 0) out->point_upstream = (RwId)json_number(r);
        else if (strcmp(key, "pointDownstream") == 0) out->point_downstream = (RwId)json_number(r);
        else json_skip_value(r);
    }
    json_expect(r, '}');
}

static void parse_point(JsonReader *r, ParsedPoint *out)
{
    bool first = true;

    memset(out, 0, sizeof(*out));
    out->in_service = true;

    if (!json_enter_object(r)) { json_fail(r, "a point entry is not an object"); return; }
    while (json_next_member(r, &first))
    {
        char key[64];
        json_string(r, key, sizeof(key));
        json_expect(r, ':');

        if (strcmp(key, "id") == 0)                 out->id = (RwId)json_number(r);
        else if (strcmp(key, "name") == 0)          json_string(r, out->name, sizeof(out->name));
        else if (strcmp(key, "trackUpstream") == 0) out->track_upstream = (RwId)json_number(r);
        else if (strcmp(key, "trackNormal") == 0)   out->track_normal = (RwId)json_number(r);
        else if (strcmp(key, "trackReverse") == 0)  out->track_reverse = (RwId)json_number(r);
        else if (strcmp(key, "position") == 0)
        {
            char text[32]; json_string(r, text, sizeof(text));
            out->position = point_position_parse(text);
        }
        else if (strcmp(key, "lat") == 0)           out->lat = (float)json_number(r);
        else if (strcmp(key, "lon") == 0)           out->lon = (float)json_number(r);
        else if (strcmp(key, "locked") == 0)        out->locked = json_bool(r);
        else if (strcmp(key, "fault") == 0)         out->fault = json_bool(r);
        else if (strcmp(key, "inService") == 0)     out->in_service = json_bool(r);
        else json_skip_value(r);
    }
    json_expect(r, '}');
}

static void parse_signal(JsonReader *r, ParsedSignal *out)
{
    bool first = true;

    memset(out, 0, sizeof(*out));
    out->in_service = true;
    out->aspect = SIGNAL_RED;

    if (!json_enter_object(r)) { json_fail(r, "a signal entry is not an object"); return; }
    while (json_next_member(r, &first))
    {
        char key[64];
        json_string(r, key, sizeof(key));
        json_expect(r, ':');

        if (strcmp(key, "id") == 0)             out->id = (RwId)json_number(r);
        else if (strcmp(key, "name") == 0)      json_string(r, out->name, sizeof(out->name));
        else if (strcmp(key, "track") == 0)     out->track = (RwId)json_number(r);
        else if (strcmp(key, "aspect") == 0)
        {
            char text[32]; json_string(r, text, sizeof(text));
            out->aspect = aspect_parse(text);
        }
        else if (strcmp(key, "atExit") == 0)    out->at_exit = json_bool(r);
        else if (strcmp(key, "manual") == 0)    out->manual = json_bool(r);
        else if (strcmp(key, "inService") == 0) out->in_service = json_bool(r);
        else json_skip_value(r);
    }
    json_expect(r, '}');
}

static void parse_sensor(JsonReader *r, ParsedSensor *out)
{
    bool first = true;

    memset(out, 0, sizeof(*out));
    out->in_service = true;

    if (!json_enter_object(r)) { json_fail(r, "a sensor entry is not an object"); return; }
    while (json_next_member(r, &first))
    {
        char key[64];
        json_string(r, key, sizeof(key));
        json_expect(r, ':');

        if (strcmp(key, "id") == 0)        out->id = (RwId)json_number(r);
        else if (strcmp(key, "name") == 0) json_string(r, out->name, sizeof(out->name));
        else if (strcmp(key, "kind") == 0)
        {
            char text[32]; json_string(r, text, sizeof(text));
            out->kind = sensor_kind_parse(text);
        }
        else if (strcmp(key, "track") == 0) out->track = (RwId)json_number(r);
        else if (strcmp(key, "point") == 0) out->point = (RwId)json_number(r);
        else if (strcmp(key, "value") == 0) out->value = (int)json_number(r);
        else if (strcmp(key, "inService") == 0) out->in_service = json_bool(r);
        else json_skip_value(r);
    }
    json_expect(r, '}');
}

static void parse_call(JsonReader *r, ParsedCall *out)
{
    bool first = true;

    memset(out, 0, sizeof(*out));
    if (!json_enter_object(r)) { json_fail(r, "a call entry is not an object"); return; }
    while (json_next_member(r, &first))
    {
        char key[64];
        json_string(r, key, sizeof(key));
        json_expect(r, ':');

        if (strcmp(key, "station") == 0)          json_string(r, out->station, sizeof(out->station));
        else if (strcmp(key, "platform") == 0)    out->platform = (int)json_number(r);
        else if (strcmp(key, "arrive") == 0)      json_string(r, out->arrive, sizeof(out->arrive));
        else if (strcmp(key, "depart") == 0)      json_string(r, out->depart, sizeof(out->depart));
        else if (strcmp(key, "dwellSeconds") == 0) out->dwell_seconds = (int)json_number(r);
        else if (strcmp(key, "booking") == 0)     json_string(r, out->booking, sizeof(out->booking));
        else json_skip_value(r);
    }
    json_expect(r, '}');
}

static void parse_service(JsonReader *r, ParsedService *out)
{
    bool first = true;

    memset(out, 0, sizeof(*out));
    if (!json_enter_object(r)) { json_fail(r, "a service entry is not an object"); return; }
    while (json_next_member(r, &first))
    {
        char key[64];
        json_string(r, key, sizeof(key));
        json_expect(r, ':');

        if (strcmp(key, "headcode") == 0)        json_string(r, out->headcode, sizeof(out->headcode));
        else if (strcmp(key, "description") == 0) json_string(r, out->description, sizeof(out->description));
        else if (strcmp(key, "class") == 0)      json_string(r, out->service_class, sizeof(out->service_class));
        else if (strcmp(key, "origin") == 0)     json_string(r, out->origin, sizeof(out->origin));
        else if (strcmp(key, "destination") == 0) json_string(r, out->destination, sizeof(out->destination));
        else if (strcmp(key, "priority") == 0)   out->priority = (int)json_number(r);
        else if (strcmp(key, "plannedSpeedKph") == 0) out->planned_speed_kph = (float)json_number(r);
        else if (strcmp(key, "calls") == 0)
        {
            bool first_call = true;
            if (!json_enter_array(r)) { json_fail(r, "calls is not an array"); return; }
            while (json_next_element(r, &first_call)
                   && out->call_count < RC_TIMETABLE_MAX_CALLS)
            {
                parse_call(r, &out->calls[out->call_count]);
                out->call_count++;
            }
            json_drain_array(r);
        }
        else json_skip_value(r);
    }
    json_expect(r, '}');
}

static void parse_platform(JsonReader *r, RcTimetablePlatform *out)
{
    bool first = true;

    memset(out, 0, sizeof(*out));
    if (!json_enter_object(r)) { json_fail(r, "a platform entry is not an object"); return; }
    while (json_next_member(r, &first))
    {
        char key[64];
        json_string(r, key, sizeof(key));
        json_expect(r, ':');

        if (strcmp(key, "id") == 0)          out->id = (int)json_number(r);
        else if (strcmp(key, "name") == 0)   json_string(r, out->name, sizeof(out->name));
        else if (strcmp(key, "track") == 0)  out->track = (RwId)json_number(r);
        else if (strcmp(key, "lengthM") == 0) out->length_m = (float)json_number(r);
        else if (strcmp(key, "direction") == 0) json_string(r, out->direction, sizeof(out->direction));
        else json_skip_value(r);
    }
    json_expect(r, '}');
}

static void parse_rules(JsonReader *r, RcTimetableRules *out)
{
    bool first = true;

    memset(out, 0, sizeof(*out));
    if (!json_enter_object(r)) { json_fail(r, "rules is not an object"); return; }
    while (json_next_member(r, &first))
    {
        char key[64];
        json_string(r, key, sizeof(key));
        json_expect(r, ':');

        if (strcmp(key, "minimumHeadwaySeconds") == 0) out->minimum_headway_seconds = (int)json_number(r);
        else if (strcmp(key, "platformReoccupationSeconds") == 0) out->platform_reoccupation_seconds = (int)json_number(r);
        else if (strcmp(key, "dwellToleranceSeconds") == 0) out->dwell_tolerance_seconds = (int)json_number(r);
        else if (strcmp(key, "lateThresholdSeconds") == 0) out->late_threshold_seconds = (int)json_number(r);
        else if (strcmp(key, "regulationStrategy") == 0) json_string(r, out->regulation_strategy, sizeof(out->regulation_strategy));
        else json_skip_value(r);
    }
    json_expect(r, '}');
}

static void parse_fleet_train(JsonReader *r, ParsedSnapshot *snap)
{
    bool first = true;
    int slot;

    if (snap->train_count >= RW_MAX_TRAINS)
    {
        json_skip_value(r);
        return;
    }
    slot = snap->train_count;

    memset(snap->train_headcode[slot], 0, RW_MAX_NAME);
    snap->train_speed[slot] = -1.0f;
    snap->train_position[slot] = -1.0f;
    snap->train_route[slot] = (int)RW_ID_NONE;
    snap->train_emergency[slot] = false;
    snap->train_held[slot] = false;

    if (!json_enter_object(r)) { json_fail(r, "a train entry is not an object"); return; }
    while (json_next_member(r, &first))
    {
        char key[64];
        json_string(r, key, sizeof(key));
        json_expect(r, ':');

        if (strcmp(key, "headcode") == 0)
        {
            json_string(r, snap->train_headcode[slot], RW_MAX_NAME);
        }
        else if (strcmp(key, "speedKph") == 0)        snap->train_speed[slot] = (float)json_number(r);
        else if (strcmp(key, "positionM") == 0)       snap->train_position[slot] = (float)json_number(r);
        else if (strcmp(key, "route") == 0)           snap->train_route[slot] = (int)json_number(r);
        else if (strcmp(key, "emergencyBrake") == 0)  snap->train_emergency[slot] = json_bool(r);
        else if (strcmp(key, "held") == 0)            snap->train_held[slot] = json_bool(r);
        else if (strcmp(key, "cars") == 0)
        {
            /* The formation is describe-only on load: rebuilding a train's
             * cars would change its length, and length drives occupancy. A
             * restore must not silently re-dimension a train. */
            json_skip_value(r);
        }
        else json_skip_value(r);
    }
    json_expect(r, '}');

    if (snap->train_headcode[slot][0] != '\0')
    {
        snap->train_count++;
    }
}

RwResult rc_persist_inspect(const char *path, RcPersistResult *out)
{
    FILE *file;
    char *buffer;
    long size;
    JsonReader r;
    bool first = true;
    int tracks = 0, points = 0, signals = 0, sensors = 0, trains = 0, services = 0;

    if (out != NULL)
    {
        memset(out, 0, sizeof(*out));
        if (path != NULL) snprintf(out->path, sizeof(out->path), "%s", path);
    }
    if (path == NULL || path[0] == '\0')
    {
        set_error("no path given");
        return RW_ERR_INVALID_ARG;
    }

    file = fopen(path, "rb");
    if (file == NULL)
    {
        set_error("cannot open %s", path);
        return RW_ERR_IO;
    }
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size <= 0 || size > 8 * 1024 * 1024)
    {
        fclose(file);
        set_error("%s is empty or implausibly large", path);
        return RW_ERR_IO;
    }
    buffer = (char *)malloc((size_t)size + 1);
    if (buffer == NULL)
    {
        fclose(file);
        set_error("out of memory reading %s", path);
        return RW_ERR_INTERNAL;
    }
    if (fread(buffer, 1, (size_t)size, file) != (size_t)size)
    {
        free(buffer);
        fclose(file);
        set_error("short read on %s", path);
        return RW_ERR_IO;
    }
    fclose(file);
    buffer[size] = '\0';

    /* Count array elements without applying anything. This is what lets a
     * caller show "this file has 14 tracks and 5 trains" before committing. */
    {
        const char *p = buffer;
        for (; *p != '\0'; ++p)
        {
            if (strncmp(p, "\"tracks\"", 8) == 0
                || strncmp(p, "\"points\"", 8) == 0
                || strncmp(p, "\"signals\"", 9) == 0
                || strncmp(p, "\"sensors\"", 9) == 0
                || strncmp(p, "\"trains\"", 8) == 0
                || strncmp(p, "\"services\"", 10) == 0)
            {
                const char *q = strchr(p, '[');
                int depth = 0;
                int *target = NULL;

                if (q == NULL) continue;
                if (strncmp(p, "\"tracks\"", 8) == 0)        target = &tracks;
                else if (strncmp(p, "\"points\"", 8) == 0)   target = &points;
                else if (strncmp(p, "\"signals\"", 9) == 0)  target = &signals;
                else if (strncmp(p, "\"sensors\"", 9) == 0)  target = &sensors;
                else if (strncmp(p, "\"trains\"", 8) == 0)   target = &trains;
                else if (strncmp(p, "\"services\"", 10) == 0) target = &services;

                for (; *q != '\0'; ++q)
                {
                    if (*q == '{') { depth++; if (depth == 1) (*target)++; }
                    else if (*q == '}') depth--;
                    else if (*q == ']' && depth == 0) break;
                }
            }
        }
    }

    memset(&r, 0, sizeof(r));
    r.cursor = buffer;
    r.end = buffer + size;
    (void)json_enter_object(&r);

    free(buffer);

    if (out != NULL)
    {
        out->ok = true;
        out->tracks = tracks;
        out->points = points;
        out->signals = signals;
        out->sensors = sensors;
        out->trains = trains;
        snprintf(out->message, sizeof(out->message),
                 "%d track(s), %d point(s), %d signal(s), %d sensor(s), "
                 "%d train(s), %d timetable service(s)",
                 tracks, points, signals, sensors, trains, services);
    }
    (void)first;
    return RW_OK;
}

/* --------------------------------------------------------------------------
 * Full parse of a snapshot file
 * -------------------------------------------------------------------------- */
static RwResult parse_snapshot_file(const char *path, ParsedSnapshot *snap)
{
    FILE *file;
    char *buffer;
    long size;
    JsonReader r;
    bool first = true;

    memset(snap, 0, sizeof(*snap));

    file = fopen(path, "rb");
    if (file == NULL)
    {
        set_error("cannot open %s", path);
        return RW_ERR_IO;
    }
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size <= 0)
    {
        fclose(file);
        set_error("%s is empty", path);
        return RW_ERR_IO;
    }
    buffer = (char *)malloc((size_t)size + 1);
    if (buffer == NULL)
    {
        fclose(file);
        set_error("out of memory reading %s", path);
        return RW_ERR_INTERNAL;
    }
    if (fread(buffer, 1, (size_t)size, file) != (size_t)size)
    {
        free(buffer);
        fclose(file);
        set_error("short read on %s", path);
        return RW_ERR_IO;
    }
    fclose(file);
    buffer[size] = '\0';

    memset(&r, 0, sizeof(r));
    r.cursor = buffer;
    r.end = buffer + size;

    if (!json_enter_object(&r))
    {
        free(buffer);
        set_error("%s is not a JSON object", path);
        return RW_ERR_INVALID_ARG;
    }

    while (json_next_member(&r, &first))
    {
        char key[64];
        json_string(&r, key, sizeof(key));
        json_expect(&r, ':');

        /* The meta block is informational; skip it. */
        if (strcmp(key, "meta") == 0)
        {
            json_skip_value(&r);
        }
        /* The timetable file puts its arrays at the top level rather than
         * nested under a section, so accept that shape too. One loader, both
         * spellings - the alternative is a second parser that drifts. */
        else if (strcmp(key, "services") == 0)
        {
            bool first_elem = true;
            if (!json_enter_array(&r)) { json_fail(&r, "services is not an array"); break; }
            while (json_next_element(&r, &first_elem))
            {
                if (snap->service_count < RC_TIMETABLE_MAX_SERVICES)
                {
                    parse_service(&r, &snap->services[snap->service_count]);
                    snap->service_count++;
                }
                else json_skip_value(&r);
            }
            json_drain_array(&r);
            json_accept(&r, ']');
        }
        else if (strcmp(key, "platforms") == 0)
        {
            bool first_elem = true;
            if (!json_enter_array(&r)) { json_fail(&r, "platforms is not an array"); break; }
            while (json_next_element(&r, &first_elem))
            {
                if (snap->platform_count < RC_TIMETABLE_MAX_PLATFORMS)
                {
                    parse_platform(&r, &snap->platforms[snap->platform_count]);
                    snap->platform_count++;
                }
                else json_skip_value(&r);
            }
            json_drain_array(&r);
            json_accept(&r, ']');
        }
        else if (strcmp(key, "rules") == 0)
        {
            parse_rules(&r, &snap->rules);
            snap->have_rules = true;
        }
        else if (strcmp(key, "station") == 0)
        {
            json_string(&r, snap->station, sizeof(snap->station));
        }
        else if (strcmp(key, "validFrom") == 0)
        {
            json_string(&r, snap->valid_from, sizeof(snap->valid_from));
        }
        else if (strcmp(key, "validTo") == 0)
        {
            json_string(&r, snap->valid_to, sizeof(snap->valid_to));
        }
        else if (strcmp(key, "layout") == 0 || strcmp(key, "timetable") == 0)
        {
            bool first_inner = true;

            if (!json_enter_object(&r))
            {
                json_fail(&r, "layout is not an object");
                break;
            }
            while (json_next_member(&r, &first_inner))
            {
                char inner[64];
                bool first_elem = true;

                json_string(&r, inner, sizeof(inner));
                json_expect(&r, ':');

                if (strcmp(inner, "tracks") == 0)
                {
                    if (!json_enter_array(&r)) { json_fail(&r, "tracks is not an array"); break; }
                    while (json_next_element(&r, &first_elem))
                    {
                        if (snap->track_count < RW_MAX_TRACKS)
                        {
                            parse_track(&r, &snap->tracks[snap->track_count]);
                            snap->track_count++;
                        }
                        else json_skip_value(&r);
                    }
                    json_drain_array(&r);
                    json_accept(&r, ']');
                }
                else if (strcmp(inner, "points") == 0)
                {
                    if (!json_enter_array(&r)) { json_fail(&r, "points is not an array"); break; }
                    while (json_next_element(&r, &first_elem))
                    {
                        if (snap->point_count < RW_MAX_SWITCHES)
                        {
                            parse_point(&r, &snap->points[snap->point_count]);
                            snap->point_count++;
                        }
                        else json_skip_value(&r);
                    }
                    json_drain_array(&r);
                    json_accept(&r, ']');
                }
                else if (strcmp(inner, "signals") == 0)
                {
                    if (!json_enter_array(&r)) { json_fail(&r, "signals is not an array"); break; }
                    while (json_next_element(&r, &first_elem))
                    {
                        if (snap->signal_count < RW_MAX_SIGNALS)
                        {
                            parse_signal(&r, &snap->signals[snap->signal_count]);
                            snap->signal_count++;
                        }
                        else json_skip_value(&r);
                    }
                    json_drain_array(&r);
                    json_accept(&r, ']');
                }
                else if (strcmp(inner, "sensors") == 0)
                {
                    if (!json_enter_array(&r)) { json_fail(&r, "sensors is not an array"); break; }
                    while (json_next_element(&r, &first_elem))
                    {
                        if (snap->sensor_count < RW_MAX_SENSORS)
                        {
                            parse_sensor(&r, &snap->sensors[snap->sensor_count]);
                            snap->sensor_count++;
                        }
                        else json_skip_value(&r);
                    }
                    json_drain_array(&r);
                    json_accept(&r, ']');
                }
                else if (strcmp(inner, "services") == 0)
                {
                    if (!json_enter_array(&r)) { json_fail(&r, "services is not an array"); break; }
                    while (json_next_element(&r, &first_elem))
                    {
                        if (snap->service_count < RC_TIMETABLE_MAX_SERVICES)
                        {
                            parse_service(&r, &snap->services[snap->service_count]);
                            snap->service_count++;
                        }
                        else json_skip_value(&r);
                    }
                    json_drain_array(&r);
                    json_accept(&r, ']');
                }
                else if (strcmp(inner, "platforms") == 0)
                {
                    if (!json_enter_array(&r)) { json_fail(&r, "platforms is not an array"); break; }
                    while (json_next_element(&r, &first_elem))
                    {
                        if (snap->platform_count < RC_TIMETABLE_MAX_PLATFORMS)
                        {
                            parse_platform(&r, &snap->platforms[snap->platform_count]);
                            snap->platform_count++;
                        }
                        else json_skip_value(&r);
                    }
                    json_drain_array(&r);
                    json_accept(&r, ']');
                }
                else if (strcmp(inner, "rules") == 0)
                {
                    parse_rules(&r, &snap->rules);
                    snap->have_rules = true;
                }
                else if (strcmp(inner, "station") == 0)
                {
                    json_string(&r, snap->station, sizeof(snap->station));
                }
                else if (strcmp(inner, "validFrom") == 0)
                {
                    json_string(&r, snap->valid_from, sizeof(snap->valid_from));
                }
                else if (strcmp(inner, "validTo") == 0)
                {
                    json_string(&r, snap->valid_to, sizeof(snap->valid_to));
                }
                else json_skip_value(&r);
            }
            if (r.failed) break;
            json_expect(&r, '}');
        }
        else if (strcmp(key, "fleet") == 0)
        {
            bool first_inner = true;

            if (!json_enter_object(&r)) { json_fail(&r, "fleet is not an object"); break; }
            while (json_next_member(&r, &first_inner))
            {
                char inner[64];
                bool first_elem = true;

                json_string(&r, inner, sizeof(inner));
                json_expect(&r, ':');

                if (strcmp(inner, "trains") == 0)
                {
                    if (!json_enter_array(&r)) { json_fail(&r, "trains is not an array"); break; }
                    while (json_next_element(&r, &first_elem))
                    {
                        parse_fleet_train(&r, snap);
                    }
                    json_drain_array(&r);
                    json_accept(&r, ']');
                }
                else json_skip_value(&r);
            }
            if (r.failed) break;
            json_expect(&r, '}');
        }
        else if (strcmp(key, "running") == 0)
        {
            json_skip_value(&r);
        }
        else
        {
            json_skip_value(&r);
        }
    }

    free(buffer);

    if (r.failed)
    {
        set_error("JSON error in %s: %s", path, r.error);
        return RW_ERR_INVALID_ARG;
    }

    /* A file that parsed to nothing is not a valid snapshot. Without this
     * check a typo'd or truncated file would be accepted as "loaded" and the
     * operator would be told everything was fine while nothing had changed -
     * which is the failure the old stub had, reintroduced through the parser. */
    if (snap->track_count == 0 && snap->signal_count == 0
        && snap->point_count == 0 && snap->sensor_count == 0
        && snap->train_count == 0 && snap->service_count == 0)
    {
        set_error("%s contains no layout, fleet or timetable data", path);
        return RW_ERR_INVALID_ARG;
    }
    return RW_OK;
}

/* --------------------------------------------------------------------------
 * Validation
 *
 * Every reference in the file is checked against the engine before anything is
 * applied. A snapshot naming a track that does not exist is refused whole.
 * -------------------------------------------------------------------------- */
static RwResult validate_snapshot(const ParsedSnapshot *snap, RcSaveScope scope)
{
    RailwayEngine *e = rw_engine();
    int i;

    if (scope & RC_SAVE_LAYOUT)
    {
        /* The layout being replaced is the engine's own, so the check is that
         * the file is internally consistent: ids in range, names present. */
        for (i = 0; i < snap->track_count; ++i)
        {
            if (snap->tracks[i].id == RW_ID_NONE || snap->tracks[i].id > RW_MAX_TRACKS)
            {
                set_error("track %d has an id outside 1..%d", i + 1, RW_MAX_TRACKS);
                return RW_ERR_INVALID_ARG;
            }
            if (snap->tracks[i].name[0] == '\0')
            {
                set_error("track %d has no name", snap->tracks[i].id);
                return RW_ERR_INVALID_ARG;
            }
            if (snap->tracks[i].length <= 0.0f)
            {
                set_error("track %s has no length", snap->tracks[i].name);
                return RW_ERR_INVALID_ARG;
            }
        }
        for (i = 0; i < snap->signal_count; ++i)
        {
            if (snap->signals[i].name[0] == '\0')
            {
                set_error("signal %d has no name", i + 1);
                return RW_ERR_INVALID_ARG;
            }
        }
        for (i = 0; i < snap->sensor_count; ++i)
        {
            if (snap->sensors[i].name[0] == '\0')
            {
                set_error("sensor %d has no name", i + 1);
                return RW_ERR_INVALID_ARG;
            }
        }
        /* A point must reference tracks that exist in the file. These are the
         * references that matter: a point wired to a non-existent track would
         * make a route unprovable in a way that is very hard to diagnose. */
        for (i = 0; i < snap->point_count; ++i)
        {
            const ParsedPoint *p = &snap->points[i];
            const char *role = NULL;
            RwId ref = RW_ID_NONE;

            if (p->track_normal != RW_ID_NONE
                && p->track_normal > (RwId)snap->track_count)
            {
                role = "normal"; ref = p->track_normal;
            }
            else if (p->track_reverse != RW_ID_NONE
                     && p->track_reverse > (RwId)snap->track_count)
            {
                role = "reverse"; ref = p->track_reverse;
            }
            if (role != NULL)
            {
                set_error("point %s routes to track %u, which the file does not contain",
                          p->name, (unsigned)ref);
                return RW_ERR_INVALID_ARG;
            }
        }
    }

    if (scope & RC_SAVE_FLEET)
    {
        for (i = 0; i < snap->train_count; ++i)
        {
            if (snap->train_headcode[i][0] == '\0')
            {
                set_error("a fleet entry has no headcode");
                return RW_ERR_INVALID_ARG;
            }
        }
    }
    (void)e;
    return RW_OK;
}

/* --------------------------------------------------------------------------
 * Apply
 * -------------------------------------------------------------------------- */
static void apply_layout(const ParsedSnapshot *snap, RcPersistResult *out)
{
    RailwayEngine *e = rw_engine();
    int i;

    /* Tracks. The engine's occupancy is derived from track circuits and train
     * positions on the next tick, so a restored occupancy is a starting point
     * rather than the final word - which is correct: the trains are restored
     * too, and the two are reconciled by the controllers. */
    e->track_count = 0;
    for (i = 0; i < snap->track_count && e->track_count < RW_MAX_TRACKS; ++i)
    {
        TrackState *t = &e->tracks[e->track_count];
        const ParsedTrack *p = &snap->tracks[i];

        memset(t, 0, sizeof(*t));
        t->id = p->id;
        snprintf(t->name, sizeof(t->name), "%s", p->name);
        t->start_lat = p->lat1; t->start_lon = p->lon1;
        t->end_lat = p->lat2;   t->end_lon = p->lon2;
        t->speed_limit_kph = p->speed > 0 ? p->speed : 80;
        t->length_m = p->length;
        t->occupancy = p->occupancy;
        t->occupied_by = p->occupied_by;
        t->circuit_failed = p->circuit_failed;
        t->in_service = p->in_service;
        t->point_upstream = p->point_upstream;
        t->point_downstream = p->point_downstream;
        e->track_count++;
        out->tracks++;
    }

    e->point_count = 0;
    for (i = 0; i < snap->point_count && e->point_count < RW_MAX_SWITCHES; ++i)
    {
        PointState *pt = &e->points[e->point_count];
        const ParsedPoint *p = &snap->points[i];

        memset(pt, 0, sizeof(*pt));
        pt->id = p->id;
        snprintf(pt->name, sizeof(pt->name), "%s", p->name);
        pt->track_upstream = p->track_upstream;
        pt->track_normal = p->track_normal;
        pt->track_reverse = p->track_reverse;
        pt->position = p->position;
        pt->commanded = p->position;
        pt->latitude = p->lat;
        pt->longitude = p->lon;
        pt->locked = p->locked;
        pt->fault = p->fault;
        pt->in_service = p->in_service;
        e->point_count++;
        out->points++;
    }

    e->signal_count = 0;
    for (i = 0; i < snap->signal_count && e->signal_count < RW_MAX_SIGNALS; ++i)
    {
        SignalStateInternal *s = &e->signals[e->signal_count];
        const ParsedSignal *p = &snap->signals[i];

        memset(s, 0, sizeof(*s));
        s->id = p->id;
        snprintf(s->name, sizeof(s->name), "%s", p->name);
        s->track = p->track;
        s->aspect = p->aspect;
        s->commanded = p->aspect;
        s->at_exit = p->at_exit;
        s->manual = p->manual;
        s->in_service = p->in_service;
        e->signal_count++;
        out->signals++;
    }

    e->sensor_count = 0;
    for (i = 0; i < snap->sensor_count && e->sensor_count < RW_MAX_SENSORS; ++i)
    {
        SensorState *s = &e->sensors[e->sensor_count];
        const ParsedSensor *p = &snap->sensors[i];

        memset(s, 0, sizeof(*s));
        s->id = p->id;
        snprintf(s->name, sizeof(s->name), "%s", p->name);
        s->kind = p->kind;
        s->track = p->track;
        s->point = p->point;
        s->value = p->value;
        s->in_service = p->in_service;
        e->sensor_count++;
        out->sensors++;
    }
}

static void apply_fleet(const ParsedSnapshot *snap, RcPersistResult *out)
{
    RailwayEngine *e = rw_engine();
    int i;

    /* Match by headcode, not by index: a train added since the snapshot was
     * taken must not shift every later train onto the wrong state. */
    for (i = 0; i < snap->train_count; ++i)
    {
        RwId id = rw_train_find(snap->train_headcode[i]);
        TrainStateInternal *t;

        if (id == RW_ID_NONE || id > e->train_count)
        {
            continue;
        }
        t = &e->trains[id - 1];

        if (snap->train_speed[i] >= 0.0f)
        {
            t->speed_kph = snap->train_speed[i];
        }
        if (snap->train_position[i] >= 0.0f)
        {
            t->position_m = snap->train_position[i];
        }
        t->route = (snap->train_route[i] > 0) ? (RwId)snap->train_route[i] : RW_ID_NONE;
        t->emergency_brake = snap->train_emergency[i];
        t->held = snap->train_held[i];
        out->trains++;
    }
}

static void apply_timetable(const ParsedSnapshot *snap, RcPersistResult *out)
{
    RcTimetable *tt = rc_timetable();
    int i;

    if (tt == NULL || snap->service_count == 0)
    {
        return;
    }

    memset(tt, 0, sizeof(*tt));
    snprintf(tt->station, sizeof(tt->station), "%s",
             snap->station[0] ? snap->station : "UNKNOWN");
    snprintf(tt->valid_from, sizeof(tt->valid_from), "%s", snap->valid_from);
    snprintf(tt->valid_to, sizeof(tt->valid_to), "%s", snap->valid_to);

    for (i = 0; i < snap->platform_count && tt->platform_count < RC_TIMETABLE_MAX_PLATFORMS; ++i)
    {
        tt->platforms[tt->platform_count++] = snap->platforms[i];
    }

    for (i = 0; i < snap->service_count && tt->service_count < RC_TIMETABLE_MAX_SERVICES; ++i)
    {
        RcTimetableService *svc = &tt->services[tt->service_count];
        const ParsedService *p = &snap->services[i];
        int c;

        memset(svc, 0, sizeof(*svc));
        snprintf(svc->headcode, sizeof(svc->headcode), "%s", p->headcode);
        snprintf(svc->description, sizeof(svc->description), "%s", p->description);
        snprintf(svc->service_class, sizeof(svc->service_class), "%s", p->service_class);
        snprintf(svc->origin, sizeof(svc->origin), "%s", p->origin);
        snprintf(svc->destination, sizeof(svc->destination), "%s", p->destination);
        svc->priority = p->priority;
        svc->planned_speed_kph = p->planned_speed_kph;

        for (c = 0; c < p->call_count && c < RC_TIMETABLE_MAX_CALLS; ++c)
        {
            RcTimetableCall *call = &svc->calls[svc->call_count];

            snprintf(call->station, sizeof(call->station), "%s", p->calls[c].station);
            call->platform = p->calls[c].platform;
            snprintf(call->arrive, sizeof(call->arrive), "%s", p->calls[c].arrive);
            snprintf(call->depart, sizeof(call->depart), "%s", p->calls[c].depart);
            call->dwell_seconds = p->calls[c].dwell_seconds;
            snprintf(call->booking, sizeof(call->booking), "%s", p->calls[c].booking);
            svc->call_count++;
        }
        tt->service_count++;
    }

    if (snap->have_rules)
    {
        tt->rules = snap->rules;
    }
    out->routes = tt->service_count;   /* reuse the slot for a non-zero count */
}

RwResult rc_load_snapshot(const char *path, RcSaveScope scope, RcPersistResult *out)
{
    ParsedSnapshot snap;
    RcPersistResult result;
    RwResult outcome;

    if (out == NULL)
    {
        out = &result;
    }
    memset(out, 0, sizeof(*out));
    if (path != NULL)
    {
        snprintf(out->path, sizeof(out->path), "%s", path);
    }

    if (path == NULL || path[0] == '\0')
    {
        set_error("no path given");
        return RW_ERR_INVALID_ARG;
    }
    if (!rw_engine_ready())
    {
        set_error("the engine is not initialised");
        return RW_ERR_NOT_INITIALIZED;
    }

    outcome = parse_snapshot_file(path, &snap);
    if (outcome != RW_OK)
    {
        return outcome;
    }

    outcome = validate_snapshot(&snap, scope);
    if (outcome != RW_OK)
    {
        return outcome;
    }

    /* Nothing has been touched until this point. */
    if (scope & RC_SAVE_LAYOUT)
    {
        apply_layout(&snap, out);
    }
    if (scope & RC_SAVE_FLEET)
    {
        apply_fleet(&snap, out);
    }
    if (scope & RC_SAVE_TIMETABLE)
    {
        apply_timetable(&snap, out);
    }

    out->ok = true;
    snprintf(out->message, sizeof(out->message),
             "%d track(s), %d point(s), %d signal(s), %d sensor(s), %d train(s) restored",
             out->tracks, out->points, out->signals, out->sensors, out->trains);

    rw_bump_revision();
    rw_event(SEVERITY_INFO, "PERSIST", "Snapshot loaded from %s - %s",
             path, out->message);
    return RW_OK;
}

RwResult rc_load_layout(const char *path, RcPersistResult *out)
{
    return rc_load_snapshot(path, RC_SAVE_LAYOUT, out);
}

RwResult rc_load_fleet(const char *path, RcPersistResult *out)
{
    return rc_load_snapshot(path, RC_SAVE_FLEET, out);
}

/* ==========================================================================
 * Timetable
 * ========================================================================== */
static RcTimetable g_timetable;
static bool g_timetable_loaded = false;

RcTimetable *rc_timetable(void)
{
    return &g_timetable;
}

void rc_timetable_clear(void)
{
    memset(&g_timetable, 0, sizeof(g_timetable));
    g_timetable_loaded = false;
}

int rc_timetable_find(const char *headcode)
{
    int i;

    if (headcode == NULL)
    {
        return -1;
    }
    for (i = 0; i < g_timetable.service_count; ++i)
    {
        if (strcmp(g_timetable.services[i].headcode, headcode) == 0)
        {
            return i;
        }
    }
    return -1;
}

RwResult rc_timetable_load(const char *path, RcPersistResult *out)
{
    ParsedSnapshot snap;
    RcPersistResult result;
    RwResult outcome;

    if (out == NULL)
    {
        out = &result;
    }
    memset(out, 0, sizeof(*out));
    if (path != NULL)
    {
        snprintf(out->path, sizeof(out->path), "%s", path);
    }

    outcome = parse_snapshot_file(path, &snap);
    if (outcome != RW_OK)
    {
        return outcome;
    }
    if (snap.service_count == 0)
    {
        set_error("%s contains no timetable services", path);
        return RW_ERR_INVALID_ARG;
    }

    apply_timetable(&snap, out);
    g_timetable_loaded = true;
    out->ok = true;
    snprintf(out->message, sizeof(out->message),
             "%d service(s) over %d platform(s) at %s",
             g_timetable.service_count, g_timetable.platform_count,
             g_timetable.station);

    rw_event(SEVERITY_INFO, "TIMETABLE", "Working timetable loaded from %s - %s",
             path, out->message);
    return RW_OK;
}

RwResult rc_timetable_save(const char *path, RcPersistResult *out)
{
    RcPersistResult result;
    int i;
    int c;

    if (out == NULL)
    {
        out = &result;
    }
    memset(out, 0, sizeof(*out));
    if (path != NULL)
    {
        snprintf(out->path, sizeof(out->path), "%s", path);
    }

    if (path == NULL || path[0] == '\0')
    {
        set_error("no path given");
        return RW_ERR_INVALID_ARG;
    }

    g_out = fopen(path, "w");
    if (g_out == NULL)
    {
        set_error("cannot open %s for writing", path);
        return RW_ERR_IO;
    }

    fputs("{\n", g_out);
    write_meta(1, g_timetable.station[0] ? g_timetable.station : "timetable",
               "RailControl working timetable");
    fputs(",\n", g_out);

    write_indent(1); fputs("\"timetable\": {\n", g_out);
    write_indent(2); fputs("\"station\": ", g_out);
    write_string(g_timetable.station); fputs(",\n", g_out);
    write_indent(2); fputs("\"validFrom\": ", g_out);
    write_string(g_timetable.valid_from); fputs(",\n", g_out);
    write_indent(2); fputs("\"validTo\": ", g_out);
    write_string(g_timetable.valid_to); fputs(",\n", g_out);

    write_indent(2); fputs("\"platforms\": [\n", g_out);
    for (i = 0; i < g_timetable.platform_count; ++i)
    {
        const RcTimetablePlatform *p = &g_timetable.platforms[i];
        write_indent(3); fprintf(g_out, "{\"id\": %d, \"name\": ", p->id);
        write_string(p->name);
        fprintf(g_out, ", \"track\": %u, \"lengthM\": %.0f, \"direction\": ",
                (unsigned)p->track, (double)p->length_m);
        write_string(p->direction);
        fprintf(g_out, "}%s\n", (i + 1 < g_timetable.platform_count) ? "," : "");
    }
    write_indent(2); fputs("],\n", g_out);

    write_indent(2); fputs("\"services\": [\n", g_out);
    for (i = 0; i < g_timetable.service_count; ++i)
    {
        const RcTimetableService *s = &g_timetable.services[i];

        write_indent(3); fputs("{\n", g_out);
        write_indent(4); fputs("\"headcode\": ", g_out); write_string(s->headcode); fputs(",\n", g_out);
        write_indent(4); fputs("\"description\": ", g_out); write_string(s->description); fputs(",\n", g_out);
        write_indent(4); fputs("\"class\": ", g_out); write_string(s->service_class); fputs(",\n", g_out);
        write_indent(4); fputs("\"origin\": ", g_out); write_string(s->origin); fputs(",\n", g_out);
        write_indent(4); fputs("\"destination\": ", g_out); write_string(s->destination); fputs(",\n", g_out);
        write_indent(4); fprintf(g_out, "\"priority\": %d,\n", s->priority);
        write_indent(4); fprintf(g_out, "\"plannedSpeedKph\": %.0f,\n", (double)s->planned_speed_kph);

        write_indent(4); fputs("\"calls\": [\n", g_out);
        for (c = 0; c < s->call_count; ++c)
        {
            const RcTimetableCall *call = &s->calls[c];
            write_indent(5); fputs("{\"station\": ", g_out); write_string(call->station);
            fprintf(g_out, ", \"platform\": %d, \"arrive\": ", call->platform);
            write_string(call->arrive);
            fputs(", \"depart\": ", g_out); write_string(call->depart);
            fprintf(g_out, ", \"dwellSeconds\": %d, \"booking\": ", call->dwell_seconds);
            write_string(call->booking);
            fprintf(g_out, "}%s\n", (c + 1 < s->call_count) ? "," : "");
        }
        write_indent(4); fputs("]\n", g_out);
        write_indent(3); fprintf(g_out, "}%s\n", (i + 1 < g_timetable.service_count) ? "," : "");
    }
    write_indent(2); fputs("],\n", g_out);

    write_indent(2); fputs("\"rules\": {\n", g_out);
    write_indent(3); fprintf(g_out, "\"minimumHeadwaySeconds\": %d,\n", g_timetable.rules.minimum_headway_seconds);
    write_indent(3); fprintf(g_out, "\"platformReoccupationSeconds\": %d,\n", g_timetable.rules.platform_reoccupation_seconds);
    write_indent(3); fprintf(g_out, "\"dwellToleranceSeconds\": %d,\n", g_timetable.rules.dwell_tolerance_seconds);
    write_indent(3); fprintf(g_out, "\"lateThresholdSeconds\": %d,\n", g_timetable.rules.late_threshold_seconds);
    write_indent(3); fputs("\"regulationStrategy\": ", g_out);
    write_string(g_timetable.rules.regulation_strategy); fputs("\n", g_out);
    write_indent(2); fputs("}\n", g_out);

    write_indent(1); fputs("}\n", g_out);
    fputs("}\n", g_out);

    fclose(g_out);
    g_out = NULL;

    out->ok = true;
    out->routes = g_timetable.service_count;
    snprintf(out->message, sizeof(out->message), "%d service(s) written",
             g_timetable.service_count);
    return RW_OK;
}

RwResult rc_timetable_describe(int index, char *buffer, size_t size)
{
    const RcTimetableService *s;

    if (buffer == NULL || size == 0)
    {
        return RW_ERR_INVALID_ARG;
    }
    buffer[0] = '\0';
    if (index < 0 || index >= g_timetable.service_count)
    {
        return RW_ERR_INVALID_ID;
    }
    s = &g_timetable.services[index];

    if (s->call_count > 0)
    {
        const RcTimetableCall *call = &s->calls[0];
        snprintf(buffer, size, "%-6s arr %s  dep %s  plat %-2d  %s",
                 s->headcode, call->arrive, call->depart,
                 call->platform, s->description);
    }
    else
    {
        snprintf(buffer, size, "%-6s %s", s->headcode, s->description);
    }
    return RW_OK;
}

RwResult rc_timetable_format(char *buffer, size_t size)
{
    size_t used = 0;
    int i;

    if (buffer == NULL || size == 0)
    {
        return RW_ERR_INVALID_ARG;
    }
    buffer[0] = '\0';

    if (g_timetable.service_count == 0)
    {
        snprintf(buffer, size,
                 "No working timetable loaded.\n"
                 "Load one with:  LOAD TIMETABLE data/timetable.json\n");
        return RW_OK;
    }

    used += (size_t)snprintf(buffer + used, size - used,
                             "WORKING TIMETABLE - %s (%s to %s)\n",
                             g_timetable.station, g_timetable.valid_from,
                             g_timetable.valid_to);
    used += (size_t)snprintf(buffer + used, size - used,
                             "%-7s %-6s %-6s %-5s %-6s %s\n",
                             "HEAD", "ARR", "DEP", "PLAT", "PRIO", "DESCRIPTION");

    for (i = 0; i < g_timetable.service_count && used < size; ++i)
    {
        const RcTimetableService *s = &g_timetable.services[i];
        const RcTimetableCall *call = (s->call_count > 0) ? &s->calls[0] : NULL;

        used += (size_t)snprintf(buffer + used, size - used,
                                 "%-7s %-6s %-6s %-5d %-6d %s\n",
                                 s->headcode,
                                 call ? call->arrive : "--:--",
                                 call ? call->depart : "--:--",
                                 call ? call->platform : 0,
                                 s->priority, s->description);
    }
    (void)g_timetable_loaded;
    return RW_OK;
}
