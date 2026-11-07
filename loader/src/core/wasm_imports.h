// SPDX-License-Identifier: GPL-3.0-or-later
// What a WebAssembly mod gets (docs/wasm-mods.md): its limits, and the
// functions Tome gives it (module "tome") with WAMR's signature strings: i i32, f f32, F f64,
// "(params)result". Pointers and lengths are raw i32s, checked by
// core/wasm_abi, not WAMR's '*' conversions. loader/src/wasm_mods.cpp
// implements them (w_<name>); fuzz/fuzz_wamr.cpp registers the same list.
#pragma once

#include <cstddef>
#include <cstdint>

namespace tome::wasm {

// Instructions one call into a module may run (every opcode counts):
// about half a second of the interpreter, so a module stuck in a loop
// can't hang the game.
constexpr int kInstructionBudget = 200'000'000;
// The budget doesn't cover everything: the interpreter counts each entry
// into the module on its own (a callback the module's import call causes
// starts afresh), and time spent in imports isn't counted. So past this
// much time since the outermost call began, the next import call or nested
// call traps (checked there, so a call stuck in pure Wasm still stops at
// the budget). Far beyond any sane call: only a module looping over imports
// gets there.
constexpr uint32_t kCallTimeLimitMs = 5000;
// 64 KiB pages a module's memory may grow to (64 MiB).
constexpr uint32_t kMaxMemoryPages = 1024;
// The interpreter's operand stack, per module.
constexpr uint32_t kStackSize = 64 * 1024;
// Largest .wasm Tome loads.
constexpr size_t kMaxModuleSize = 16u << 20;

} // namespace tome::wasm

#define TOME_WASM_IMPORTS(X) \
    X(log, "(iii)") \
    X(mod_id, "(ii)i") \
    X(mod_version, "(ii)i") \
    X(game_info, "(ii)i") \
    X(register_command, "(iii)i") \
    X(display_text, "(iif)i") \
    X(setting_bool, "(iii)i") \
    X(setting_int, "(iii)i") \
    X(setting_number, "(iiF)F") \
    X(setting_string, "(iiii)i") \
    X(set_setting_bool, "(iii)i") \
    X(set_setting_int, "(iii)i") \
    X(set_setting_number, "(iiF)i") \
    X(set_setting_string, "(iiii)i") \
    X(on_setting_changed, "(i)i") \
    X(on_event, "(ii)i") \
    X(on_ui, "(ii)i") \
    X(ui_root, "(i)i") \
    X(frame_get, "(iii)i") \
    X(frame_create, "(iiiii)i") \
    X(frame_set_point, "(iiiiff)i") \
    X(frame_set_text, "(iii)i") \
    X(frame_set_visible, "(ii)i") \
    X(frame_set_animation, "(iii)i") \
    X(frame_on_click, "(ii)i") \
    X(load_fdf, "(ii)i") \
    X(load_toc, "(ii)i") \
    X(on_keybind, "(iii)i") \
    X(keybind_name, "(iiii)i") \
    X(popup, "(iii)i") \
    X(popup_close, "(i)i") \
    X(restart_game, "()i") \
    X(selection, "(iiii)i") \
    X(subscribe, "(iii)i") \
    X(publish, "(iiii)i") \
    X(map_script_shadows, "(iii)i") \
    X(read_game_file, "(iiii)i") \
    X(config_read, "(iiii)i") \
    X(config_write, "(iiii)i") \
    X(register_native, "(iii)i") \
    X(jass_code_call, "(ii)i") \
    X(jass_code_execute, "(i)i") \
    X(object_type, "(i)i") \
    X(object_of, "(ii)i") \
    X(handle_of, "(i)i") \
    X(player_object, "(i)i") \
    X(unit_info, "(iii)i") \
    X(get_property, "(iii)i") \
    X(set_property, "(iiF)i") \
    X(get_keyed_property, "(iiii)i") \
    X(set_keyed_property, "(iiiF)i") \
    X(list_objects, "(iii)i") \
    X(jass_call, "(iiiiiii)i") \
    X(host_player, "()i") \
    X(net_send, "(ii)i") \
    X(on_net_message, "(i)i")
