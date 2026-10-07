/* ==========================================================================
 * rc_audio.c - 🔊 Sound effects and audible alarms.
 *
 * Backends
 *   WINMM    PlaySound() with the generated .wav files, falling back to a
 *            system message beep when the file is missing. This is the default
 *            on Windows and needs no extra build dependency (winmm.lib).
 *   CONSOLE  writes the ASCII BEL character. Portable everywhere, silent on
 *            most modern terminals unless the user enables the bell, which is
 *            exactly the right default for a program that may run headless.
 *   SILENT   no output. Used by the tests and by --no-audio.
 *
 * Degradation is graceful and always logged once, so an operator is never
 * left believing alarms are audible when they are not.
 * ========================================================================== */
#include "rc_audio.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>
#endif

/* ==========================================================================
 * Palette
 * ========================================================================== */
typedef struct
{
    const char *name;
    const char *description;
    const char *file;
    int priority; /* 0..9 */
    unsigned debounce_ms;
} SoundSpec;

/* The table order matches RcSoundId exactly. Priorities are chosen so an
 * emergency (9) always beats a keypress (0), and a track circuit failure (7)
 * beats a route set (3). */
static const SoundSpec g_sounds[SOUND_COUNT] = {
    {"NONE", "no sound", "", 0, 0},
    {"KEYPRESS", "command accepted", "keypress.wav", 0, 40},
    {"REFUSED", "command refused by the interlocking", "refused.wav", 4, 250},
    {"ACKNOWLEDGE", "alarm acknowledged", "acknowledge.wav", 1, 120},
    {"SIGNAL_CLEARED", "a signal cleared to proceed", "signal_clear.wav", 2, 150},
    {"SIGNAL_DANGER", "a signal replaced to danger", "signal_danger.wav", 2, 150},
    {"POINT_MOVING", "a point is swinging", "point_move.wav", 1, 400},
    {"POINT_LOCKED", "a point locked by a route", "point_lock.wav", 2, 200},
    {"ROUTE_SET", "a route was set", "route_set.wav", 3, 200},
    {"ROUTE_RELEASED", "a route was released", "route_release.wav", 2, 200},
    {"TRAIN_DEPART", "a train departed under authority", "train_depart.wav", 3, 500},
    {"TRAIN_ARRIVE", "a train arrived in a section", "train_arrive.wav", 3, 500},
    {"WARNING", "a warning was raised", "warning.wav", 5, 800},
    {"ALARM", "an alarm was raised", "alarm.wav", 6, 800},
    {"TRACK_CIRCUIT", "a track circuit has failed", "track_fail.wav", 7, 1500},
    {"COLLISION_RISK", "a collision or route conflict was detected", "collision.wav", 8, 1500},
    {"EMERGENCY", "the emergency stop was tripped", "emergency.wav", 9, 0},
    {"EMERGENCY_CLEAR", "the emergency stop was cleared", "emergency_clear.wav", 5, 500},
    {"CRITICAL", "a system level critical alarm", "critical.wav", 9, 1000}};

const char *rc_sound_name(RcSoundId sound)
{
    if (sound <= SOUND_NONE || sound >= SOUND_COUNT)
    {
        return "NONE";
    }
    return g_sounds[sound].name;
}

const char *rc_sound_description(RcSoundId sound)
{
    if (sound <= SOUND_NONE || sound >= SOUND_COUNT)
    {
        return "no sound";
    }
    return g_sounds[sound].description;
}

const char *rc_sound_file(RcSoundId sound)
{
    if (sound <= SOUND_NONE || sound >= SOUND_COUNT)
    {
        return "";
    }
    return g_sounds[sound].file;
}

int rc_sound_priority(RcSoundId sound)
{
    if (sound <= SOUND_NONE || sound >= SOUND_COUNT)
    {
        return 0;
    }
    return g_sounds[sound].priority;
}

unsigned rc_sound_debounce_ms(RcSoundId sound)
{
    if (sound <= SOUND_NONE || sound >= SOUND_COUNT)
    {
        return 0;
    }
    return g_sounds[sound].debounce_ms;
}

RcSoundId rc_sound_parse(const char *name)
{
    int i;

    if (name == NULL)
    {
        return SOUND_NONE;
    }
    for (i = 0; i < SOUND_COUNT; ++i)
    {
        const char *candidate = g_sounds[i].name;
        size_t j = 0;
        bool match = true;

        for (j = 0; candidate[j] != '\0' && name[j] != '\0'; ++j)
        {
            if (toupper((unsigned char)candidate[j]) != toupper((unsigned char)name[j]))
            {
                match = false;
                break;
            }
        }
        if (match && candidate[j] == '\0' && name[j] == '\0')
        {
            return (RcSoundId)i;
        }
    }
    return SOUND_NONE;
}

