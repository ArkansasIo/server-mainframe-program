/* ==========================================================================
 * rc_audio.h - 🔊 Sound effects and audible alarms.
 *
 * A signalling control centre is not silent. Real installations have audible
 * indications for alarms, emergency stops and train movement, and signallers
 * rely on them: an alarm you can hear is an alarm you notice while looking at
 * another part of the panel.
 *
 * This module provides:
 *   - a small set of named system sounds (alarm, emergency, route set, ...)
 *   - a per-sound enable/volume table the operator can edit in Settings
 *   - a priority rule so a critical sound is never masked by a low priority one
 *   - a mute function for heads-down working
 *   - a backlog so a burst of events does not produce a burst of noise
 *
 * Backends
 * --------
 * AUDIO_BACKEND_SILENT  no sound at all (headless, tests, CI)
 * AUDIO_BACKEND_CONSOLE bell characters (\a) - the portable default
 * AUDIO_BACKEND_WINMM   the Win32 PlaySound/message beep API
 *
 * The synthesiser is deliberately not here: the generated WAV assets live in
 * assets/audio and are produced by tools/make_assets.c, so the program itself
 * has no audio synthesis code and no platform audio dependency beyond a
 * single PlaySound call.
 *
 * SAFETY NOTICE
 * -------------
 * Sound is an ADVISORY indication only. It must never be the sole means of
 * conveying a safety-critical state. Every audible alarm is also shown on the
 * panel, written to the event log and (for critical alarms) sent to stderr.
 * ========================================================================== */
#ifndef RC_AUDIO_H
#define RC_AUDIO_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* --------------------------------------------------------------------------
     * Sound palette
     * -------------------------------------------------------------------------- */
    typedef enum
    {
        SOUND_NONE = 0,

        /* Operator feedback */
        SOUND_KEYPRESS,    /* a command was accepted */
        SOUND_REFUSED,     /* a command was refused */
        SOUND_ACKNOWLEDGE, /* an alarm was acknowledged */

        /* Control actions */
        SOUND_SIGNAL_CLEARED,
        SOUND_SIGNAL_DANGER,
        SOUND_POINT_MOVING,
        SOUND_POINT_LOCKED,
        SOUND_ROUTE_SET,
        SOUND_ROUTE_RELEASED,
        SOUND_TRAIN_DEPART,
        SOUND_TRAIN_ARRIVE,

        /* Alarms, in increasing severity */
        SOUND_WARNING,         /* a warning was raised */
        SOUND_ALARM,           /* an alarm was raised */
        SOUND_TRACK_CIRCUIT,   /* track circuit failure */
        SOUND_COLLISION_RISK,  /* a conflict was detected */
        SOUND_EMERGENCY,       /* the emergency stop was tripped */
        SOUND_EMERGENCY_CLEAR, /* the emergency stop was cleared */
        SOUND_CRITICAL,        /* system level critical alarm */

        SOUND_COUNT
    } RcSoundId;

    const char *rc_sound_name(RcSoundId sound);
    const char *rc_sound_description(RcSoundId sound);
    RcSoundId rc_sound_parse(const char *name);

    /* The asset file name for a sound, e.g. "alarm.wav". */
    const char *rc_sound_file(RcSoundId sound);

    /* Priority 0..9. A sound may not interrupt one of strictly higher priority
     * that is still playing. */
    int rc_sound_priority(RcSoundId sound);

    /* Default repeat/decay behaviour: the number of milliseconds a sound blocks
     * the channel, used to collapse a burst of identical events. */
    unsigned rc_sound_debounce_ms(RcSoundId sound);

    /* --------------------------------------------------------------------------
     * Backends
     * -------------------------------------------------------------------------- */
    typedef enum
    {
        AUDIO_BACKEND_SILENT = 0,
        AUDIO_BACKEND_CONSOLE,
        AUDIO_BACKEND_WINMM
    } RcAudioBackend;

    const char *rc_audio_backend_name(RcAudioBackend backend);

    /* --------------------------------------------------------------------------
     * Settings
     * -------------------------------------------------------------------------- */
    typedef struct
    {
        bool enabled; /* master switch */
        bool muted;   /* temporary silence, restorable */
        RcAudioBackend backend;
        int master_volume;       /* 0..100 */
        int volume[SOUND_COUNT]; /* per sound, 0..100 */
        bool per_sound_enabled[SOUND_COUNT];
        int max_per_second;      /* rate limit across all sounds */
        bool announce_emergency; /* repeat the emergency tone */
        int emergency_repeat_ms; /* how often, when active */
    } RcAudioSettings;

    RcAudioSettings *rc_audio_settings(void);

    /* Restore sensible defaults (all sounds on, master volume 70, console bell). */
    void rc_audio_defaults(RcAudioSettings *settings);

    /* Clamp and validate. Returns the number of fields corrected. */
    int rc_audio_normalise(RcAudioSettings *settings);

    /* --------------------------------------------------------------------------
     * Lifecycle
     * -------------------------------------------------------------------------- */

    /* Initialise the backend. `asset_directory` may be NULL, in which case the
     * console or WinMM tone fallback is used and no files are loaded. */
    void rc_audio_init(const char *asset_directory);

    void rc_audio_shutdown(void);

    /* --------------------------------------------------------------------------
     * Playing sounds
     * -------------------------------------------------------------------------- */

    /* Play a sound, subject to the settings, the priority rule and the debounce.
     * Returns true when the sound was actually produced. */
    bool rc_audio_play(RcSoundId sound);

    /* Play a sound even if it is muted or debounced - used for the emergency, or
     * for a test in the settings page. */
    bool rc_audio_play_forced(RcSoundId sound);

    /* Convenience: map an event severity onto the tone palette. */
    void rc_audio_play_for_severity(int severity);

    /* Stop everything currently playing. */
    void rc_audio_stop_all(void);

    /* --------------------------------------------------------------------------
     * Per-tick service
     * -------------------------------------------------------------------------- */

    /* Ages the debounce and rate-limit counters, and repeats the emergency tone
     * while the emergency stop is active. Call from the engine tick. */
    void rc_audio_tick(unsigned delta_ms, bool emergency_active);

    /* --------------------------------------------------------------------------
     * Inspection / diagnostics
     * -------------------------------------------------------------------------- */
    typedef struct
    {
        unsigned long played;
        unsigned long suppressed_muted;
        unsigned long suppressed_disabled;
        unsigned long suppressed_priority;
        unsigned long suppressed_rate;
        RcSoundId last_sound;
    } RcAudioStats;

    void rc_audio_stats(RcAudioStats *stats);

    /* Render the palette as a table (Settings page and CLI). */
    void rc_audio_format_palette(char *buffer, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* RC_AUDIO_H */
