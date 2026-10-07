/* ==========================================================================
 * mcu_plugin.c - 🔌 Microcontroller plugin engine.
 *
 * The registry, the bindings and the per-tick loop that connects a board's
 * pins to the railway. Read mcu_plugin.h first: this file implements the
 * contract described there, and the three rules at the top of that header
 * (no bypassing the interlocking, fail-safe default on every input, and a
 * plugin is *described* rather than executing) are what the code below is
 * arranged around.
 *
 * The per-tick order matters:
 *
 *     1. heartbeat check   - a plugin that has gone quiet is failed FIRST,
 *                            so nothing downstream acts on stale data
 *     2. inputs            - readings propagate through rw_sensor_apply(),
 *                            which is the same path a real field bus uses
 *     3. outputs           - commands go through the public API and can be
 *                            refused, exactly as an operator's would be
 *
 * Doing the heartbeat first is the whole point: an input that is 200 ms old
 * is not a safety input.
 * ========================================================================== */
#include "mcu_plugin.h"
#include "railway_internal.h"
#include "railway_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Registry
 * -------------------------------------------------------------------------- */
static McuPlugin g_plugins[MCU_MAX_PLUGINS];
static size_t g_plugin_count = 0;
static McuSettings g_settings;
static bool g_settings_ready = false;
static RwId g_next_plugin_id = RW_ID_NONE;
static RwId g_next_binding_id = RW_ID_NONE;

static McuPlugin *plugin_by_id(RwId plugin_id)
{
    size_t i;

    if (plugin_id == RW_ID_NONE)
    {
        return NULL;
    }
    for (i = 0; i < g_plugin_count; ++i)
    {
        if (g_plugins[i].id == plugin_id)
        {
            return &g_plugins[i];
        }
    }
    return NULL;
}

/** Locate the plugin that owns a binding id, and the binding itself. */
static McuPlugin *plugin_of_binding(RwId binding_id, McuBinding **out)
{
    size_t i;
    size_t b;

    if (out != NULL)
    {
        *out = NULL;
    }
    for (i = 0; i < g_plugin_count; ++i)
    {
        for (b = 0; b < g_plugins[i].binding_count; ++b)
        {
            if (g_plugins[i].bindings[b].id == binding_id)
            {
                if (out != NULL)
                {
                    *out = &g_plugins[i].bindings[b];
                }
                return &g_plugins[i];
            }
        }
    }
    return NULL;
}

