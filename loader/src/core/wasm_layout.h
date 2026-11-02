// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What Tome copies into a WebAssembly mod's memory (docs/wasm-mods.md): its
// C structs laid out as clang lays them out for wasm32 (pointers 4 bytes,
// doubles 8-aligned), the layout sdk/include/tome/tome_wasm.h declares and
// checks with static asserts. Pointers in them are addresses in the
// module's memory: a block is encoded for the address it will be copied to
// (`base`), so encode once with base 0 for the size, ask the module for a
// buffer, and encode again for its address. Game objects become handles
// (`Handles`), never pointers.

#include "wasm_abi.h"

#include <tome/tome.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tome::wasm {

// The handle for a game object of `kind` (0 for null).
using Handles = std::function<uint32_t(HandleKind kind, const void* object)>;

// Sizes and offsets of the wasm32 structs (tome_wasm.h checks the same).
constexpr uint32_t kUnitSize = 28;
constexpr uint32_t kEventSize = 184;
constexpr uint32_t kGameInfoSize = 12;
constexpr uint32_t kShadowSize = 20;

// tome_wasm_event: tome_event_info with wasm32 pointers; `item` and
// `destructable` and each unit's `object` are handles.
std::string encode_event(const tome_event_info& info, const Handles& handles, uint32_t base);

// An array of tome_wasm_unit (selection), its names after it.
std::string encode_units(const tome_unit* units, uint32_t count, const Handles& handles, uint32_t base);

// tome_wasm_game_info: size, build_name, file_version (no game_base: an
// address).
std::string encode_game_info(const char* build_name, const char* file_version, uint32_t base);

// An array of tome_wasm_script_shadow.
std::string encode_shadows(const tome_script_shadow* shadows, uint32_t count, uint32_t base);

// A JASS native's signature as the engine has it: "(Hunit;RS)V".
struct JassType {
    char kind = 0;          // I integer, R real, B boolean, S string, C code, H handle, V nothing (results)
    std::string handle;     // H: the handle type, e.g. "unit"
    bool operator==(const JassType&) const = default;
};
struct JassSignature {
    std::vector<JassType> params;
    JassType result;
};
// False for anything but a well-formed signature.
bool parse_signature(std::string_view text, JassSignature& out);

// A native's argument as Tome read it from the JASS stack: the word (an
// integer, boolean, handle id, code value, or a real's float bits), or a
// string's text (`null` for a null string).
struct NativeArg {
    uint32_t word = 0;
    std::string text;
    bool null = false;
};

// What tome_native(id, args, count) gets: one u32 per argument (strings as
// pointers to NUL-terminated copies after the words, 0 for null), the
// strings after them.
std::string encode_native_args(const JassSignature& sig, const std::vector<NativeArg>& args, uint32_t base);

// Whether a native may return `value`, by its result type: a handle only if
// it's 0 or one of the call's own arguments of the same handle type (the
// module can't make up handle ids, which JASS can't tell apart from
// another type's); `code` never (natives returning code aren't available to
// Wasm mods; registration refuses them).
bool valid_native_result(const JassSignature& sig, const std::vector<NativeArg>& args, uint32_t value);

} // namespace tome::wasm
