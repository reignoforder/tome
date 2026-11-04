// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Game objects as mods see them (tome.h, API version 16): which property is
// on which kind of object and what it needs mapped, the "property:<name>"
// requirements of tome.mod.json, the handle table (an id's object, and
// whether a call made new ids), and the checks jass_call makes on its
// arguments. The calls into the game are in loader/src/objects.cpp.

#include "core/wasm_layout.h"

#include <tome/tome.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace tome::objects {

// The bit of a tome_object_type in PropertyInfo::on.
constexpr uint32_t bit(int32_t type) {
    return 1u << type;
}

// What a property's key is (get_keyed_property): none (0 only), a choice
// with 0 as the default, or something it can't do without (an ability id).
enum class Key : uint8_t { None, Optional, Required };

struct PropertyInfo {
    int32_t id;              // tome_property
    const char* name;        // for "requires": "property:<name>"
    uint32_t on;             // bit(TOME_OBJECT_*) of the kinds that have it
    uint32_t set_on;         // the kinds it can be set on (0: read only)
    Key key = Key::None;
    uint32_t max_key = 0;      // Key::Optional: the keys up to this one can be read
    uint32_t max_set_key = 0;  // and up to this one set
    bool heroes_only = false;  // units that are heroes
    const char* symbols[6] = {};  // the mapped names it needs (nullptr-terminated)
};

// Whether a property can be read from (`set` false) or set on an object of
// tome_object_type `type` with `key`; TOME_ERR_INVALID_ARGUMENT if not.
// Hero checks are the caller's.
tome_result check_access(const PropertyInfo& p, int32_t type, uint32_t key, bool set);

// The property with this id or name, or null for one this Tome doesn't know.
const PropertyInfo* property(int32_t id);
const PropertyInfo* property_by_name(std::string_view name);

// Checks one tome.mod.json "requires" entry that is a capability
// ("prefix:name", not "Class::Method"). Only "property:" is checked for now:
// false (with `error`) for a property this Tome doesn't know or whose
// symbols `mapped` says this build lacks. Other capabilities pass.
bool capability_met(std::string_view capability, const std::function<bool(const char*)>& mapped,
                    std::string& error);

// The first mapped name property `p` needs that `mapped` says this build
// lacks, or null if it has them all: its own symbols, and for properties
// of units, items and destructables the agent checks every use makes
// (ObjectLookup, AgentRecord.tag, AgentRecord.dying, AgentTag.agent).
// What both capability_met and objects::install decide by.
const char* missing_symbol(const PropertyInfo& p, const std::function<bool(const char*)>& mapped);

// A double for set_property as the integer a game field takes: rounded
// toward zero and clamped to int32_t. False for NaN and infinities.
bool to_int(double value, int32_t& out);
// The same check for a float field; the value is clamped to float's range.
bool to_float(double value, float& out);

// Where the handle manager keeps its table (HandleManager.*, HandleEntry.*,
// HandleId.first). `count` and `object` must be mapped (non-zero) to read it.
struct HandleTable {
    uint32_t entries = 0;     // offset of the entry array pointer
    uint32_t count = 0;       // offset of the number of ids handed out
    uint32_t entry_size = 0;  // bytes per entry
    uint32_t object = 0;      // the object's offset in an entry (the reference count is at +0)
    uint32_t first = 0;       // the first handle id
};

// The object a handle id stands for, or null if it isn't one in use: below
// the first id, past the ids handed out, or with no references (freed).
// The game's own lookup (0x285FE0) checks only the lower bound.
void* handle_object(const void* manager, uint32_t handle, const HandleTable& table);

// Whether a JASS handle type's values are indices the game keeps apart
// from the handle table and checks itself: texttag (0 to 99, every texttag
// native compares the id with the texttag count before using it, on 1.27b
// and 1.26a). jass_call passes them as they are, like Convert* types. Not
// image or ubersplat: their natives index an array with the id unchecked
// (reverse-engineering.md, "Text tags").
bool handle_is_index(std::string_view jass_type);

// Whether a live handle's object may be passed as a JASS `jass_type`
// argument, by what Tome knows it is (tome_object_type: OTHER for any
// object that isn't a unit, item, destructable or player). unit, item,
// destructable and player take only their own kind, widget any of the
// first three, handle and agent anything; other types (timer, group...)
// take only OTHER objects, and the game's own converter checks which kind
// (an is-a test on the object's type tag, verified for the converters of
// 30 types: reverse-engineering.md, "Game objects").
bool handle_type_fits(std::string_view jass_type, int32_t object_type);

// What jass_call checks before running a native with `count` arguments:
// the native's signature must parse and take that many. Returns "" if the
// call can go ahead, or why it can't.
std::string check_call(std::string_view native, std::string_view signature, int32_t count,
                       wasm::JassSignature& parsed);

} // namespace tome::objects