static McuBinding *binding_of_plugin_on_pin(McuPlugin *plugin, int pin)
{
    size_t b;

    if (plugin == NULL)
    {
        return NULL;
    }
    for (b = 0; b < plugin->binding_count; ++b)
    {
        if (plugin->bindings[b].pin == pin)
        {
            return &plugin->bindings[b];
        }
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * Board / bus / binding labels
 * -------------------------------------------------------------------------- */
const char *mcu_board_name(McuBoard board)
{
    switch (board)
    {
    case MCU_BOARD_ARDUINO_UNO:
        return "Arduino Uno";
    case MCU_BOARD_ARDUINO_MEGA:
        return "Arduino Mega 2560";
    case MCU_BOARD_ESP32:
        return "Espressif ESP32";
    case MCU_BOARD_ESP8266:
        return "Espressif ESP8266";
    case MCU_BOARD_STM32:
        return "ST Microelectronics STM32";
    case MCU_BOARD_RP2040:
        return "Raspberry Pi Pico (RP2040)";
    case MCU_BOARD_TEENSY:
        return "PJRC Teensy";
    case MCU_BOARD_PLC_INDUSTRIAL:
        return "Industrial PLC";
    case MCU_BOARD_SIMULATOR:
        return "Software simulator";
    case MCU_BOARD_UNKNOWN:
        break;
    }
    return "Unknown board";
}

const char *mcu_board_code(McuBoard board)
{
    switch (board)
    {
    case MCU_BOARD_ARDUINO_UNO:
        return "UNO";
    case MCU_BOARD_ARDUINO_MEGA:
        return "MEGA";
    case MCU_BOARD_ESP32:
        return "ESP32";
    case MCU_BOARD_ESP8266:
        return "ESP8266";
    case MCU_BOARD_STM32:
        return "STM32";
    case MCU_BOARD_RP2040:
        return "RP2040";
    case MCU_BOARD_TEENSY:
        return "TEENSY";
    case MCU_BOARD_PLC_INDUSTRIAL:
        return "PLC";
    case MCU_BOARD_SIMULATOR:
        return "SIM";
    case MCU_BOARD_UNKNOWN:
        break;
    }
    return "?";
}

float mcu_board_voltage(McuBoard board)
{
    switch (board)
    {
    /* The AVR boards run on 5 V; a 3.3 V peripheral on a 5 V bus is the
     * classic way to let the smoke out, so this is worth modelling. */
    case MCU_BOARD_ARDUINO_UNO:
    case MCU_BOARD_ARDUINO_MEGA:
        return 5.0f;
    case MCU_BOARD_ESP32:
    case MCU_BOARD_ESP8266:
    case MCU_BOARD_STM32:
    case MCU_BOARD_RP2040:
    case MCU_BOARD_TEENSY:
        return 3.3f;
    case MCU_BOARD_PLC_INDUSTRIAL:
        return 24.0f;
    default:
        break;
    }
    return 0.0f;
}

int mcu_board_max_pins(McuBoard board)
{
    switch (board)
    {
    case MCU_BOARD_ARDUINO_UNO:
        return 20;
    case MCU_BOARD_ARDUINO_MEGA:
        return 70;
    case MCU_BOARD_ESP32:
        return 34;
    case MCU_BOARD_ESP8266:
        return 17;
    case MCU_BOARD_STM32:
        return 50;
    case MCU_BOARD_RP2040:
        return 30;
    case MCU_BOARD_TEENSY:
        return 40;
    case MCU_BOARD_PLC_INDUSTRIAL:
        return 128;
    case MCU_BOARD_SIMULATOR:
        return 256;
    default:
        break;
    }
    return 0;
}

bool mcu_board_has_can(McuBoard board)
{
    return board == MCU_BOARD_ESP32 || board == MCU_BOARD_STM32 || board == MCU_BOARD_TEENSY || board == MCU_BOARD_PLC_INDUSTRIAL;
}

const char *mcu_bus_name(McuBus bus)
{
    switch (bus)
    {
    case MCU_BUS_UART:
        return "UART";
    case MCU_BUS_I2C:
        return "I2C";
    case MCU_BUS_SPI:
        return "SPI";
    case MCU_BUS_CAN:
        return "CAN";
    case MCU_BUS_WIFI_TCP:
        return "WiFi/TCP";
    case MCU_BUS_GPIO:
        return "GPIO";
    case MCU_BUS_NONE:
        break;
    }
    return "none";
}

bool mcu_bus_is_networked(McuBus bus)
{
    return bus == MCU_BUS_WIFI_TCP || bus == MCU_BUS_CAN;
}

const char *mcu_state_name(McuState state)
{
    switch (state)
    {
    case MCU_STATE_UNLOADED:
        return "UNLOADED";
    case MCU_STATE_LOADED:
        return "LOADED";
    case MCU_STATE_RUNNING:
        return "RUNNING";
    case MCU_STATE_DEGRADED:
        return "DEGRADED";
    case MCU_STATE_STALE:
        return "STALE";
    case MCU_STATE_FAULTED:
        return "FAULTED";
    }
    return "UNKNOWN";
}

const char *mcu_binding_kind_name(McuBindingKind kind)
{
    switch (kind)
    {
    case MCU_BIND_TRACK_CIRCUIT:
        return "track-circuit";
    case MCU_BIND_AXLE_COUNTER:
        return "axle-counter";
    case MCU_BIND_TREADLE:
        return "treadle";
    case MCU_BIND_HOT_BOX:
        return "hot-box";
    case MCU_BIND_LEVEL_CROSSING:
        return "level-crossing";
    case MCU_BIND_POINT_DETECTION:
        return "point-detection";
    case MCU_BIND_SIGNAL_REPEATER:
        return "signal-repeater";
    case MCU_BIND_SIGNAL_LAMP:
        return "signal-lamp";
    case MCU_BIND_POINT_DRIVE:
        return "point-drive";
    case MCU_BIND_BARRIER_MOTOR:
        return "barrier-motor";
    case MCU_BIND_INDICATOR_LAMP:
        return "indicator-lamp";
    case MCU_BIND_ALARM_SOUNDER:
        return "alarm-sounder";
    case MCU_BIND_NONE:
        break;
    }
    return "unbound";
}

bool mcu_binding_is_input(McuBindingKind kind)
{
    return kind == MCU_BIND_TRACK_CIRCUIT || kind == MCU_BIND_AXLE_COUNTER || kind == MCU_BIND_TREADLE || kind == MCU_BIND_HOT_BOX || kind == MCU_BIND_LEVEL_CROSSING || kind == MCU_BIND_POINT_DETECTION || kind == MCU_BIND_SIGNAL_REPEATER;
}

bool mcu_binding_is_output(McuBindingKind kind)
{
    return kind == MCU_BIND_SIGNAL_LAMP || kind == MCU_BIND_POINT_DRIVE || kind == MCU_BIND_BARRIER_MOTOR || kind == MCU_BIND_INDICATOR_LAMP || kind == MCU_BIND_ALARM_SOUNDER;
}

/**
 * The capability a binding requires. The loader checks this against the
 * plugin's declared capability bitmask, so a board cannot drive a point
 * unless it said up front that it would.
 */
McuCapability mcu_binding_required_capability(McuBindingKind kind)
{
    switch (kind)
    {
    case MCU_BIND_TRACK_CIRCUIT:
        return MCU_CAP_READ_TRACK_CIRCUIT;
    case MCU_BIND_AXLE_COUNTER:
        return MCU_CAP_READ_AXLE_COUNTER;
    case MCU_BIND_TREADLE:
        return MCU_CAP_READ_DIGITAL;
    case MCU_BIND_HOT_BOX:
        return MCU_CAP_READ_HOT_BOX;
    case MCU_BIND_LEVEL_CROSSING:
        return MCU_CAP_READ_DIGITAL;
    case MCU_BIND_POINT_DETECTION:
        return MCU_CAP_READ_DIGITAL;
    case MCU_BIND_SIGNAL_REPEATER:
        return MCU_CAP_READ_DIGITAL;
    case MCU_BIND_SIGNAL_LAMP:
        return MCU_CAP_CONTROL_SIGNAL;
    case MCU_BIND_POINT_DRIVE:
        return MCU_CAP_CONTROL_POINT;
    case MCU_BIND_BARRIER_MOTOR:
        return MCU_CAP_CONTROL_LEVEL_CROSSING;
    case MCU_BIND_INDICATOR_LAMP:
        return MCU_CAP_WRITE_DIGITAL;
    case MCU_BIND_ALARM_SOUNDER:
        return MCU_CAP_WRITE_DIGITAL;
    case MCU_BIND_NONE:
        break;
    }
    return MCU_CAP_NONE;
}

const char *mcu_capability_name(McuCapability capability)
{
    switch (capability)
    {
    case MCU_CAP_READ_DIGITAL:
        return "read-digital";
    case MCU_CAP_READ_ANALOG:
        return "read-analog";
    case MCU_CAP_WRITE_DIGITAL:
        return "write-digital";
    case MCU_CAP_WRITE_ANALOG:
        return "write-analog";
    case MCU_CAP_READ_TRACK_CIRCUIT:
        return "read-track-circuit";
    case MCU_CAP_READ_AXLE_COUNTER:
        return "read-axle-counter";
    case MCU_CAP_READ_HOT_BOX:
        return "read-hot-box";
    case MCU_CAP_CONTROL_LEVEL_CROSSING:
        return "control-level-crossing";
    case MCU_CAP_CONTROL_POINT:
        return "control-point";
    case MCU_CAP_CONTROL_SIGNAL:
        return "control-signal";
    case MCU_CAP_HEARTBEAT:
        return "heartbeat";
    case MCU_CAP_NONE:
        break;
    }
    return "none";
}

/* --------------------------------------------------------------------------
 * Settings
 * -------------------------------------------------------------------------- */
static void settings_defaults(McuSettings *s)
{
    memset(s, 0, sizeof(*s));

    s->enabled = true;
    /* Point and signal control are OFF by default. A board that has not been
     * explicitly trusted should be able to report, not to command. */
    s->allow_point_control = false;
    s->allow_signal_control = false;
    s->allow_track_circuit_input = true; /* reporting occupancy is the safe use */
    s->fail_safe_on_timeout = true;
    s->dry_run = false;
    s->default_timeout_ms = 3000;
    s->poll_interval_ms = 200;
    s->log_level = 1;
}

McuSettings *mcu_settings(void)
{
    if (!g_settings_ready)
    {
        settings_defaults(&g_settings);
        g_settings_ready = true;
    }
    return &g_settings;
}

RwResult mcu_apply_settings(const McuSettings *settings)
{
    McuSettings *current = mcu_settings();

    if (settings == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }
    *current = *settings;

    /* Fail-safe on timeout is not a preference. A system that lets an
     * operator disable it is not a safety system - the same reasoning as
     * RcSettings.fail_safe_unknown. */
    if (!current->fail_safe_on_timeout)
    {
        current->fail_safe_on_timeout = true;
        rw_event(SEVERITY_WARNING, "PLUGIN",
                 "fail_safe_on_timeout cannot be disabled - forcing it on");
    }
    if (current->default_timeout_ms < 100)
    {
        current->default_timeout_ms = 100;
    }
    if (current->poll_interval_ms < 20)
    {
        current->poll_interval_ms = 20;
    }
    rw_bump_revision();
    return RW_OK;
}

const char *mcu_settings_summary(void)
{
    static char summary[MCU_MAX_TEXT];
    const McuSettings *s = mcu_settings();

    snprintf(summary, sizeof(summary),
             "%s | points %s | signals %s | timeout %ums | %s",
             s->enabled ? "enabled" : "disabled",
             s->allow_point_control ? "allowed" : "blocked",
             s->allow_signal_control ? "allowed" : "blocked",
             s->default_timeout_ms,
             s->dry_run ? "DRY RUN" : "live");
    return summary;
}

/* --------------------------------------------------------------------------
 * Registry
 * -------------------------------------------------------------------------- */
void mcu_init(void)
{
    memset(g_plugins, 0, sizeof(g_plugins));
    g_plugin_count = 0;
    g_next_plugin_id = 1;
    g_next_binding_id = 1;
    (void)mcu_settings();
    rw_event(SEVERITY_INFO, "PLUGIN", "Microcontroller plugin registry initialised");
}

void mcu_shutdown(void)
{
    size_t i;

    /* Drive every output to its safe state before letting go, so a lamp or a
     * point does not keep the last commanded value after the driver is gone. */
    for (i = 0; i < g_plugin_count; ++i)
    {
        (void)mcu_apply_safe_state(g_plugins[i].id);
    }
    memset(g_plugins, 0, sizeof(g_plugins));
    g_plugin_count = 0;
}

static bool declaration_valid(const McuPlugin *plugin, const char **why)
{
    if (plugin == NULL)
    {
        if (why)
            *why = "null plugin";
        return false;
    }
    if (plugin->name[0] == '\0')
    {
        if (why)
            *why = "missing name";
        return false;
    }
    if (plugin->board == MCU_BOARD_UNKNOWN)
    {
        if (why)
            *why = "unknown board";
        return false;
    }
    if (plugin->bus == MCU_BUS_NONE)
    {
        if (why)
            *why = "no bus declared";
        return false;
    }
    if (plugin->bus == MCU_BUS_CAN && !mcu_board_has_can(plugin->board))
    {
        if (why)
            *why = "board has no CAN controller";
        return false;
    }
    if (plugin->capabilities == MCU_CAP_NONE)
    {
        if (why)
            *why = "no capabilities declared";
        return false;
    }
    return true;
}

RwId mcu_add(const McuPlugin *plugin)
{
    McuPlugin *slot;
    const char *why = "";

    if (!declaration_valid(plugin, &why))
    {
        rw_event(SEVERITY_WARNING, "PLUGIN",
                 "Plugin rejected: %s", why);
        return RW_ID_NONE;
    }
    if (g_plugin_count >= MCU_MAX_PLUGINS)
    {
        rw_event(SEVERITY_WARNING, "PLUGIN",
                 "Plugin registry full (%d) - %s not loaded",
                 MCU_MAX_PLUGINS, plugin->name);
        return RW_ID_NONE;
    }

    slot = &g_plugins[g_plugin_count++];
    *slot = *plugin;
    slot->id = g_next_plugin_id++;
    slot->binding_count = 0;
    slot->state = plugin->enabled ? MCU_STATE_LOADED : MCU_STATE_UNLOADED;
    slot->loaded_at_ms = (unsigned)rw_engine()->clock_ms;
    slot->last_seen_ms = slot->loaded_at_ms;
    slot->missed_heartbeats = 0;
    slot->reads = 0;
    slot->writes = 0;
    slot->refusals = 0;
    slot->faults = 0;
    slot->timeouts = 0;

    if (slot->heartbeat_ms == 0)
    {
        /* Default the expected heartbeat to a third of the timeout: three
         * missed beats is a timeout. */
        const unsigned timeout = slot->timeout_ms > 0
                                     ? slot->timeout_ms
                                     : mcu_settings()->default_timeout_ms;
        slot->timeout_ms = timeout;
        slot->heartbeat_ms = timeout / 3;
        if (slot->heartbeat_ms < 50)
        {
            slot->heartbeat_ms = 50;
        }
    }
    if (slot->timeout_ms == 0)
    {
        slot->timeout_ms = mcu_settings()->default_timeout_ms;
    }

    rw_event(SEVERITY_INFO, "PLUGIN",
             "Loaded %s (%s over %s) with %d capability flag(s)",
             slot->name, mcu_board_code(slot->board), mcu_bus_name(slot->bus),
             (int)slot->capabilities);
    rw_bump_revision();
    return slot->id;
}

RwResult mcu_remove(RwId plugin_id)
{
    McuPlugin *plugin = plugin_by_id(plugin_id);
    size_t index;

    if (plugin == NULL)
    {
        return RW_ERR_INVALID_ID;
    }

    (void)mcu_apply_safe_state(plugin_id);

    index = (size_t)(plugin - g_plugins);
    if (index + 1 < g_plugin_count)
    {
        memmove(&g_plugins[index], &g_plugins[index + 1],
                (g_plugin_count - index - 1) * sizeof(g_plugins[0]));
    }
    g_plugin_count--;

    rw_event(SEVERITY_INFO, "PLUGIN", "Plugin %u unloaded (outputs to safe state)",
             (unsigned)plugin_id);
    rw_bump_revision();
    return RW_OK;
}

RwResult mcu_set_enabled(RwId plugin_id, bool enabled)
{
    McuPlugin *plugin = plugin_by_id(plugin_id);

    if (plugin == NULL)
    {
        return RW_ERR_INVALID_ID;
    }
    plugin->enabled = enabled;
    if (!enabled)
    {
        (void)mcu_apply_safe_state(plugin_id);
        plugin->state = MCU_STATE_UNLOADED;
    }
    else
    {
        plugin->state = MCU_STATE_LOADED;
        plugin->missed_heartbeats = 0;
        plugin->last_seen_ms = (unsigned)rw_engine()->clock_ms;
    }
    rw_event(SEVERITY_INFO, "PLUGIN", "Plugin %s %s",
             plugin->name, enabled ? "enabled" : "disabled (safe state applied)");
    rw_bump_revision();
    return RW_OK;
}

/* --------------------------------------------------------------------------
 * Bindings
 * -------------------------------------------------------------------------- */
RwId mcu_bind(RwId plugin_id, McuBindingKind kind, RwId target, int pin,
              bool active_low, int safe_state)
{
    McuPlugin *plugin = plugin_by_id(plugin_id);
    McuBinding *binding;
    McuCapability needed;

    if (plugin == NULL)
    {
        return RW_ID_NONE;
    }
    if (kind == MCU_BIND_NONE)
    {
        return RW_ID_NONE;
    }
    if (!mcu_binding_is_input(kind) && !mcu_binding_is_output(kind))
    {
        return RW_ID_NONE;
    }
    if (pin < 0 || pin >= mcu_board_max_pins(plugin->board))
    {
        rw_event(SEVERITY_WARNING, "PLUGIN",
                 "%s has no pin %d (board maximum is %d)",
                 plugin->name, pin, mcu_board_max_pins(plugin->board));
        return RW_ID_NONE;
    }
    if (binding_of_plugin_on_pin(plugin, pin) != NULL)
    {
        rw_event(SEVERITY_WARNING, "PLUGIN",
                 "%s already has a binding on pin %d", plugin->name, pin);
        return RW_ID_NONE;
    }

    needed = mcu_binding_required_capability(kind);
    if ((plugin->capabilities & (unsigned)needed) == 0)
    {
        rw_event(SEVERITY_WARNING, "PLUGIN",
                 "%s cannot bind %s - it did not declare capability %s",
                 plugin->name, mcu_binding_kind_name(kind),
                 mcu_capability_name(needed));
        return RW_ID_NONE;
    }

    /* The system-wide gates. These are the operator's master switches: a
     * board may declare that it is *capable* of moving a point, but the site
     * settings still decide whether any plugin is *allowed* to. */
    if (kind == MCU_BIND_POINT_DRIVE && !mcu_settings()->allow_point_control)
    {
        rw_event(SEVERITY_WARNING, "PLUGIN",
                 "Refused %s point-drive binding - plugin point control is disabled",
                 plugin->name);
        return RW_ID_NONE;
    }
    if (kind == MCU_BIND_SIGNAL_LAMP && !mcu_settings()->allow_signal_control)
    {
        rw_event(SEVERITY_WARNING, "PLUGIN",
                 "Refused %s signal-lamp binding - plugin signal control is disabled",
                 plugin->name);
        return RW_ID_NONE;
    }
    if (kind == MCU_BIND_TRACK_CIRCUIT && !mcu_settings()->allow_track_circuit_input)
    {
        rw_event(SEVERITY_WARNING, "PLUGIN",
                 "Refused %s track-circuit binding - plugin occupancy input is disabled",
                 plugin->name);
        return RW_ID_NONE;
    }

    binding = &plugin->bindings[plugin->binding_count++];
    memset(binding, 0, sizeof(*binding));
    binding->id = g_next_binding_id++;
    binding->kind = kind;
    binding->target = target;
    binding->pin = pin;
    binding->active_low = active_low;
    binding->safe_state = safe_state;
    binding->value = safe_state;
    binding->stale = false;
    binding->last_update_ms = (unsigned)rw_engine()->clock_ms;

    if (mcu_binding_is_input(kind))
    {
        plugin->state = MCU_STATE_RUNNING;
    }
    rw_bump_revision();
    return binding->id;
}

RwResult mcu_unbind(RwId binding_id)
{
    McuPlugin *plugin = plugin_of_binding(binding_id, NULL);
    size_t index;

    if (plugin == NULL)
    {
        return RW_ERR_INVALID_ID;
    }
    for (index = 0; index < plugin->binding_count; ++index)
    {
        if (plugin->bindings[index].id == binding_id)
        {
            if (index + 1 < plugin->binding_count)
            {
                memmove(&plugin->bindings[index], &plugin->bindings[index + 1],
                        (plugin->binding_count - index - 1) * sizeof(plugin->bindings[0]));
            }
            plugin->binding_count--;
            rw_bump_revision();
            return RW_OK;
        }
    }
    return RW_ERR_INVALID_ID;
}

RwResult mcu_get_binding(RwId binding_id, McuBinding *out)
{
    McuBinding *binding = NULL;

    if (out == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }
    if (plugin_of_binding(binding_id, &binding) == NULL || binding == NULL)
    {
        return RW_ERR_INVALID_ID;
    }
    *out = *binding;
    return RW_OK;
}

int mcu_binding_count(RwId plugin_id)
{
    McuPlugin *plugin = plugin_by_id(plugin_id);

    return plugin != NULL ? (int)plugin->binding_count : 0;
}

/* --------------------------------------------------------------------------
 * Safe state
 *
 * This is the fail-safe path and it is deliberately blunt: every output is
 * pushed to its declared safe value and every input is marked stale so the
 * tick loop will not act on it. Signals go to danger, barriers go down,
 * sounders go off - the defaults that fail towards safety.
 * -------------------------------------------------------------------------- */
RwResult mcu_apply_safe_state(RwId plugin_id)
{
    McuPlugin *plugin = plugin_by_id(plugin_id);
    size_t b;

    if (plugin == NULL)
    {
        return RW_ERR_INVALID_ID;
    }

    for (b = 0; b < plugin->binding_count; ++b)
    {
        McuBinding *binding = &plugin->bindings[b];

        binding->stale = true;
        binding->value = binding->safe_state;

        if (!mcu_binding_is_output(binding->kind))
        {
            continue;
        }
        switch (binding->kind) {
        case MCU_BIND_SIGNAL_LAMP:
            /* The safe aspect is always danger. */
            if (binding->target != RW_ID_NONE) {
                binding->value = (int)SIGNAL_RED;
            }
            break;
        case MCU_BIND_POINT_DRIVE:
            /* A point is left where it is on failure: moving it under a train
             * is exactly what INV-3 forbids. The interlocking withdraws it. */
            break;
        case MCU_BIND_BARRIER_MOTOR:
            /* Barriers fail DOWN - the road is protected, not the railway. */
            binding->value = 1;
            break;
        case MCU_BIND_ALARM_SOUNDER:
            /* A sounder fails ON: a silent failure is worse than a noisy one. */
            binding->value = 1;
            break;
        case MCU_BIND_INDICATOR_LAMP:
        default:
            binding->value = binding->safe_state;
            break;
        }
    }

    if (plugin->state != MCU_STATE_UNLOADED)
    {
        plugin->state = MCU_STATE_STALE;
    }
    return RW_OK;
}

int mcu_apply_safe_state_all(void)
{
    size_t i;
    int affected = 0;

    for (i = 0; i < g_plugin_count; ++i)
    {
        if (mcu_apply_safe_state(g_plugins[i].id) == RW_OK)
        {
            affected++;
        }
    }
    if (affected > 0)
    {
        rw_event(SEVERITY_WARNING, "PLUGIN",
                 "Safe state applied to %d plugin(s)", affected);
    }
    return affected;
}

/* --------------------------------------------------------------------------
 * Liveness
 * -------------------------------------------------------------------------- */
RwResult mcu_heartbeat(RwId plugin_id)
{
    McuPlugin *plugin = plugin_by_id(plugin_id);

    if (plugin == NULL)
    {
        return RW_ERR_INVALID_ID;
    }
    plugin->last_seen_ms = (unsigned)rw_engine()->clock_ms;
    plugin->missed_heartbeats = 0;

    if (plugin->state == MCU_STATE_STALE)
    {
        rw_event(SEVERITY_INFO, "PLUGIN",
                 "%s recovered - inputs trusted again", plugin->name);
        plugin->state = plugin->enabled ? MCU_STATE_RUNNING : MCU_STATE_LOADED;
    }
    return RW_OK;
}

/**
 * Check one plugin's liveness. Returns true when the plugin is stale, so the
 * caller can skip its inputs for this tick.
 */
static bool check_heartbeat(McuPlugin *plugin, unsigned now_ms)
{
    if (plugin == NULL || !plugin->enabled)
    {
        return true;
    }
    if (elapsed_exceeded(now_ms, plugin->last_seen_ms, plugin->timeout_ms))
    {
        if (plugin->state != MCU_STATE_STALE)
        {
            plugin->timeouts++;
            rw_event(SEVERITY_ALARM, "PLUGIN",
                     "%s timed out after %ums - inputs forced to SAFE state",
                     plugin->name, plugin->timeout_ms);
            if (mcu_settings()->fail_safe_on_timeout)
            {
                (void)mcu_apply_safe_state(plugin->id);
            }
        }
        plugin->state = MCU_STATE_STALE;
        return true;
    }
    return false;
}

/* --------------------------------------------------------------------------
 * Inputs
 * -------------------------------------------------------------------------- */
RwResult mcu_feed_input(RwId plugin_id, int pin, int value, bool triggered)
{
    McuPlugin *plugin = plugin_by_id(plugin_id);
    McuBinding *binding;
    int effective;

    if (plugin == NULL)
    {
        return RW_ERR_INVALID_ID;
    }
    if (!plugin->enabled)
    {
        return RW_ERR_OUT_OF_SERVICE;
    }
    if (plugin->state == MCU_STATE_STALE)
    {
        /* A stale plugin's readings are not trustworthy. Note the reading but
         * do not propagate it - INV-5 by another name. */
        return RW_ERR_OUT_OF_SERVICE;
    }

    binding = binding_of_plugin_on_pin(plugin, pin);
    if (binding == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }
    if (!mcu_binding_is_input(binding->kind))
    {
        return RW_ERR_INVALID_ARG;
    }

    effective = binding->active_low ? -value : value;
    if (binding->value != effective)
    {
        binding->transitions++;
    }
    binding->value = effective;
    binding->stale = false;
    binding->last_update_ms = (unsigned)rw_engine()->clock_ms;
    plugin->reads++;

    /* Propagate. Everything below this line is the engine's own fail-safe
     * handling - the plugin gets no special path. */
    switch (binding->kind)
    {
    case MCU_BIND_TRACK_CIRCUIT:
        (void)rw_sensor_apply_routed(plugin, binding, TRACK_CIRCUIT_SENSOR,
                                     triggered ? 1 : 0,
                                     triggered ? SENSOR_TRIGGERED : SENSOR_OK);
        break;
    case MCU_BIND_AXLE_COUNTER:
        (void)rw_sensor_apply_routed(plugin, binding, AXLE_COUNTER_SENSOR,
                                     effective,
                                     triggered ? SENSOR_TRIGGERED : SENSOR_OK);
        break;
    case MCU_BIND_HOT_BOX:
        (void)rw_sensor_apply_routed(plugin, binding, HOT_BOX_SENSOR,
                                     effective, SENSOR_OK);
        break;
    case MCU_BIND_TREADLE:
        (void)rw_sensor_apply_routed(plugin, binding, TREADLE_SENSOR,
                                     triggered ? 1 : 0,
                                     triggered ? SENSOR_TRIGGERED : SENSOR_OK);
        break;
    case MCU_BIND_LEVEL_CROSSING:
        (void)rw_sensor_apply_routed(plugin, binding, LEVEL_CROSSING_SENSOR,
                                     effective, SENSOR_OK);
        break;
    case MCU_BIND_POINT_DETECTION:
    case MCU_BIND_SIGNAL_REPEATER:
        /* Reporting inputs: recorded for the diagram, but they do not drive
         * the interlocking. Point detection that disagrees with the commanded
         * lie is an alarm for the signaller, not something a plugin may
         * assert. */
        break;
    default:
        break;
    }
    return RW_OK;
}

/* --------------------------------------------------------------------------
 * Outputs
 *
 * Every command here goes through the public API, so the interlocking can
 * refuse it. A refusal is counted, not ignored - a board repeatedly asking
 * for something the interlocking will not grant is a fault worth seeing.
 * -------------------------------------------------------------------------- */
static void refresh_output(McuPlugin *plugin, McuBinding *binding)
{
    RwResult result = RW_OK;

    if (binding->stale || !plugin->enabled)
    {
        return;
    }
    if (mcu_settings()->dry_run)
    {
        return;
    }

    switch (binding->kind)
    {
    case MCU_BIND_SIGNAL_LAMP:
        if (binding->target != RW_ID_NONE)
        {
            RwSignalInfo info;
            if (railway_get_signal_info((int)binding->target, &info) == RW_OK)
            {
                if (info.aspect != (SignalState)binding->value)
                {
                    binding->value = (int)info.aspect;
                    binding->transitions++;
                    plugin->writes++;
                }
            }
        }
        break;

    case MCU_BIND_POINT_DRIVE:
        if (binding->target != RW_ID_NONE)
        {
            result = railway_set_switch((int)binding->target,
                                        (SwitchPosition)binding->value);
        }
        break;

    case MCU_BIND_BARRIER_MOTOR:
    case MCU_BIND_INDICATOR_LAMP:
    case MCU_BIND_ALARM_SOUNDER:
        /* Driven mirrors: the engine tells the board, the board does not tell
         * the engine. Nothing to command back. */
        break;

    default:
        break;
    }

    if (result == RW_ERR_INTERLOCK || result == RW_ERR_OCCUPIED || result == RW_ERR_POINT_LOCKED || result == RW_ERR_EMERGENCY || result == RW_ERR_OUT_OF_SERVICE)
    {
        plugin->refusals++;
        binding->stale = true; /* stop hammering a refusal every tick */
        rw_event(SEVERITY_WARNING, "PLUGIN",
                 "%s command on %s refused by the interlocking (%s)",
                 plugin->name, mcu_binding_kind_name(binding->kind),
                 rw_result_string(result));
    }
}

/* --------------------------------------------------------------------------
 * Per-tick
 * -------------------------------------------------------------------------- */
void mcu_tick(unsigned delta_ms)
{
    const unsigned now_ms = (unsigned)rw_engine()->clock_ms;
    size_t i;
    size_t b;
    static unsigned since_poll = 0;

    (void)delta_ms;

    if (!mcu_settings()->enabled || g_plugin_count == 0)
    {
        return;
    }

    /* 1. Liveness first: a plugin that has gone quiet must not have its
     *    inputs acted on this tick. */
    for (i = 0; i < g_plugin_count; ++i)
    {
        (void)check_heartbeat(&g_plugins[i], now_ms);
    }

    /* 2. Outputs are refreshed on the poll interval, not every tick - a bus
     *    has finite bandwidth and a real field bus would be polled. */
    since_poll += delta_ms;
    if (since_poll < mcu_settings()->poll_interval_ms)
    {
        return;
    }
    since_poll = 0;

    for (i = 0; i < g_plugin_count; ++i)
    {
        McuPlugin *plugin = &g_plugins[i];

        if (!plugin->enabled || plugin->state == MCU_STATE_UNLOADED)
        {
            continue;
        }

        for (b = 0; b < plugin->binding_count; ++b)
        {
            McuBinding *binding = &plugin->bindings[b];

            if (mcu_binding_is_output(binding->kind))
            {
                refresh_output(plugin, binding);
            }

            /* An input that has not been refreshed within the timeout is
             * marked stale so the diagram can show it as unproven. */
            if (mcu_binding_is_input(binding->kind) && !binding->stale)
            {
                if (now_ms - binding->last_update_ms > plugin->timeout_ms)
                {
                    binding->stale = true;
                    binding->value = binding->safe_state;
                }
            }
        }

        if (plugin->state == MCU_STATE_RUNNING && plugin->binding_count == 0)
        {
            plugin->state = MCU_STATE_LOADED;
        }
    }
}

/* --------------------------------------------------------------------------
 * Inspection
 * -------------------------------------------------------------------------- */
int mcu_count(void)
{
    return (int)g_plugin_count;
}

RwResult mcu_get(RwId plugin_id, McuPlugin *out)
{
    McuPlugin *plugin = plugin_by_id(plugin_id);

    if (out == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }
    if (plugin == NULL)
    {
        return RW_ERR_INVALID_ID;
    }
    *out = *plugin;
    return RW_OK;
}

RwResult mcu_get_at(int index, McuPlugin *out)
{
    if (out == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }
    if (index < 0 || (size_t)index >= g_plugin_count)
    {
        return RW_ERR_INVALID_ID;
    }
    *out = g_plugins[index];
    return RW_OK;
}

RwId mcu_find(const char *name)
{
    size_t i;

    if (name == NULL || name[0] == '\0')
    {
        return RW_ID_NONE;
    }
    for (i = 0; i < g_plugin_count; ++i)
    {
        if (strcmp(g_plugins[i].name, name) == 0)
        {
            return g_plugins[i].id;
        }
    }
    return RW_ID_NONE;
}

RwResult mcu_get_stats(McuStats *stats)
{
    size_t i;

    if (stats == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }
    memset(stats, 0, sizeof(*stats));

    for (i = 0; i < g_plugin_count; ++i)
    {
        const McuPlugin *plugin = &g_plugins[i];

        stats->plugins_total++;
        stats->bindings_total += (int)plugin->binding_count;
        stats->total_reads += plugin->reads;
        stats->total_writes += plugin->writes;
        stats->total_refusals += plugin->refusals;
        stats->total_timeouts += plugin->timeouts;

        switch (plugin->state)
        {
        case MCU_STATE_RUNNING:
            stats->plugins_running++;
            break;
        case MCU_STATE_STALE:
            stats->plugins_stale++;
            break;
        case MCU_STATE_FAULTED:
            stats->plugins_faulted++;
            break;
        default:
            break;
        }
    }
    return RW_OK;
}

RwResult mcu_describe(RwId plugin_id, char *buffer, size_t size)
{
    McuPlugin *plugin = plugin_by_id(plugin_id);
    int stale = 0;
    size_t b;

    if (buffer == NULL || size == 0)
    {
        return RW_ERR_INVALID_ARG;
    }
    if (plugin == NULL)
    {
        return RW_ERR_INVALID_ID;
    }
    for (b = 0; b < plugin->binding_count; ++b)
    {
        if (plugin->bindings[b].stale)
        {
            stale++;
        }
    }

    snprintf(buffer, size, "%-14s [%-8s] %-6s over %-7s addr %d - %d binding(s), %d stale",
             plugin->name, mcu_state_name(plugin->state),
             mcu_board_code(plugin->board), mcu_bus_name(plugin->bus),
             plugin->address, (int)plugin->binding_count, stale);
    return RW_OK;
}

/* --------------------------------------------------------------------------
 * Persistence - the same INI style as the settings and CONJOB files.
 * -------------------------------------------------------------------------- */
RwResult mcu_save(const char *path)
{
    FILE *file;
    size_t i;
    size_t b;

    if (path == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }
    file = fopen(path, "w");
    if (file == NULL)
    {
        return RW_ERR_IO;
    }

    fprintf(file, "# RailControl microcontroller plugin registry\n");
    fprintf(file, "[settings]\n");
    {
        const McuSettings *s = mcu_settings();
        fprintf(file, "enabled = %s\n", s->enabled ? "true" : "false");
        fprintf(file, "allow_point_control = %s\n", s->allow_point_control ? "true" : "false");
        fprintf(file, "allow_signal_control = %s\n", s->allow_signal_control ? "true" : "false");
        fprintf(file, "allow_track_circuit_input = %s\n", s->allow_track_circuit_input ? "true" : "false");
        fprintf(file, "fail_safe_on_timeout = %s\n", s->fail_safe_on_timeout ? "true" : "false");
        fprintf(file, "dry_run = %s\n", s->dry_run ? "true" : "false");
        fprintf(file, "default_timeout_ms = %u\n", s->default_timeout_ms);
        fprintf(file, "poll_interval_ms = %u\n", s->poll_interval_ms);
    }

    for (i = 0; i < g_plugin_count; ++i)
    {
        const McuPlugin *plugin = &g_plugins[i];

        fprintf(file, "\n[plugin.%u]\n", (unsigned)plugin->id);
        fprintf(file, "name = %s\n", plugin->name);
        fprintf(file, "description = %s\n", plugin->description);
        fprintf(file, "board = %s\n", mcu_board_code(plugin->board));
        fprintf(file, "bus = %s\n", mcu_bus_name(plugin->bus));
        fprintf(file, "device = %s\n", plugin->device);
        fprintf(file, "firmware = %s\n", plugin->firmware);
        fprintf(file, "address = %d\n", plugin->address);
        fprintf(file, "capabilities = %u\n", plugin->capabilities);
        fprintf(file, "enabled = %s\n", plugin->enabled ? "true" : "false");
        fprintf(file, "heartbeat_ms = %u\n", plugin->heartbeat_ms);
        fprintf(file, "timeout_ms = %u\n", plugin->timeout_ms);

        for (b = 0; b < plugin->binding_count; ++b)
        {
            const McuBinding *binding = &plugin->bindings[b];
            fprintf(file, "binding.%d = %s,%u,%d,%d,%d\n",
                    (int)b,
                    mcu_binding_kind_name(binding->kind),
                    (unsigned)binding->target,
                    binding->pin,
                    binding->active_low ? 1 : 0,
                    binding->safe_state);
        }
    }

    if (fclose(file) != 0)
    {
        return RW_ERR_IO;
    }
    return RW_OK;
}

RwResult mcu_load(const char *path)
{
    FILE *file;
    char line[512];
    McuPlugin candidate;
    bool have_candidate = false;

    if (path == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }
    file = fopen(path, "r");
    if (file == NULL)
    {
        return RW_ERR_IO;
    }

    memset(&candidate, 0, sizeof(candidate));
    candidate.enabled = true;

    while (fgets(line, (int)sizeof(line), file) != NULL)
    {
        char *cursor = line;
        char *equals;
        char *key;
        char *value;

        /* Trim. */
        while (*cursor == ' ' || *cursor == '\t')
            cursor++;
        {
            size_t len = strlen(cursor);
            while (len > 0 && (cursor[len - 1] == '\n' || cursor[len - 1] == '\r' || cursor[len - 1] == ' '))
            {
                cursor[--len] = '\0';
            }
        }
        if (*cursor == '\0' || *cursor == '#' || *cursor == ';')
        {
            continue;
        }
        if (*cursor == '[')
        {
            /* A new [plugin.N] section flushes the previous one. */
            if (have_candidate)
            {
                (void)mcu_add(&candidate);
                memset(&candidate, 0, sizeof(candidate));
                candidate.enabled = true;
                have_candidate = false;
            }
            continue;
        }

        equals = strchr(cursor, '=');
        if (equals == NULL)
        {
            continue;
        }
        *equals = '\0';
        key = cursor;
        value = equals + 1;
        {
            /* Trim the key and value. */
            char *kend = key + strlen(key);
            while (kend > key && (kend[-1] == ' ' || kend[-1] == '\t'))
                *--kend = '\0';
            while (*value == ' ' || *value == '\t')
                value++;
            {
                char *vend = value + strlen(value);
                while (vend > value && (vend[-1] == ' ' || vend[-1] == '\t'))
                    *--vend = '\0';
            }
        }

        if (strcmp(key, "name") == 0)
        {
            snprintf(candidate.name, sizeof(candidate.name), "%s", value);
            have_candidate = true;
        }
        else if (strcmp(key, "description") == 0)
        {
            snprintf(candidate.description, sizeof(candidate.description), "%s", value);
        }
        else if (strcmp(key, "device") == 0)
        {
            snprintf(candidate.device, sizeof(candidate.device), "%s", value);
        }
        else if (strcmp(key, "firmware") == 0)
        {
            snprintf(candidate.firmware, sizeof(candidate.firmware), "%s", value);
        }
        else if (strcmp(key, "board") == 0)
        {
            /* Match the short code from mcu_board_code(). */
            int b;
            for (b = 0; b <= (int)MCU_BOARD_SIMULATOR; ++b)
            {
                if (strcmp(mcu_board_code((McuBoard)b), value) == 0)
                {
                    candidate.board = (McuBoard)b;
                    break;
                }
            }
        }
        else if (strcmp(key, "bus") == 0)
        {
            int b;
            for (b = 0; b <= (int)MCU_BUS_GPIO; ++b)
            {
                if (strcmp(mcu_bus_name((McuBus)b), value) == 0)
                {
                    candidate.bus = (McuBus)b;
                    break;
                }
            }
        }
        else if (strcmp(key, "address") == 0)
        {
            candidate.address = atoi(value);
        }
        else if (strcmp(key, "capabilities") == 0)
        {
            candidate.capabilities = (unsigned)atoi(value);
        }
        else if (strcmp(key, "enabled") == 0)
        {
            candidate.enabled = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
        }
        else if (strcmp(key, "heartbeat_ms") == 0)
        {
            candidate.heartbeat_ms = (unsigned)atoi(value);
        }
        else if (strcmp(key, "timeout_ms") == 0)
        {
            candidate.timeout_ms = (unsigned)atoi(value);
        }
    }

    if (have_candidate)
    {
        (void)mcu_add(&candidate);
    }

    fclose(file);
    return RW_OK;
}
