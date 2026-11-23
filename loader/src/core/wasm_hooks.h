// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Typed hooks for WebAssembly mods (docs/wasm-mods.md, "Hooks"): which mapped
// functions a module may hook, and the block of words its handler is called
// with. A DLL mod's hook sees raw argument words and game pointers; a
// module never gets a pointer, so only functions whose every argument is a
// number or a game object it can hold as a handle qualify, judged from the
// mapping's signature alone:
//
//   arguments: int, int32_t, uint32_t, unsigned (numbers), float,
//              CUnit*, CItem*, CPlayerWar3* (handles; `this` too)
//   result:    void, an integer type, or float (no pointers)
//
// Strings, other pointers, 64-bit values and variadic functions don't
// qualify. Neither do functions whose hook could cost the game its safety nets
// or whose numbers are game handles: the JASS natives ("Native.*": trigger,
// code and boolexpr handles), the CRT and crash handler ("__*",
// "CrashHandler::*") and the game's own start and end (GameShutdown,
// InitNatives). Of the rest only float arguments and the int arguments of
// a short list of functions (writable_ints() in the .cpp) are changeable: any
// other int may be an index or a table key, which a module could set out of
// range (CPlayerWar3::SetState's state, CGlueMgr::SetScreen's screen).
// tools/gen-game-header.py applies the same rule to write
// sdk/include/tome/tome_wasm_game.h.
//
// The handler's block, 32-bit little-endian words:
//   [flags][result][argument 0][argument 1]...
// `flags` is in/out: a BEFORE handler sets kHookSkip to return `result`
// without the game's function (and the REPLACE handlers) running. `result`
// is the game's result in an AFTER handler (what the handler leaves there is
// returned) and the value to return with kHookSkip in a BEFORE one. Number
// arguments a BEFORE handler changes are what the game's function receives;
// object arguments are read-only (the module never hands the game a pointer).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tome::wasm {

// Int is a number the module may change, Index one it may only read.
enum class HookArg : uint8_t { Int, Index, Float, Unit, Item, Player };
enum class HookResult : uint8_t { None, Int, Float };

struct HookPoint {
    std::vector<HookArg> args;
    HookResult result = HookResult::None;
};

constexpr uint32_t kHookSkip = 1;      // flags bit: return `result`, don't run the game's function
constexpr uint32_t kHookHeader = 8;    // bytes before the arguments: flags and result
constexpr size_t kMaxHookArgs = 16;

// Whether the mapped function `name` with this C-style signature can be a Wasm
// hook; if not, `why` says what keeps it from being one.
bool hook_point(std::string_view name, std::string_view signature, HookPoint& out, std::string& why);

inline uint32_t hook_block_size(const HookPoint& point) {
    return kHookHeader + 4u * static_cast<uint32_t>(point.args.size());
}

} // namespace tome::wasm
