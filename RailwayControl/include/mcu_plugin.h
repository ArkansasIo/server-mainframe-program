/* ==========================================================================
 * mcu_plugin.h - 🔌 Microcontroller plugin system.
 *
 * The interlocking in this program is complete on its own: it needs only its
 * signals, points, track circuits and trains. Everything outside that - the
 * physical world of buttons, lamps, detectors and remote relay rooms - arrives
 * through a *plugin*: a small described adapter that a microcontroller board
 * presents to the engine.
 *
 * WHY A PLUGIN LAYER, AND WHY IT LOOKS LIKE THIS
 * ----------------------------------------------
 * A real installation does not let a hobbyist board reach into the safety
 * logic. Field equipment talks to the interlocking over a *described*
 * interface: a fixed set of readings in, a fixed set of commands out, with
 * timeouts, sequence numbers and a fail-safe default on every input. This
 * header models exactly that boundary.
 *
 * Three rules shape the whole design:
 *
 *   1. A plugin CANNOT bypass the interlocking. Every command it issues is
 *      routed through the same public API the signaller uses, so it can be
 *      refused (INV-8). A plugin that asks to clear a signal into an occupied
 *      section is rejected exactly as an operator would be.
 *
 *   2. Every input has a FAIL-SAFE default. If a plugin stops heartbeating,
 *      its inputs are driven to their declared safe state - which is why
 *      `safe_state` is part of the binding, not an afterthought.
 *
 *   3. A plugin is DESCRIBED, not loaded as arbitrary code. Boards, buses,
 *      capabilities and bindings are small enums and fixed arrays, so a
 *      plugin can be written into a config file, printed in the event log
 *      verbatim, and reasoned about without executing anything. (A future
 *      dynamic loader can sit on top of this; the description is the
 *      contract either way.)
 *
 * SAFETY NOTICE
 * -------------
 * This is a SIMULATOR. Plugins here drive a simulated railway. A deployable
 * field interface must satisfy EN 50159 (safe transmission), EN 50128 and
 * IEC 61508 SIL 4, with certified hardware and independent assessment. None
 * of that is claimed here - the point of this file is the *shape* of a safe
 * boundary, not to be one.
 * ========================================================================== */
#ifndef MCU_PLUGIN_H
#define MCU_PLUGIN_H

#include "railway_types.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* --------------------------------------------------------------------------
 * Capacity limits - fixed arrays, deterministic, allocation free.
 * -------------------------------------------------------------------------- */