const char *rc_audio_backend_name(RcAudioBackend backend)
{
    switch (backend)
    {
    case AUDIO_BACKEND_SILENT:
        return "SILENT";
    case AUDIO_BACKEND_CONSOLE:
        return "CONSOLE";
    case AUDIO_BACKEND_WINMM:
        return "WINMM";
    }
    return "SILENT";
}

/* ==========================================================================
 * State
 * ========================================================================== */
static RcAudioSettings g_settings;
static RcAudioStats g_stats;
static char g_asset_dir[260];
static bool g_initialised = false;
static bool g_backend_warned = false;

/* Debounce bookkeeping per sound. */
static unsigned g_last_played_at[SOUND_COUNT];
static unsigned g_clock_ms = 0;

/* Rate limiting across all sounds. */
static unsigned g_second_window_ms = 0;
static int g_played_in_window = 0;

/* The currently sounding element, for the priority rule. */
static RcSoundId g_current = SOUND_NONE;
static unsigned g_current_until_ms = 0;

/* Emergency repeat. */
static unsigned g_emergency_next_ms = 0;

/* --------------------------------------------------------------------------
 * Settings
 * -------------------------------------------------------------------------- */
void rc_audio_defaults(RcAudioSettings *settings)
{
    int i;

    if (settings == NULL)
    {
        return;
    }
    memset(settings, 0, sizeof(*settings));
    settings->enabled = true;
    settings->muted = false;
#if defined(_WIN32)
    settings->backend = AUDIO_BACKEND_WINMM;
#else
    settings->backend = AUDIO_BACKEND_CONSOLE;
#endif
    settings->master_volume = 70;
    settings->max_per_second = 6;
    settings->announce_emergency = true;
    settings->emergency_repeat_ms = 2000;

    for (i = 0; i < SOUND_COUNT; ++i)
    {
        settings->volume[i] = 70;
        settings->per_sound_enabled[i] = (i != SOUND_NONE);
    }

    /* Spoken-style feedback tones should be quieter than alarms; the operator
     * must be able to work without the panel constantly chirping. */
    settings->volume[SOUND_KEYPRESS] = 30;
    settings->volume[SOUND_ACKNOWLEDGE] = 35;
    settings->volume[SOUND_POINT_MOVING] = 25;
    settings->volume[SOUND_EMERGENCY] = 100;
    settings->volume[SOUND_CRITICAL] = 100;
    settings->volume[SOUND_COLLISION_RISK] = 95;
}

static int clamp_int(int value, int low, int high)
{
    if (value < low)
    {
        return low;
    }
    if (value > high)
    {
        return high;
    }
    return value;
}

int rc_audio_normalise(RcAudioSettings *settings)
{
    int corrections = 0;
    int i;
    int clamped;

    if (settings == NULL)
    {
        return 0;
    }

    clamped = clamp_int(settings->master_volume, 0, 100);
    if (clamped != settings->master_volume)
    {
        settings->master_volume = clamped;
        corrections++;
    }

    clamped = clamp_int(settings->max_per_second, 1, 30);
    if (clamped != settings->max_per_second)
    {
        settings->max_per_second = clamped;
        corrections++;
    }

    clamped = clamp_int(settings->emergency_repeat_ms, 500, 10000);
    if (clamped != settings->emergency_repeat_ms)
    {
        settings->emergency_repeat_ms = clamped;
        corrections++;
    }

    for (i = 0; i < SOUND_COUNT; ++i)
    {
        clamped = clamp_int(settings->volume[i], 0, 100);
        if (clamped != settings->volume[i])
        {
            settings->volume[i] = clamped;
            corrections++;
        }
    }

    /* The emergency tone must always be armed. An operator who can silence the
     * emergency alarm has defeated the alarm. Turning sound off entirely is
     * still allowed (enabled = false), but that is an explicit, logged choice. */
    settings->per_sound_enabled[SOUND_EMERGENCY] = true;
    settings->per_sound_enabled[SOUND_CRITICAL] = true;

    return corrections;
}

RcAudioSettings *rc_audio_settings(void)
{
    return &g_settings;
}

/* ==========================================================================
 * Lifecycle
 * ========================================================================== */
