/* ==========================================================================
 * rc_settings.c - Program identity, invariant text and the Settings store.
 * ========================================================================== */
#include "railcontrol.h"
#include "railway_types.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Safety invariants
 * -------------------------------------------------------------------------- */
const char *rc_invariant_text(enum RcInvariant invariant)
{
    switch (invariant) {
    case RC_INV_SIGNAL_REQUIRES_ROUTE:
        return "A signal may only show proceed while a route is set and the "
               "section ahead is proven clear.";
    case RC_INV_ROUTES_EXCLUSIVE:
        return "Two routes sharing any track or point shall never both be set.";
    case RC_INV_NO_MOVE_UNDER_TRAIN:
        return "A point shall not move while the section it occupies is occupied.";
    case RC_INV_POINT_LOCK_HELD:
        return "A point locked by a route shall not move until it is released.";
    case RC_INV_UNKNOWN_IS_OCCUPIED:
        return "An UNKNOWN or failed track circuit shall be treated as occupied.";
    case RC_INV_EMERGENCY_ALL_DANGER:
        return "On emergency stop every signal shows danger and every train "
               "applies the emergency brake.";
    case RC_INV_RELEASE_ON_CLEAR:
        return "A route is released only once the train has cleared it, or by "
               "an audited cancel with the path proven clear.";
    case RC_INV_AUTHORITY_VIA_INTERLOCK:
        return "Movement authority is granted only through the interlocking.";
    }
    return "Unknown invariant";
}

/* --------------------------------------------------------------------------
 * Settings
 * -------------------------------------------------------------------------- */
static RcSettings g_settings;
static bool g_settings_ready = false;

static void settings_defaults(RcSettings *s)
{
    memset(s, 0, sizeof(*s));

    s->tick_interval_ms = 100;
    s->time_scale = 1;
    s->auto_signal_routes = true;
    s->auto_release_routes = true;

    s->fail_safe_unknown = true;          /* never disabled in a real system */
    s->allow_call_on = false;
    s->block_on_circuit_failure = true;
    s->emergency_requires_stop = true;

    s->show_grid = true;
    s->show_sensor_ids = false;
    s->show_train_labels = true;
    s->animate_points = true;
    s->diagram_zoom_percent = 100;

    s->log_to_file = true;
    snprintf(s->log_path, sizeof(s->log_path), "%s", "railcontrol.log");
    s->log_info_events = true;
    s->event_retention = 500;

    s->conjobs_enabled = true;
    s->conjob_dry_run = false;
    s->scripting_enabled = true;
}

RcSettings *rc_settings(void)
{
    if (!g_settings_ready) {
        settings_defaults(&g_settings);
        g_settings_ready = true;
    }
    return &g_settings;
}

/* --------------------------------------------------------------------------
 * INI parsing helpers (shared with the CONJOB and script registries).
 * -------------------------------------------------------------------------- */
static char *trim_in_place(char *text)
{
    char *end;

    if (text == NULL) {
        return NULL;
    }
    while (*text != '\0' && isspace((unsigned char)*text)) {
        text++;
    }
    if (*text == '\0') {
        return text;
    }
    end = text + strlen(text) - 1;
    while (end > text && isspace((unsigned char)*end)) {
        *end-- = '\0';
    }
    return text;
}

static bool parse_bool(const char *value, bool fallback)
{
    if (value == NULL) {
        return fallback;
    }
    if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0) {
        return true;
    }
    if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0 || strcmp(value, "no") == 0) {
        return false;
    }
    return fallback;
}