#define MCU_MAX_PLUGINS 32
#define MCU_MAX_BINDINGS 128 /* inputs + outputs across all plugins */
#define MCU_MAX_NAME 32
#define MCU_MAX_TEXT 192
#define MCU_MAX_DEVICE_PATH 96 /* e.g. "COM3", "tcp://10.0.0.5:9000" */
#define MCU_MAX_FIRMWARE 24

    /* --------------------------------------------------------------------------
     * Board families. The board decides which buses and pins exist, and the
     * default voltage - the three things a plugin author actually needs.
     * -------------------------------------------------------------------------- */
    typedef enum
    {
        MCU_BOARD_UNKNOWN = 0,
        MCU_BOARD_ARDUINO_UNO,    /* 8-bit AVR, 5 V, UART + I2C + SPI */
        MCU_BOARD_ARDUINO_MEGA,   /* 8-bit AVR, 5 V, more pins */
        MCU_BOARD_ESP32,          /* 32-bit Xtensa, 3.3 V, WiFi + UART + I2C + SPI + CAN */
        MCU_BOARD_ESP8266,        /* 32-bit, 3.3 V, WiFi + UART */
        MCU_BOARD_STM32,          /* 32-bit ARM Cortex-M, 3.3 V, CAN-capable */
        MCU_BOARD_RP2040,         /* Raspberry Pi Pico, 3.3 V, dual core */
        MCU_BOARD_TEENSY,         /* 32-bit ARM, 3.3 V, fast */
        MCU_BOARD_PLC_INDUSTRIAL, /* industrial PLC acting as a field concentrator */
        MCU_BOARD_SIMULATOR       /* software stand-in, no hardware */
    } McuBoard;

    const char *mcu_board_name(McuBoard board);
    const char *mcu_board_code(McuBoard board); /* short code, e.g. "ESP32" */
    float mcu_board_voltage(McuBoard board);
    int mcu_board_max_pins(McuBoard board);
    bool mcu_board_has_can(McuBoard board);

    /* --------------------------------------------------------------------------
     * Field bus. This is the transport between the board and the interlocking.
     * -------------------------------------------------------------------------- */
    typedef enum
    {
        MCU_BUS_NONE = 0,
        MCU_BUS_UART, /* serial / RS-485 multidrop */
        MCU_BUS_I2C,
        MCU_BUS_SPI,
        MCU_BUS_CAN,      /* CAN bus - the usual choice for rolling stock */
        MCU_BUS_WIFI_TCP, /* networked board, e.g. ESP32 over WiFi */
        MCU_BUS_GPIO      /* direct pins, no bus at all */
    } McuBus;

    const char *mcu_bus_name(McuBus bus);
    bool mcu_bus_is_networked(McuBus bus);

    /* --------------------------------------------------------------------------
     * What a plugin can do. A bitmask, declared up front so the engine knows
     * which commands it is allowed to send before any traffic flows.
     * -------------------------------------------------------------------------- */
    typedef enum
    {
        MCU_CAP_NONE = 0,
        MCU_CAP_READ_DIGITAL = 1 << 0,       /* switches, treadles, buttons */
        MCU_CAP_READ_ANALOG = 1 << 1,        /* temperatures, currents */
        MCU_CAP_WRITE_DIGITAL = 1 << 2,      /* lamps, relays, barriers */
        MCU_CAP_WRITE_ANALOG = 1 << 3,       /* PWM - lamp dimming, motor speed */
        MCU_CAP_READ_TRACK_CIRCUIT = 1 << 4, /* the safety-relevant one */
        MCU_CAP_READ_AXLE_COUNTER = 1 << 5,
        MCU_CAP_READ_HOT_BOX = 1 << 6,
        MCU_CAP_CONTROL_LEVEL_CROSSING = 1 << 7,
        MCU_CAP_CONTROL_POINT = 1 << 8,  /* move a point - high trust */
        MCU_CAP_CONTROL_SIGNAL = 1 << 9, /* drive a signal head */
        MCU_CAP_HEARTBEAT = 1 << 10      /* participates in liveness checking */
    } McuCapability;

    /* --------------------------------------------------------------------------
     * Plugin lifecycle state.
     * -------------------------------------------------------------------------- */
    typedef enum
    {
        MCU_STATE_UNLOADED = 0,
        MCU_STATE_LOADED,
        MCU_STATE_RUNNING,
        MCU_STATE_DEGRADED, /* communicating but some inputs are stale */
        MCU_STATE_STALE,    /* missed heartbeat - inputs forced to safe state */
        MCU_STATE_FAULTED   /* reported a self-test failure */
    } McuState;

    const char *mcu_state_name(McuState state);

    /* --------------------------------------------------------------------------
     * A binding maps one pin/register on the board to one asset in the engine.
     *
     *   INPUT  (board -> engine): a reading that drives a sensor or a track
     *   OUTPUT (engine -> board): a command that lights a lamp or moves a point
     *
     * `safe_state` is the value the engine uses the moment the plugin goes
     * stale. It is mandatory, not optional - an input with no safe state is an
     * input that fails silently.
     * -------------------------------------------------------------------------- */
    typedef enum
    {
        MCU_BIND_NONE = 0,
        /* Inputs */
        MCU_BIND_TRACK_CIRCUIT,   /* target = track id */
        MCU_BIND_AXLE_COUNTER,    /* target = track id, value = net axles */
        MCU_BIND_TREADLE,         /* target = track id */
        MCU_BIND_HOT_BOX,         /* target = track id, value = degrees C */
        MCU_BIND_LEVEL_CROSSING,  /* target = track id, value = 1 barriers down */
        MCU_BIND_POINT_DETECTION, /* target = point id, value = lie */
        MCU_BIND_SIGNAL_REPEATER, /* target = signal id, value = aspect */
        /* Outputs */
        MCU_BIND_SIGNAL_LAMP,    /* target = signal id - mirror the aspect */
        MCU_BIND_POINT_DRIVE,    /* target = point id - request a lie */
        MCU_BIND_BARRIER_MOTOR,  /* target = track id - raise/lower |
                                  * requires MCU_CAP_CONTROL_LEVEL_CROSSING */
        MCU_BIND_INDICATOR_LAMP, /* target = any asset - status lamp */
        MCU_BIND_ALARM_SOUNDER   /* target = none - drives the alarm buzzer */
    } McuBindingKind;

    const char *mcu_binding_kind_name(McuBindingKind kind);
    bool mcu_binding_is_input(McuBindingKind kind);
    bool mcu_binding_is_output(McuBindingKind kind);

    /* Which capability a binding needs. The loader refuses a plugin that binds
     * something it did not declare. */
    McuCapability mcu_binding_required_capability(McuBindingKind kind);

    typedef struct
    {
        RwId id;
        McuBindingKind kind;
        RwId target;     /* asset id in the engine */
        int pin;         /* board pin / register number */
        bool active_low; /* invert the electrical sense */
        int safe_state;  /* value applied when the plugin is stale */
        int value;       /* last value read / written */
        bool stale;      /* true once the heartbeat lapsed */
        unsigned last_update_ms;
        unsigned transitions; /* how often the value changed */
    } McuBinding;

    /* --------------------------------------------------------------------------
     * The plugin itself.
     * -------------------------------------------------------------------------- */
    typedef struct
    {
        RwId id;
        char name[MCU_MAX_NAME]; /* "YARD-NODE-1" */
        char description[MCU_MAX_TEXT];
        McuBoard board;
        McuBus bus;
        char device[MCU_MAX_DEVICE_PATH]; /* "COM3", "i2c:0x20", "tcp://..." */
        char firmware[MCU_MAX_FIRMWARE];
        unsigned capabilities; /* bitmask of McuCapability */

        bool enabled;
        McuState state;
        int address; /* bus address, or node id on CAN */

        /* Liveness. A plugin that stops answering is driven to safe state. */
        unsigned heartbeat_ms; /* expected interval; 0 = not checked */
        unsigned last_seen_ms;
        unsigned missed_heartbeats;
        unsigned timeout_ms; /* grace before STALE */

        /* Counters, for the diagnostics panel. */
        unsigned reads;
        unsigned writes;
        unsigned refusals; /* commands the interlocking refused */
        unsigned faults;
        unsigned timeouts;

        McuBinding bindings[MCU_MAX_BINDINGS];
        size_t binding_count;

        unsigned loaded_at_ms;
    } McuPlugin;

    /* --------------------------------------------------------------------------
     * Registry settings (the "Plugins" page in the GUI / the [plugins] INI
     * section).
     * -------------------------------------------------------------------------- */
    typedef struct
    {
        bool enabled;                   /* master switch for the whole system */
        bool allow_point_control;       /* may any plugin move a point? */
        bool allow_signal_control;      /* may any plugin drive a signal lamp? */
        bool allow_track_circuit_input; /* may a plugin assert occupancy? */
        bool fail_safe_on_timeout;      /* force safe state when stale - INV */
        bool dry_run;                   /* observe only, never write */
        unsigned default_timeout_ms;    /* when a plugin does not set its own */
        unsigned poll_interval_ms;      /* how often bindings are refreshed */
        int log_level;                  /* 0 quiet, 1 normal, 2 verbose */
    } McuSettings;

    /* ==========================================================================
     * Public plugin API
     * ========================================================================== */

    /* Build the registry and install the demonstration board set. */
    void mcu_init(void);
    void mcu_shutdown(void);

    /* Register a plugin. The struct is copied - the caller keeps ownership.
     * Returns the new plugin id, or RW_ID_NONE when the registry is full or the
     * declaration is invalid (unknown board, no name, capabilities missing). */
    RwId mcu_add(const McuPlugin *plugin);

    /* Remove a plugin. Its outputs are driven to safe state first, so a lamp
     * does not stay lit because its driver was unplugged. */
    RwResult mcu_remove(RwId plugin_id);

    /* Enable / disable. Disabling forces safe state but keeps the binding
     * definitions so it can be re-enabled. */
    RwResult mcu_set_enabled(RwId plugin_id, bool enabled);

    /* Bind one pin to one asset. Refused when the plugin has not declared the
     * capability the binding needs. Returns the new binding id. */
    RwId mcu_bind(RwId plugin_id, McuBindingKind kind, RwId target, int pin,
                  bool active_low, int safe_state);

    RwResult mcu_unbind(RwId binding_id);

    /* Per-tick work: refresh inputs, apply outputs, check heartbeats.
     * Called by the engine tick alongside the controllers. */
    void mcu_tick(unsigned delta_ms);

    /* Liveness. A board calls this to say "I am here"; a real one would do it by
     * answering a poll. Updates last_seen_ms and clears the stale flag. */
    RwResult mcu_heartbeat(RwId plugin_id);

    /* Drive one input binding by hand - the entry point a real driver would use
     * when a byte arrives from the bus. Routes through rw_sensor_apply(), so the
     * normal fail-safe propagation applies. */
    RwResult mcu_feed_input(RwId plugin_id, int pin, int value, bool triggered);

    /* Force every binding of a plugin (or all plugins) to its safe state. */
    RwResult mcu_apply_safe_state(RwId plugin_id);
    int mcu_apply_safe_state_all(void);

    /* Settings. */
    McuSettings *mcu_settings(void);
    RwResult mcu_apply_settings(const McuSettings *settings);
    const char *mcu_settings_summary(void);

    /* Inspection for the GUI, CLI and the diagnostics page. */
    int mcu_count(void);
    RwResult mcu_get(RwId plugin_id, McuPlugin *out);
    RwResult mcu_get_at(int index, McuPlugin *out);
    RwId mcu_find(const char *name);
    RwResult mcu_get_binding(RwId binding_id, McuBinding *out);
    int mcu_binding_count(RwId plugin_id);

    /* Aggregate counters for the status line. */
    typedef struct
    {
        int plugins_total;
        int plugins_running;
        int plugins_stale;
        int plugins_faulted;
        int bindings_total;
        unsigned total_reads;
        unsigned total_writes;
        unsigned total_refusals;
        unsigned total_timeouts;
    } McuStats;

    RwResult mcu_get_stats(McuStats *stats);

    /* Persistence: the registry as an INI-style file, matching the CONJOB and
     * settings files so one editor handles all three. */
    RwResult mcu_save(const char *path);
    RwResult mcu_load(const char *path);

    /* One-line rendering for the GUI list:
     *   "YARD-NODE-1 [running] ESP32 over CAN addr 12 - 6 bindings, 0 stale" */
    RwResult mcu_describe(RwId plugin_id, char *buffer, size_t size);

    /* Human readable name for a capability, for the About / diagnostics panel. */
    const char *mcu_capability_name(McuCapability capability);

#ifdef __cplusplus
}
#endif

#endif /* MCU_PLUGIN_H */