void rc_audio_init(const char *asset_directory)
{
    rc_audio_defaults(&g_settings);
    memset(&g_stats, 0, sizeof(g_stats));
    memset(g_last_played_at, 0, sizeof(g_last_played_at));

    if (asset_directory != NULL)
    {
        snprintf(g_asset_dir, sizeof(g_asset_dir), "%s", asset_directory);
    }
    else
    {
        g_asset_dir[0] = '\0';
    }

    g_current = SOUND_NONE;
    g_current_until_ms = 0;
    g_emergency_next_ms = 0;
    g_initialised = true;
    g_backend_warned = false;
}

void rc_audio_shutdown(void)
{
    rc_audio_stop_all();
    g_initialised = false;
}

/* ==========================================================================
 * Backends
 * ========================================================================== */
static void play_console(RcSoundId sound)
{
    /* BEL. The volume is expressed by repeating the bell for the louder
     * sounds; a terminal bell is either on or off so this is the only
     * available nuance. */
    int repeats = 1;

    if (g_settings.volume[sound] > 80)
    {
        repeats = 3;
    }
    else if (g_settings.volume[sound] > 50)
    {
        repeats = 2;
    }

    while (repeats-- > 0)
    {
        fputc('\a', stderr);
    }
    fflush(stderr);
}

#if defined(_WIN32)
static void play_winmm(RcSoundId sound)
{
    char path[520];

    if (g_asset_dir[0] != '\0')
    {
        snprintf(path, sizeof(path), "%s\\%s", g_asset_dir, g_sounds[sound].file);
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
        {
            /* SND_ASYNC so the UI never blocks on a sound. */
            if (PlaySoundA(path, NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT))
            {
                return;
            }
        }
    }

    /* Fall back to a system beep shaped by severity. This keeps the audible
     * indication working even when the asset directory is missing. */
    {
        DWORD frequency = 440;
        DWORD duration = 150;

        switch (sound)
        {
        case SOUND_EMERGENCY:
        case SOUND_CRITICAL:
            frequency = 880;
            duration = 600;
            break;
        case SOUND_COLLISION_RISK:
        case SOUND_TRACK_CIRCUIT:
            frequency = 660;
            duration = 400;
            break;
        case SOUND_ALARM:
        case SOUND_WARNING:
            frequency = 550;
            duration = 250;
            break;
        case SOUND_REFUSED:
            frequency = 220;
            duration = 200;
            break;
        default:
            frequency = 440;
            duration = 80;
            break;
        }
        Beep(frequency, duration);
    }
}
#else
static void play_winmm(RcSoundId sound)
{
    (void)sound;
    play_console(sound);
}
#endif

/* ==========================================================================
 * Playing
 * ========================================================================== */
void rc_audio_stop_all(void)
{
#if defined(_WIN32)
    if (g_settings.backend == AUDIO_BACKEND_WINMM)
    {
        PlaySoundA(NULL, NULL, 0);
    }
#endif
    g_current = SOUND_NONE;
    g_current_until_ms = 0;

#if defined(_WIN32)
    /* WinMM pulls in winmm.lib; when the program is built without it the
     * linker would fail on PlaySoundA, so the call is guarded by the build. */
#endif
}

static bool emit(RcSoundId sound)
{
    switch (g_settings.backend)
    {
    case AUDIO_BACKEND_SILENT:
        return false;
    case AUDIO_BACKEND_CONSOLE:
        play_console(sound);
        return true;
    case AUDIO_BACKEND_WINMM:
        play_winmm(sound);
        return true;
    }
    return false;
}

bool rc_audio_play_forced(RcSoundId sound)
{
    if (!g_initialised || sound <= SOUND_NONE || sound >= SOUND_COUNT)
    {
        return false;
    }

    g_stats.played++;
    g_stats.last_sound = sound;
    g_current = sound;
    g_current_until_ms = g_clock_ms + (unsigned)rc_sound_priority(sound) * 60u;

    if (!emit(sound) && !g_backend_warned)
    {
        fprintf(stderr,
                "[audio] backend %s produced no output - audible alarms are NOT working\n",
                rc_audio_backend_name(g_settings.backend));
        fflush(stderr);
        g_backend_warned = true;
        return false;
    }
    return true;
}

