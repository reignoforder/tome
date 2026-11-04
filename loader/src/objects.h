// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Game objects for mods (tome.h, API version 16): telling what an object
// is and whether it's still there, JASS handle ids to objects and back,
// properties read and changed through the game's own functions (the ones
// the JASS natives call), and jass_call, which runs any native by name.
// Main thread, in a game. core/objects.h has the portable parts.
//
// Tome learns every native's address and signature from the engine's own
// BindNative calls (an AFTER hook), so jass_call needs no address of its
// own: whatever a map can call, a mod can, once a map has loaded.

#include "core/wasm_layout.h"
#include "mappings.h"

#include <tome/tome.h>

#include <cstdint>
#include <string>
#include <vector>

namespace tome::objects {

// Hooks BindNative and remembers the build's objects symbols. In DllMain,
// after patches::install (which sets up the hook engine). False (with
// `report`) if the hook can't go in; the API functions then fail.
bool install(uintptr_t game_base, const BuildMappings& build, std::string& report);

// What an object is (tome_object_type); TOME_OBJECT_NONE if it's null or gone.
int32_t type_of(const void* object);

// A unit's, item's or destructable's agent id (CAgent.id), read while it's
// known to be live; and whether the agent with that id still is (the
// agent table only: nothing of the object is read). For keeping objects
// across frames without touching freed memory (Wasm mods' handles).
struct AgentId {
    uint32_t a = 0, b = 0;
};
AgentId agent_id(const void* object);
bool agent_id_alive(AgentId id);

// jass_call in two steps, for callers that need the native's parameters
// before they have the arguments (Wasm mods: which words are strings). The
// checks api jass_call makes (main thread, a game, the native bound and
// taking `count` arguments: errors logged for `mod`), then the native's
// address and parsed signature; and running it with those arguments (the
// handle, code and string checks, as for DLL mods).
struct JassCall {
    void* fn = nullptr;
    wasm::JassSignature sig;
    std::vector<bool> integer_handle;  // by parameter: a Convert* type (not in the handle table)
};
tome_result prepare_jass_call(tome_mod* mod, const char* name, int32_t count, JassCall& out);
tome_result run_jass_call(tome_mod* mod, const char* name, const JassCall& call, const tome_jass_value* args,
                          tome_jass_value* result);

// Whether `object` is one of the current game's players (compared only:
// nothing of `object` is read).
bool is_player(const void* object);

// Fills the version 16 part of the API table.
void fill_api(tome_api& api);

} // namespace tome::objects
