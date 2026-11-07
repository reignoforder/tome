// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * ExecuteFunc guard (docs/known-bugs.md, "JASS natives that crash or
 * misbehave").
 *
 * ExecuteFunc(name) runs a script function by name in a new thread with no
 * arguments. On 1.27b, naming a function that takes parameters (the map's own
 * or a Blizzard.j one such as ClearTextMessagesBJ) crashes the game in the
 * JASS VM (access violation reading 0x21 at game.dll+0x91B5EA, checked in
 * game, Nov 2026): the function reads arguments that were never pushed. A
 * missing name and a native's name are already ignored by the game.
 *
 * Fix: the same BindNative mechanism as the path sanitizer -- when the engine
 * binds ExecuteFunc, bind a wrapper instead. At each map's loading screen,
 * from the scripts the map script guard reads there (events.h
 * set_map_script_hook: war3map.j, Blizzard.j, common.j, the map's own copies
 * first), it collects the functions that take parameters
 * (jass::functions_taking_parameters); a call naming one is refused (nothing
 * runs, as for a missing name). If the scripts can't be read, every call goes
 * through, as without the patch. Without the events (not mapped), it reads
 * the scripts itself on the first call in each map.
 */
#include "patches.h"

#include "core/jass.h"
#include "core/log_guard.h"
#include "events.h"
#include "hooks.h"
#include "log.h"
#include "natives.h"
#include "storm.h"
#include "tome/tome.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#pragma GCC diagnostic ignored "-Wattributes"

namespace tome::patches {
namespace {

using StringTextFn = const char*(__thiscall*)(const void* string);
using ExecuteFuncFn = uint32_t(__cdecl*)(uint32_t name);

StringTextFn g_string_text = nullptr;
// The engine's ExecuteFunc, captured at bind time. Main thread only, like
// the path sanitizer's (bound during InitNatives, called by the map script).
void* volatile g_orig = nullptr;

// The functions of the current map's scripts that take parameters; g_known
// false if the scripts couldn't be read. From the last loading screen once
// g_from_events; before that, read on the first call in each map load
// (g_loaded_for, natives::map_load_count). Main thread.
std::unordered_set<std::string> g_taking;
bool g_from_events = false;
unsigned g_loaded_for = 0;
bool g_known = false;
unsigned long g_refused = 0;

// The game reads war3map.j from the map, or scripts\war3map.j (protected maps).
constexpr size_t kMaxScript = 64u << 20;

void collect(const std::string& script, const std::vector<std::string_view>& prelude) {
    g_taking.clear();
    jass::functions_taking_parameters(script, g_taking);
    for (std::string_view text : prelude) jass::functions_taking_parameters(text, g_taking);
    g_known = true;
}

// events.h MapScriptHook: at the loading screen.
void on_map_script(const std::string* script, const std::vector<std::string_view>& prelude) {
    g_from_events = true;
    if (script) {
        collect(*script, prelude);
        return;
    }
    g_taking.clear();
    g_known = false;
    TOME_INFO("execute-func-guard: couldn't read the map's script; ExecuteFunc goes unchecked in this map");
}

void load_functions() {
    g_taking.clear();
    g_known = false;
    if (!storm::init()) return;
    std::string script;
    if ((!storm::read_file("war3map.j", script, kMaxScript) || script.empty()) &&
        (!storm::read_file("scripts\\war3map.j", script, kMaxScript) || script.empty())) {
        TOME_WARN("execute-func-guard: couldn't read the map's script; ExecuteFunc goes unchecked in this map");
        return;
    }
    std::string common, blizzard;
    std::vector<std::string_view> prelude;
    if (storm::read_file("Scripts\\common.j", common, kMaxScript)) prelude.push_back(common);
    if (storm::read_file("Scripts\\Blizzard.j", blizzard, kMaxScript)) prelude.push_back(blizzard);
    collect(script, prelude);
}

uint32_t __cdecl wrapped_execute_func(uint32_t name_arg) {
    auto orig = reinterpret_cast<ExecuteFuncFn>(g_orig);
    if (!orig) return 0;
    const char* name = name_arg ? g_string_text(reinterpret_cast<const void*>(name_arg)) : nullptr;
    if (name && *name) {
        const unsigned load = natives::map_load_count();
        if (!g_from_events && g_loaded_for != load) {
            g_loaded_for = load;
            load_functions();
        }
        if (g_known && g_taking.count(name)) {
            ++g_refused;
            // A map can call it in a loop: the first few, then every 1000th.
            if (g_refused <= 20 || g_refused % 1000 == 0) {
                TOME_WARN("execute-func-guard: refused ExecuteFunc(\"%s\") -- the function takes parameters, "
                          "which would crash the game (total %lu)", log::printable(name, 80).c_str(), g_refused);
            }
            return 0;
        }
    }
    return orig(name_arg);
}

// BEFORE BindNative(fn, name, signature): bind the wrapper for ExecuteFunc.
// As in the path sanitizer, the hook isn't reserved (mods aren't the threat).
void TOME_CALL on_bind_native(tome_mod*, tome_hook_call* call, void*) {
    if (call->arg_count < 3) return;
    const char* name = reinterpret_cast<const char*>(call->args[1]);
    if (!name || std::strcmp(name, "ExecuteFunc") != 0) return;
    void* fn = reinterpret_cast<void*>(call->args[0]);
    if (fn != reinterpret_cast<void*>(&wrapped_execute_func)) g_orig = fn;
    call->args[0] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&wrapped_execute_func));
}

bool install(uintptr_t game_base, const BuildMappings& build, const Variant&, std::string& report) {
    g_string_text = reinterpret_cast<StringTextFn>(game_base + build.jass.string_get_text);
    std::string error;
    hooks::Target* bind = hooks::find_or_install("BindNative", error);
    if (!bind) {
        report = "BindNative: " + error;
        return false;
    }
    hooks::Handler before;
    before.phase = TOME_HOOK_BEFORE;
    before.priority = INT32_MIN;
    before.fn = on_bind_native;
    hooks::add(bind, before);
    events::set_map_script_hook(on_map_script);
    report = "refuses ExecuteFunc on functions that take parameters";
    return true;
}

const char* const kSymbols[] = {"BindNative", "RCString::GetText", nullptr};

} // namespace

extern const Patch kExecuteFuncGuard = {
    "execute-func-guard",
    "refuses ExecuteFunc on a function that takes parameters (crashes the game)",
    {},
    kSymbols,
    nullptr,  // reserves nothing (see the path sanitizer)
    nullptr,
    install,
};

} // namespace tome::patches