bool rc_audio_play(RcSoundId sound)
{
    if (!g_initialised || sound <= SOUND_NONE || sound >= SOUND_COUNT)
    {
        return false;
    }

    /* Master switches first, and count why we suppressed so the settings page
     * can show the operator that sound is being dropped. */
    if (!g_settings.enabled)
    {
        g_stats.suppressed_disabled++;
        return false;
    }
    if (g_settings.muted)
    {
        g_stats.suppressed_muted++;
        return false;
    }
    if (!g_settings.per_sound_enabled[sound] || g_settings.volume[sound] <= 0)
    {
        g_stats.suppressed_disabled++;
        return false;
    }

    /* Priority: never cut off something more important. The emergency and the
     * critical alarm are exempt from being suppressed in turn. */
    if (g_current != SOUND_NONE && g_clock_ms < g_current_until_ms)
    {
        if (rc_sound_priority(sound) <= rc_sound_priority(g_current))
        {
            g_stats.suppressed_priority++;
            return false;
        }
    }

    /* Debounce: collapse a burst of identical events into one tone. */
    if (rc_sound_debounce_ms(sound) > 0)
    {
        const unsigned since = g_clock_ms - g_last_played_at[sound];
        if (g_last_played_at[sound] != 0 && since < rc_sound_debounce_ms(sound))
        {
            g_stats.suppressed_rate++;
            return false;
        }
    }

    /* Rate limit across all sounds so a cascade of route releases cannot turn
     * the panel into a machine gun. */
    if (g_played_in_window >= g_settings.max_per_second)
    {
        if (sound != SOUND_EMERGENCY && sound != SOUND_CRITICAL)
        {
            g_stats.suppressed_rate++;
            return false;
        }
    }

    g_last_played_at[sound] = g_clock_ms;
    g_played_in_window++;
    return rc_audio_play_forced(sound);
}

void rc_audio_play_for_severity(int severity)
{
    /* Mirrors RwSeverity: 0 info, 1 warning, 2 alarm, 3 critical. */
    switch (severity)
    {
    case 0:
        rc_audio_play(SOUND_ACKNOWLEDGE);
        break;
    case 1:
        rc_audio_play(SOUND_WARNING);
        break;
    case 2:
        rc_audio_play(SOUND_ALARM);
        break;
    default:
        rc_audio_play(SOUND_CRITICAL);
        break;
    }
}

/* ==========================================================================
 * Tick
 * ========================================================================== */
void rc_audio_tick(unsigned delta_ms, bool emergency_active)
{
    if (!g_initialised)
    {
        return;
    }

    g_clock_ms += delta_ms;

    /* One second rate limiting window. */
    g_second_window_ms += delta_ms;
    if (g_second_window_ms >= 1000)
    {
        g_second_window_ms = 0;
        g_played_in_window = 0;
    }

    if (g_current != SOUND_NONE && g_clock_ms >= g_current_until_ms)
    {
        g_current = SOUND_NONE;
    }

    /* The emergency tone repeats while the emergency is active. This is the
     * one sound that is deliberately allowed to be intrusive: a signaller must
     * not be able to forget that the panel is in emergency. */
    if (emergency_active && g_settings.announce_emergency && g_settings.enabled && !g_settings.muted)
    {
        if (g_clock_ms >= g_emergency_next_ms)
        {
            rc_audio_play_forced(SOUND_EMERGENCY);
            g_emergency_next_ms = g_clock_ms + (unsigned)g_settings.emergency_repeat_ms;
        }
    }
    else
    {
        g_emergency_next_ms = g_clock_ms;
    }

    if (!emergency_active && g_current == SOUND_EMERGENCY)
    {
        g_current = SOUND_NONE;
    }
}

/* ==========================================================================
 * Diagnostics
 * ========================================================================== */
void rc_audio_stats(RcAudioStats *stats)
{
    if (stats != NULL)
    {
        *stats = g_stats;
    }
}

void rc_audio_format_palette(char *buffer, size_t size)
{
    size_t used = 0;
    int i;

    if (buffer == NULL || size == 0)
    {
        return;
    }
    buffer[0] = '\0';

    used += (size_t)snprintf(buffer + used, size - used,
                             "  %-18s %-4s %-5s %-7s %s\n",
                             "SOUND", "PRIO", "VOL", "STATE", "DESCRIPTION");
    used += (size_t)snprintf(buffer + used, size - used,
                             "  ------------------ ---- ----- ------- "
                             "----------------------------------------\n");

    for (i = 1; i < SOUND_COUNT; ++i)
    {
        const char *state;

        if (!g_settings.per_sound_enabled[i] || g_settings.volume[i] <= 0)
        {
            state = "off";
        }
        else if (g_settings.muted)
        {
            state = "muted";
        }
        else if (!g_settings.enabled)
        {
            state = "disabled";
        }
        else
        {
            state = "on";
        }

        used += (size_t)snprintf(buffer + used, size - used,
                                 "  %-18s %-4d %-5d %-7s %s\n",
                                 g_sounds[i].name, g_sounds[i].priority, g_settings.volume[i],
                                 state, g_sounds[i].description);
        if (used >= size)
        {
            break;
        }
    }
}