static int clamp_int(int value, int low, int high)
{
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

int rc_settings_normalise(RcSettings *s)
{
    int corrections = 0;
    int clamped;

    if (s == NULL) {
        return 0;
    }

    clamped = clamp_int(s->tick_interval_ms, 20, 2000);
    if (clamped != s->tick_interval_ms) { s->tick_interval_ms = clamped; corrections++; }

    clamped = clamp_int(s->time_scale, 1, 20);
    if (clamped != s->time_scale) { s->time_scale = clamped; corrections++; }

    clamped = clamp_int(s->diagram_zoom_percent, 50, 300);
    if (clamped != s->diagram_zoom_percent) { s->diagram_zoom_percent = clamped; corrections++; }

    clamped = clamp_int(s->event_retention, 50, 2048);
    if (clamped != s->event_retention) { s->event_retention = clamped; corrections++; }

    /* Fail-safe is not a preference: a safety system that lets an operator
     * disable it is not a safety system. */
    if (!s->fail_safe_unknown) {
        s->fail_safe_unknown = true;
        corrections++;
    }

    if (s->log_path[0] == '\0') {
        snprintf(s->log_path, sizeof(s->log_path), "%s", "railcontrol.log");
        corrections++;
    }

    return corrections;
}

int rc_settings_save(const RcSettings *s, const char *path)
{
    FILE *file;

    if (s == NULL || path == NULL) {
        return -1;
    }
    file = fopen(path, "w");
    if (file == NULL) {
        return -1;
    }

    fprintf(file, "# %s %s settings\n", RC_NAME, RC_VERSION_STRING);
    fprintf(file, "[simulation]\n");
    fprintf(file, "tick_interval_ms = %d\n", s->tick_interval_ms);
    fprintf(file, "time_scale = %d\n", s->time_scale);
    fprintf(file, "auto_signal_routes = %s\n", s->auto_signal_routes ? "true" : "false");
    fprintf(file, "auto_release_routes = %s\n", s->auto_release_routes ? "true" : "false");

    fprintf(file, "\n[safety]\n");
    fprintf(file, "fail_safe_unknown = %s\n", s->fail_safe_unknown ? "true" : "false");
    fprintf(file, "allow_call_on = %s\n", s->allow_call_on ? "true" : "false");
    fprintf(file, "block_on_circuit_failure = %s\n", s->block_on_circuit_failure ? "true" : "false");
    fprintf(file, "emergency_requires_stop = %s\n", s->emergency_requires_stop ? "true" : "false");

    fprintf(file, "\n[display]\n");
    fprintf(file, "show_grid = %s\n", s->show_grid ? "true" : "false");
    fprintf(file, "show_sensor_ids = %s\n", s->show_sensor_ids ? "true" : "false");
    fprintf(file, "show_train_labels = %s\n", s->show_train_labels ? "true" : "false");
    fprintf(file, "animate_points = %s\n", s->animate_points ? "true" : "false");
    fprintf(file, "diagram_zoom_percent = %d\n", s->diagram_zoom_percent);

    fprintf(file, "\n[logging]\n");
    fprintf(file, "log_to_file = %s\n", s->log_to_file ? "true" : "false");
    fprintf(file, "log_path = %s\n", s->log_path);
    fprintf(file, "log_info_events = %s\n", s->log_info_events ? "true" : "false");
    fprintf(file, "event_retention = %d\n", s->event_retention);

    fprintf(file, "\n[automation]\n");
    fprintf(file, "conjobs_enabled = %s\n", s->conjobs_enabled ? "true" : "false");
    fprintf(file, "conjob_dry_run = %s\n", s->conjob_dry_run ? "true" : "false");
    fprintf(file, "scripting_enabled = %s\n", s->scripting_enabled ? "true" : "false");

    if (fclose(file) != 0) {
        return -1;
    }
    return 0;
}

int rc_settings_load(RcSettings *s, const char *path)
{
    FILE *file;
    char line[512];
    char section[64] = "";

    if (s == NULL || path == NULL) {
        return -1;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }

    while (fgets(line, (int)sizeof(line), file) != NULL) {
        char *cursor = trim_in_place(line);
        char *equals;
        char *key;
        char *value;

        if (cursor == NULL || *cursor == '\0' || *cursor == '#' || *cursor == ';') {
            continue;
        }
        if (*cursor == '[') {
            char *close = strchr(cursor, ']');
            if (close != NULL) {
                *close = '\0';
                snprintf(section, sizeof(section), "%s", trim_in_place(cursor + 1));
            }
            continue;
        }

        equals = strchr(cursor, '=');
        if (equals == NULL) {
            continue;
        }
        *equals = '\0';
        key = trim_in_place(cursor);
        value = trim_in_place(equals + 1);
        if (key == NULL || value == NULL) {
            continue;
        }

        if (strcmp(key, "tick_interval_ms") == 0) {
            s->tick_interval_ms = atoi(value);
        } else if (strcmp(key, "time_scale") == 0) {
            s->time_scale = atoi(value);
        } else if (strcmp(key, "auto_signal_routes") == 0) {
            s->auto_signal_routes = parse_bool(value, s->auto_signal_routes);
        } else if (strcmp(key, "auto_release_routes") == 0) {
            s->auto_release_routes = parse_bool(value, s->auto_release_routes);
        } else if (strcmp(key, "fail_safe_unknown") == 0) {
            s->fail_safe_unknown = parse_bool(value, s->fail_safe_unknown);
        } else if (strcmp(key, "allow_call_on") == 0) {
            s->allow_call_on = parse_bool(value, s->allow_call_on);
        } else if (strcmp(key, "block_on_circuit_failure") == 0) {
            s->block_on_circuit_failure = parse_bool(value, s->block_on_circuit_failure);
        } else if (strcmp(key, "emergency_requires_stop") == 0) {
            s->emergency_requires_stop = parse_bool(value, s->emergency_requires_stop);
        } else if (strcmp(key, "show_grid") == 0) {
            s->show_grid = parse_bool(value, s->show_grid);
        } else if (strcmp(key, "show_sensor_ids") == 0) {
            s->show_sensor_ids = parse_bool(value, s->show_sensor_ids);
        } else if (strcmp(key, "show_train_labels") == 0) {
            s->show_train_labels = parse_bool(value, s->show_train_labels);
        } else if (strcmp(key, "animate_points") == 0) {
            s->animate_points = parse_bool(value, s->animate_points);
        } else if (strcmp(key, "diagram_zoom_percent") == 0) {
            s->diagram_zoom_percent = atoi(value);
        } else if (strcmp(key, "log_to_file") == 0) {
            s->log_to_file = parse_bool(value, s->log_to_file);
        } else if (strcmp(key, "log_path") == 0) {
            snprintf(s->log_path, sizeof(s->log_path), "%s", value);
        } else if (strcmp(key, "log_info_events") == 0) {
            s->log_info_events = parse_bool(value, s->log_info_events);
        } else if (strcmp(key, "event_retention") == 0) {
            s->event_retention = atoi(value);
        } else if (strcmp(key, "conjobs_enabled") == 0) {
            s->conjobs_enabled = parse_bool(value, s->conjobs_enabled);
        } else if (strcmp(key, "conjob_dry_run") == 0) {
            s->conjob_dry_run = parse_bool(value, s->conjob_dry_run);
        } else if (strcmp(key, "scripting_enabled") == 0) {
            s->scripting_enabled = parse_bool(value, s->scripting_enabled);
        }
    }

    fclose(file);
    (void)section;
    (void)rc_settings_normalise(s);
    return 0;
}
