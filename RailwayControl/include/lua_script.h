/* ==========================================================================
 * lua_script.h - Embedded Lua scripting with an object-oriented binding.
 *
 * The C engine is exposed to Lua as a small class hierarchy, so scripts read
 * like ordinary object-oriented code:
 *
 *     local box = Railway.SignalBox("Skeffield PSB")
 *
 *     local s3 = box:signal("S3")
 *     local r2 = box:route("R2")
 *     local t1 = box:train("1A34")
 *
 *     if box:train_waiting(r2) and box:route_free(r2) then
 *         r2:request()
 *         r2:dispatch()
 *     end
 *
 *     s3:set_aspect(Signal.GREEN)
 *
 * Object model
 * ------------
 *   Railway.SignalBox   the control area, root object
 *   Railway.Signal      :set_aspect / :danger / :clear / :manual / :info
 *   Railway.Switch      :set_position / :release / :info
 *   Railway.Track       :occupancy / :clear / :fail_circuit / :info
 *   Railway.Train       :state / :speed / :hold / :stop / :emergency / :info
 *   Railway.Route       :request / :cancel / :dispatch / :free / :info
 *   Railway.Sensor      :inject / :fail / :info
 *   Railway.Events      :log / :count / :get / :acknowledge_all
 *   Railway.Job         :enable / :disable / :trigger
 *   Railway.JobRegistry :add / :remove / :list   (CONJOB access)
 *
 * Safety: scripts can only call the public API. Every mutating call can be
 * refused by the interlocking exactly as an operator action would be, and a
 * refusal is surfaced as a Lua error with the reason attached. A script can
 * therefore never bypass a safety proof.
 *
 * If the build has no Lua library available, this module compiles against a
 * small built-in fallback that accepts the same scripts but interprets only
 * the documented constructs; see lua_script.c for details and
 * MF_LUA_EMBEDDED in the build file.
 * ========================================================================== */
#ifndef LUA_SCRIPT_H
#define LUA_SCRIPT_H

#include "railway_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define LUA_SCRIPT_MAX_PATH 260
#define LUA_SCRIPT_MAX_TEXT 512

    /* --------------------------------------------------------------------------
     * Lifecycle
     * -------------------------------------------------------------------------- */

    /* Create the scripting world and register the object classes.
     * Safe to call again - the second call is a no-op. */
    RwResult lua_script_init(void);

    /* Tear the interpreter down. Scripts do not run afterwards. */
    void lua_script_shutdown(void);

    bool lua_script_ready(void);

    /* --------------------------------------------------------------------------
     * Running scripts
     * -------------------------------------------------------------------------- */

    /* Run a chunk of Lua source from memory. `chunk_name` appears in error
     * messages. Returns RW_OK, or RW_ERR_INVALID_ARG when the script raised. */
    RwResult lua_script_run_string(const char *source, const char *chunk_name);

    /* Run a script file. */
    RwResult lua_script_run_file(const char *path);

    /* Per-tick service: runs Lua timers registered with Railway.after(),
     * Railway.every() and Railway.cancel(), plus any coroutines a script
     * suspended with Railway.wait(). Safe to call from the engine tick. */
    void lua_script_tick(unsigned delta_ms);

    /* --------------------------------------------------------------------------
     * Object lookup helpers (used by the GUI script console and the CLI)
     * -------------------------------------------------------------------------- */

    /* Fetch an object by class name and asset name, returning a new reference on
     * the Lua stack, or push nil when it does not exist.
     *   lua_script_push_object("Signal", "S3");
     * The caller supplies a valid lua_State (pass NULL to use the default one). */
    struct lua_State;
    int lua_script_push_object(struct lua_State *state, const char *class_name, const char *asset_name);

    /* The interpreter the engine owns, or NULL when scripting is unavailable. */
    struct lua_State *lua_script_state(void);

    /* --------------------------------------------------------------------------
     * Console integration
     * -------------------------------------------------------------------------- */

    /* Evaluate one console line. Lines beginning with '=' are treated as
     * expression print statements, mirroring the stock Lua REPL. The result text
     * (if any) is written into `output`. */
    RwResult lua_script_eval_console(const char *line, char *output, size_t output_size);

    /* Last error raised by a script, for the status bar. */
    const char *lua_script_last_error(void);

    /* --------------------------------------------------------------------------
     * Script registry: named scripts that can be started and stopped.
     * -------------------------------------------------------------------------- */
    typedef struct
    {
        char name[RW_MAX_NAME];
        char path[LUA_SCRIPT_MAX_PATH];
        bool enabled;
        bool running;
        unsigned run_count;
        unsigned error_count;
        char last_error[LUA_SCRIPT_MAX_TEXT];
    } LuaScriptInfo;

    RwResult lua_script_register(const char *name, const char *path);
    RwResult lua_script_start(const char *name);
    RwResult lua_script_stop(const char *name);
    int lua_script_count(void);
    RwResult lua_script_get_at(int index, LuaScriptInfo *info);
    RwResult lua_script_load_directory(const char *directory);

#ifdef __cplusplus
}
#endif

#endif /* LUA_SCRIPT_H */
