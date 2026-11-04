// SPDX-License-Identifier: GPL-3.0-or-later
#include "objects.h"

#include "core/objects.h"
#include "frames.h"
#include "hooks.h"
#include "log.h"
#include "mod_handle.h"
#include "mod_manager.h"
#include "natives.h"
#include "units.h"
#include "unwind.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <set>
#include <unordered_map>
#include <vector>

// GCC warns that __thiscall is meant for class methods, but it does apply it
// to function pointer types (see mod_menu.cpp).
#pragma GCC diagnostic ignored "-Wattributes"

namespace tome::objects {
namespace {

using ObjectLookupFn = void*(__fastcall*)(uint32_t id_a, uint32_t id_b);
using GetStateFn = float*(__thiscall*)(void* unit, float* out, int state);
using SetStateFn = void(__thiscall*)(void* unit, int state, const float* value);
using GetFloatFn = float*(__thiscall*)(void* object, float* out);
using SetFloatFn = void(__thiscall*)(void* object, const float* value);
using GetIntFn = int(__thiscall*)(void* unit);
using GetPositionFn = void*(__thiscall*)(void* unit);
using PositionGetFn = float*(__thiscall*)(void* position, float* out);
using PositionSetFn = void*(__thiscall*)(void* position, const float* xyz);
using PlayerSetStateFn = void(__thiscall*)(void* player, int state, int value);
using VmGetFn = void*(__fastcall*)(int index);
using FloatMultiplyFn = float*(__fastcall*)(float* out, const float* a, const float* b);
using SetFacingFn = void(__thiscall*)(void* position, const float* radians, int flag);
using FindAbilityFn = void*(__thiscall*)(void* unit, uint32_t id, int a, int b, int c, int d);
using AbilityStepFn = void(__thiscall*)(void* ability);
using HeroStatFn = int(__thiscall*)(void* unit, int include_bonuses);
using SetHeroStatFn = void(__thiscall*)(void* unit, int value, int permanent);
using AgentCallback = int(__fastcall*)(void* agent, void* context);
using ForEachAgentFn = void(__fastcall*)(uint32_t type_tag, AgentCallback callback, void* context, int subclasses);

// GetUnitFacing's and SetUnitFacing's factors (180/pi and pi/180; 1.27b
// has them in .data as these floats, 1.26a computes them at startup).
const float kToDegrees = 57.2957795f;
const float kToRadians = 0.0174532925f;

// Player slots to look through for player objects: 16 on these builds (12
// players and 4 neutral ones); units::player_at returns null past the
// game's player count.
constexpr int32_t kMaxPlayerSlots = 32;

uintptr_t g_base = 0;
const BuildMappings* g_build = nullptr;
HandleTable g_table;
ObjectLookupFn g_object_lookup = nullptr;
GetStateFn g_unit_get_state = nullptr;
SetStateFn g_unit_set_state = nullptr;
GetFloatFn g_unit_get_move_speed = nullptr;
SetFloatFn g_unit_set_move_speed = nullptr;
GetIntFn g_unit_get_level = nullptr;
PositionGetFn g_position_get = nullptr;
PositionSetFn g_position_set = nullptr;
PlayerSetStateFn g_player_set_state = nullptr;
VmGetFn g_vm_get = nullptr;
FloatMultiplyFn g_float_multiply = nullptr;
SetFacingFn g_position_set_facing = nullptr;
FindAbilityFn g_unit_find_ability = nullptr;
GetIntFn g_ability_max_level = nullptr;
GetFloatFn g_unit_get_acquire_range = nullptr;
SetFloatFn g_unit_set_acquire_range = nullptr;
GetIntFn g_unit_get_hero_xp = nullptr;
HeroStatFn g_unit_get_hero_stat[3] = {};
ForEachAgentFn g_for_each_agent = nullptr;
SetHeroStatFn g_unit_set_hero_stat[3] = {};
bool g_available[64] = {};  // by tome_property: every symbol it needs is mapped
bool g_jass_call_ok = false;  // every symbol jass_call reads is mapped (install)
bool g_installed = false;

// Every native the engine has bound (its own, Tome's and mods'), from
// BindNative: the address the map script calls and the signature.
struct Native {
    void* fn = nullptr;
    std::string signature;
};
SRWLOCK g_natives_lock = SRWLOCK_INIT;
std::unordered_map<std::string, Native> g_natives;
// Handle types that are really integers: what the Convert* natives make
// from one (ConvertUnitState (I)Hunitstate;, ConvertRace...). Their values
// aren't in the handle table, so jass_call doesn't look them up there.
std::set<std::string> g_integer_types;

// Warnings given once per mod and subject (main thread).
std::set<std::string> g_warned;

template <typename T>
T field(const void* object, uint32_t offset) {
    T value;
    std::memcpy(&value, static_cast<const char*>(object) + offset, sizeof(value));
    return value;
}

template <typename Fn>
Fn function(uint32_t rva) {
    return rva ? reinterpret_cast<Fn>(g_base + rva) : nullptr;
}

template <typename Fn>
Fn vtable_slot(const void* object, uint32_t slot) {
    return field<Fn>(field<const void*>(object, 0), slot);
}

bool warn_once(const tome_mod* mod, const std::string& subject) {
    return g_warned.insert((mod ? mod->id : std::string("?")) + "\n" + subject).second;
}

// BindNative(void* fn, const char* name, const char* signature), AFTER: the
// arguments as the engine bound them (built-in patches may have swapped `fn`
// for a wrapper, which is what the map script calls too).
void TOME_CALL after_bind_native(tome_mod*, tome_hook_call* call, void*) {
    if (call->arg_count < 3) return;
    auto fn = reinterpret_cast<void*>(static_cast<uintptr_t>(call->args[0]));
    auto name = reinterpret_cast<const char*>(static_cast<uintptr_t>(call->args[1]));
    auto signature = reinterpret_cast<const char*>(static_cast<uintptr_t>(call->args[2]));
    if (!fn || !name || !signature) return;
    ExclusiveLock hold(g_natives_lock);
    Native& native = g_natives[name];
    native.fn = fn;
    native.signature = signature;
    std::string_view sig = signature;
    if (std::string_view(name).substr(0, 7) == "Convert" && sig.substr(0, 4) == "(I)H" && sig.back() == ';') {
        g_integer_types.insert(std::string(sig.substr(4, sig.size() - 5)));
    }
}

// A unit, item or destructable whose agent record says it's alive (what the
// unit natives check: ObjectLookup on CAgent.id, the record's tag, and not
// dying).
bool record_alive(uint32_t a, uint32_t b) {
    if (!g_object_lookup) return false;
    const void* record = g_object_lookup(a, b);
    const auto& o = g_build->objects;
    return record && field<uint32_t>(record, o.agent_tag_offset) == o.agent_tag &&
           field<uint32_t>(record, o.agent_dying_offset) == 0;
}

bool agent_alive(const void* object) {
    const uint32_t id = g_build->play.agent_id_offset;
    return record_alive(field<uint32_t>(object, id), field<uint32_t>(object, id + 4));
}

int32_t kind_of(const void* object) {
    const auto vtable = field<uintptr_t>(object, 0);
    const auto& p = g_build->play;
    if (vtable == g_base + p.unit_vtable) return TOME_OBJECT_UNIT;
    if (vtable == g_base + p.item_vtable) return TOME_OBJECT_ITEM;
    if (vtable == g_base + p.destructable_vtable) return TOME_OBJECT_DESTRUCTABLE;
    return TOME_OBJECT_NONE;
}

// The checks every API function starts with: Tome installed, a game, the main thread.
tome_result ready(const char* what) {
    if (!g_installed) return TOME_ERR_UNAVAILABLE;
    if (!frames::on_main_thread()) {
        TOME_ERROR("%s called off the game's main thread (thread %lu); refused", what, GetCurrentThreadId());
        return TOME_ERR_WRONG_THREAD;
    }
    return units::game_object() ? TOME_OK : TOME_ERR_UNAVAILABLE;
}

// --- properties ---

void* unit_position(void* unit) {
    return vtable_slot<GetPositionFn>(unit, g_build->objects.unit_position_slot)(unit);
}

// The playerstate a player property is (the JASS ids; tome_resource).
int32_t player_state(int32_t property) {
    switch (property) {
    case TOME_PROPERTY_GOLD:      return TOME_RESOURCE_GOLD;
    case TOME_PROPERTY_LUMBER:    return TOME_RESOURCE_LUMBER;
    case TOME_PROPERTY_FOOD_CAP:  return TOME_RESOURCE_FOOD_CAP;
    case TOME_PROPERTY_FOOD_USED: return TOME_RESOURCE_FOOD_USED;
    }
    return 0;
}

tome_result read_property(void* object, int32_t type, int32_t property, uint32_t key, double& value) {
    const auto& p = g_build->play;
    const auto& o = g_build->objects;
    float f = 0;
    switch (property) {
    case TOME_PROPERTY_LIFE:
        if (type == TOME_OBJECT_UNIT) {
            value = *g_unit_get_state(object, &f, 0);
        } else {
            value = *vtable_slot<GetFloatFn>(object, o.widget_get_life_slot)(object, &f);
        }
        return TOME_OK;
    case TOME_PROPERTY_MAX_LIFE: value = *g_unit_get_state(object, &f, 1); return TOME_OK;
    case TOME_PROPERTY_MANA:     value = *g_unit_get_state(object, &f, 2); return TOME_OK;
    case TOME_PROPERTY_MAX_MANA: value = *g_unit_get_state(object, &f, 3); return TOME_OK;
    case TOME_PROPERTY_X:
    case TOME_PROPERTY_Y: {
        // Any widget's position (GetUnitX, GetItemX and GetDestructableX all use the slot).
        float xyz[3] = {};
        g_position_get(unit_position(object), xyz);
        value = xyz[property == TOME_PROPERTY_X ? 0 : 1];
        return TOME_OK;
    }
    case TOME_PROPERTY_MOVE_SPEED: value = *g_unit_get_move_speed(object, &f); return TOME_OK;
    case TOME_PROPERTY_LEVEL: {
        const int32_t hero = units::hero_level(object);  // 0 for units that aren't heroes
        value = hero ? hero : g_unit_get_level(object);
        return TOME_OK;
    }
    case TOME_PROPERTY_TYPE_ID:
        value = type == TOME_OBJECT_UNIT   ? field<uint32_t>(object, p.unit_type_offset)
                : type == TOME_OBJECT_ITEM ? units::item_type(object)
                                           : units::destructable_type(object);
        return TOME_OK;
    case TOME_PROPERTY_OWNER:   value = field<int32_t>(object, p.unit_owner_offset); return TOME_OK;
    case TOME_PROPERTY_CHARGES: value = field<int32_t>(object, o.item_charges_offset); return TOME_OK;
    case TOME_PROPERTY_SLOT:    value = units::player_slot(object); return TOME_OK;
    case TOME_PROPERTY_GOLD:
    case TOME_PROPERTY_LUMBER:
    case TOME_PROPERTY_FOOD_CAP:
    case TOME_PROPERTY_FOOD_USED: {
        int32_t amount = 0;
        if (!units::player_resource(units::player_slot(object), player_state(property), amount)) {
            return TOME_ERR_FAILED;
        }
        value = amount;
        return TOME_OK;
    }
    case TOME_PROPERTY_FACING: {
        // As GetUnitFacing: the position's radians times 180/pi, with the game's own multiply.
        void* position = unit_position(object);
        float radians = 0, degrees = 0;
        const float* r = vtable_slot<GetFloatFn>(position, o.position_facing_slot)(position, &radians);
        g_float_multiply(&degrees, r, &kToDegrees);
        value = degrees;
        return TOME_OK;
    }
    case TOME_PROPERTY_FLY_HEIGHT:    value = field<float>(object, o.unit_fly_height_offset); return TOME_OK;
    case TOME_PROPERTY_ACQUIRE_RANGE: value = *g_unit_get_acquire_range(object, &f); return TOME_OK;
    case TOME_PROPERTY_HERO_XP:       value = g_unit_get_hero_xp(object); return TOME_OK;
    case TOME_PROPERTY_STRENGTH:
    case TOME_PROPERTY_AGILITY:
    case TOME_PROPERTY_INTELLIGENCE:
        value = g_unit_get_hero_stat[property - TOME_PROPERTY_STRENGTH](object, key == 1);
        return TOME_OK;
    case TOME_PROPERTY_ARMOR:      value = field<float>(object, o.unit_armor_offset); return TOME_OK;
    case TOME_PROPERTY_ARMOR_TYPE: value = field<int32_t>(object, o.unit_armor_type_offset); return TOME_OK;
    case TOME_PROPERTY_DAMAGE_BASE:
    case TOME_PROPERTY_DAMAGE_DICE:
    case TOME_PROPERTY_DAMAGE_SIDES:
    case TOME_PROPERTY_ATTACK_COOLDOWN:
    case TOME_PROPERTY_ATTACK_RANGE: {
        // The unit's attack ability (CAbilityAttack), two weapons' values side by side.
        const void* attack = field<const void*>(object, o.unit_attack_offset);
        if (!attack) return TOME_ERR_INVALID_ARGUMENT;  // a unit without an attack
        switch (property) {
        case TOME_PROPERTY_DAMAGE_BASE:  value = field<int32_t>(attack, o.attack_damage_base_offset + 4 * key); break;
        case TOME_PROPERTY_DAMAGE_DICE:  value = field<int32_t>(attack, o.attack_damage_dice_offset + 4 * key); break;
        case TOME_PROPERTY_DAMAGE_SIDES: value = field<int32_t>(attack, o.attack_damage_sides_offset + 4 * key); break;
        case TOME_PROPERTY_ATTACK_COOLDOWN: value = field<float>(attack, o.attack_cooldown_offset + 8 * key); break;
        default:                         value = field<float>(attack, o.attack_range_offset + 8 * key); break;
        }
        return TOME_OK;
    }
    case TOME_PROPERTY_ABILITY_LEVEL: {
        // As GetUnitAbilityLevel: 0 without the ability.
        const void* ability = g_unit_find_ability(object, key, 0, 1, 1, 1);
        value = ability ? field<int32_t>(ability, o.ability_level_offset) + 1 : 0;
        return TOME_OK;
    }
    }
    return TOME_ERR_INVALID_ARGUMENT;
}

// Properties set as integers (players' resources, heroes' attributes,
// ability levels); the rest are floats.
bool integer_property(int32_t property) {
    switch (property) {
    case TOME_PROPERTY_GOLD:
    case TOME_PROPERTY_LUMBER:
    case TOME_PROPERTY_FOOD_CAP:
    case TOME_PROPERTY_FOOD_USED:
    case TOME_PROPERTY_STRENGTH:
    case TOME_PROPERTY_AGILITY:
    case TOME_PROPERTY_INTELLIGENCE:
    case TOME_PROPERTY_ABILITY_LEVEL:
        return true;
    }
    return false;
}

tome_result write_property(void* object, int32_t type, int32_t property, uint32_t key, double value) {
    const auto& o = g_build->objects;
    float f = 0;
    int32_t n = 0;
    if (integer_property(property) ? !to_int(value, n) : !to_float(value, f)) return TOME_ERR_INVALID_ARGUMENT;
    switch (property) {
    case TOME_PROPERTY_LIFE:
        if (type == TOME_OBJECT_UNIT) {
            g_unit_set_state(object, 0, &f);
        } else {
            vtable_slot<SetFloatFn>(object, o.widget_set_life_slot)(object, &f);
        }
        return TOME_OK;
    case TOME_PROPERTY_MANA: g_unit_set_state(object, 2, &f); return TOME_OK;
    case TOME_PROPERTY_X:
    case TOME_PROPERTY_Y: {
        // As SetUnitX/SetUnitY do it: the other coordinate kept, z = 0.
        float xyz[3] = {};
        g_position_get(unit_position(object), xyz);
        const float to[3] = {property == TOME_PROPERTY_X ? f : xyz[0], property == TOME_PROPERTY_Y ? f : xyz[1], 0};
        g_position_set(unit_position(object), to);
        return TOME_OK;
    }
    case TOME_PROPERTY_MOVE_SPEED: g_unit_set_move_speed(object, &f); return TOME_OK;
    case TOME_PROPERTY_GOLD:
    case TOME_PROPERTY_LUMBER:
    case TOME_PROPERTY_FOOD_CAP:
    case TOME_PROPERTY_FOOD_USED:
        // SetPlayerState passes these as they are (it scales only the
        // gathered amounts, 15 and 16).
        g_player_set_state(object, player_state(property), n);
        return TOME_OK;
    case TOME_PROPERTY_FACING: {
        // As SetUnitFacing: degrees times pi/180; flag 1 turns it at once
        // (SetUnitFacing's 0 turns it at the unit's turn speed).
        float radians = 0;
        g_float_multiply(&radians, &f, &kToRadians);
        g_position_set_facing(unit_position(object), &radians, 1);
        return TOME_OK;
    }
    case TOME_PROPERTY_ACQUIRE_RANGE: g_unit_set_acquire_range(object, &f); return TOME_OK;
    case TOME_PROPERTY_STRENGTH:
    case TOME_PROPERTY_AGILITY:
    case TOME_PROPERTY_INTELLIGENCE:
        // SetHeroStr(u, value, true): the base value, for good.
        g_unit_set_hero_stat[property - TOME_PROPERTY_STRENGTH](object, n, 1);
        return TOME_OK;
    case TOME_PROPERTY_ABILITY_LEVEL: {
        // As SetUnitAbilityLevel: clamped to 1..its levels, a level at a time.
        void* ability = g_unit_find_ability(object, key, 0, 1, 1, 1);
        if (!ability) return TOME_ERR_INVALID_ARGUMENT;  // the unit doesn't have it
        // The level is read once, as the native does: it counts the steps
        // itself instead of reading the ability again.
        const int32_t last = g_ability_max_level(ability);
        const int32_t to = std::max(1, std::min(n, last));
        int32_t level = field<int32_t>(ability, o.ability_level_offset) + 1;
        for (; level < to; ++level) vtable_slot<AbilityStepFn>(ability, o.ability_level_up_slot)(ability);
        for (; level > to; --level) vtable_slot<AbilityStepFn>(ability, o.ability_level_down_slot)(ability);
        return TOME_OK;
    }
    }
    return TOME_ERR_INVALID_ARGUMENT;
}

// Checks shared by the property functions; `type` gets the object's.
tome_result check_property(const char* what, const void* object, int32_t property, uint32_t key, bool set,
                           int32_t& type) {
    if (tome_result r = ready(what); r != TOME_OK) return r;
    const PropertyInfo* info = objects::property(property);
    if (!info) return TOME_ERR_INVALID_ARGUMENT;
    if (!g_available[property]) return TOME_ERR_UNAVAILABLE;
    type = type_of(object);
    // Readable on the object at all first: a property that only can't be set says so.
    if (check_access(*info, type, key, false) != TOME_OK) return TOME_ERR_INVALID_ARGUMENT;
    if (info->heroes_only && !(units::unit_flags(object) & TOME_UNIT_HERO)) return TOME_ERR_INVALID_ARGUMENT;
    return set ? check_access(*info, type, key, true) : TOME_OK;
}

tome_result get_keyed(const void* object, int32_t property, uint32_t key, double* value) {
    if (!value) return TOME_ERR_INVALID_ARGUMENT;
    *value = 0;
    int32_t type = 0;
    if (tome_result r = check_property("get_property", object, property, key, false, type); r != TOME_OK) return r;
    return read_property(const_cast<void*>(object), type, property, key, *value);
}

tome_result set_keyed(tome_mod* mod, void* object, int32_t property, uint32_t key, double value) {
    if (!mod) return TOME_ERR_INVALID_ARGUMENT;
    int32_t type = 0;
    tome_result r = check_property("set_property", object, property, key, true, type);
    if (r != TOME_OK) {
        // Readable but not settable on this kind of object (not refused for
        // another reason, such as a unit that isn't a hero).
        const PropertyInfo* info = objects::property(property);
        if (r == TOME_ERR_INVALID_ARGUMENT && info && type != TOME_OBJECT_NONE &&
            check_access(*info, type, key, false) == TOME_OK && check_access(*info, type, key, true) != TOME_OK &&
            warn_once(mod, std::string("read-only ") + info->name)) {
            TOME_WARN("[%s] set_property(%s): it can only be read here", mod->id.c_str(), info->name);
        }
        return r;
    }
    if (mod_manager().game_locked() && !mod->sync_required) {
        if (warn_once(mod, "set_property locked")) {
            TOME_WARN("[%s] set_property(%s) refused: in a multiplayer game or replay only a \"sync\": \"required\" "
                      "mod may change the game (it would desync)",
                      mod->id.c_str(), objects::property(property)->name);
        }
        return TOME_ERR_UNAVAILABLE;
    }
    return write_property(object, type, property, key, value);
}

// --- the API ---

int32_t TOME_CALL api_object_type(const void* object) {
    if (ready("object_type") != TOME_OK) return TOME_OBJECT_NONE;
    return type_of(object);
}

void* TOME_CALL api_object_of(tome_jass_handle handle, int32_t* type) {
    if (type) *type = TOME_OBJECT_NONE;
    if (ready("object_of") != TOME_OK) return nullptr;
    void* object = handle_object(units::handle_manager(), handle, g_table);
    if (object && type) {
        *type = type_of(object);
        if (*type == TOME_OBJECT_NONE) *type = TOME_OBJECT_OTHER;
    }
    return object;
}

tome_jass_handle TOME_CALL api_handle_of(const void* object) {
    if (ready("handle_of") != TOME_OK || type_of(object) == TOME_OBJECT_NONE) return 0;
    return units::handle_of(object);
}

void* TOME_CALL api_player_object(int32_t slot) {
    if (ready("player_object") != TOME_OK || slot < 0 || slot >= kMaxPlayerSlots) return nullptr;
    return units::player_at(slot);
}

tome_result TOME_CALL api_unit_info(const void* unit, tome_unit* out, uint32_t unit_size) {
    if (!out || unit_size < sizeof(uint32_t)) return TOME_ERR_INVALID_ARGUMENT;
    if (tome_result r = ready("unit_info"); r != TOME_OK) return r;
    if (type_of(unit) != TOME_OBJECT_UNIT) return TOME_ERR_INVALID_ARGUMENT;
    tome_unit described;
    units::describe_unit(unit, described);
    units::copy_unit(described, out, unit_size);
    return TOME_OK;
}

// What collect_agent fills: the agents of one tome_object_type.
struct Collected {
    int32_t type;
    std::vector<void*> agents;
};

// ForEachAgent's callback: keeps the agents of the wanted class (by vtable,
// as type_of tells them apart: the subclass walk reaches other classes too).
// Non-zero goes on; 0 stops the walk (the walker's neg/sbb/inc turns a 0
// into its stop flag).
int __fastcall collect_agent(void* agent, void* context) {
    auto* collected = static_cast<Collected*>(context);
    if (kind_of(agent) == collected->type) collected->agents.push_back(agent);
    return 1;
}

int32_t TOME_CALL api_list_objects(int32_t type, void** out, int32_t max) {
    if (max < 0 || (max > 0 && !out) || type < TOME_OBJECT_UNIT || type > TOME_OBJECT_PLAYER) return -1;
    const tome_result r = ready("list_objects");
    if (r == TOME_ERR_UNAVAILABLE && g_installed && frames::on_main_thread()) return 0;  // no game
    if (r != TOME_OK) return -1;
    Collected collected{type, {}};
    std::vector<void*>& found = collected.agents;
    if (type == TOME_OBJECT_PLAYER) {
        for (int32_t slot = 0; slot < kMaxPlayerSlots; ++slot) {
            if (void* player = units::player_at(slot)) found.push_back(player);
        }
    } else {
        const uint32_t tag = g_build->objects.agent_tags[type - TOME_OBJECT_UNIT];
        if (!g_for_each_agent || !tag) return -1;
        // Units as GroupEnumUnitsOfPlayer walks them; items' class has no
        // instances of its own (the items are a subclass's), so items and
        // destructables are walked with the subclasses, and filtered.
        g_for_each_agent(tag, collect_agent, &collected, type == TOME_OBJECT_UNIT ? 0 : 1);
    }
    for (int32_t i = 0; i < max && i < static_cast<int32_t>(found.size()); ++i) out[i] = found[i];
    return static_cast<int32_t>(found.size());
}

tome_result TOME_CALL api_get_property(const void* object, int32_t property, double* value) {
    return get_keyed(object, property, 0, value);
}

tome_result TOME_CALL api_set_property(tome_mod* mod, void* object, int32_t property, double value) {
    return set_keyed(mod, object, property, 0, value);
}

tome_result TOME_CALL api_get_keyed_property(const void* object, int32_t property, uint32_t key, double* value) {
    return get_keyed(object, property, key, value);
}

tome_result TOME_CALL api_set_keyed_property(tome_mod* mod, void* object, int32_t property, uint32_t key,
                                             double value) {
    return set_keyed(mod, object, property, key, value);
}

// The script's string table entry for a JASS string id (what natives get as
// a string argument), or null.
const void* string_entry(uint32_t id) {
    const auto& o = g_build->objects;
    void* game = units::game_object();
    if (!game || !g_vm_get || !id) return nullptr;
    const void* vm = g_vm_get(field<int32_t>(game, o.vm_index_offset));
    const void* strings = vm ? field<const void*>(vm, o.vm_strings_offset) : nullptr;
    if (!strings || id >= field<uint32_t>(strings, o.string_count_offset)) return nullptr;
    const char* entries = field<const char*>(strings, o.string_entries_offset);
    return entries ? entries + size_t(id) * o.string_entry_size : nullptr;
}

tome_result TOME_CALL api_jass_call(tome_mod* mod, const char* name, const tome_jass_value* args, int32_t count,
                                    tome_jass_value* result) {
    if (result) std::memset(result, 0, sizeof(*result));
    if (!mod || !name || count < 0 || (count > 0 && !args)) return TOME_ERR_INVALID_ARGUMENT;
    JassCall call;
    if (tome_result r = prepare_jass_call(mod, name, count, call); r != TOME_OK) return r;
    return run_jass_call(mod, name, call, args, result);
}

} // namespace

tome_result prepare_jass_call(tome_mod* mod, const char* name, int32_t count, JassCall& out) {
    out = {};
    if (!mod || !name) return TOME_ERR_INVALID_ARGUMENT;
    if (tome_result r = ready("jass_call"); r != TOME_OK) return r;
    if (!g_jass_call_ok) return TOME_ERR_UNAVAILABLE;
    // Tome can't tell which natives only read (GetUnitLoc makes a handle,
    // which shifts every later id on this computer): in a multiplayer game
    // or a replay none run for a mod that isn't required, as set_property.
    if (mod_manager().game_locked() && !mod->sync_required) {
        if (warn_once(mod, "jass_call locked")) {
            TOME_WARN("[%s] jass_call(%s) refused: in a multiplayer game or replay only a \"sync\": \"required\" "
                      "mod may run natives (it could desync); get_property still reads",
                      mod->id.c_str(), name);
        }
        return TOME_ERR_UNAVAILABLE;
    }
    bool found = false;
    std::string error;
    {
        SharedLock hold(g_natives_lock);
        auto it = g_natives.find(name);
        if (it == g_natives.end() && g_natives.empty()) return TOME_ERR_UNAVAILABLE;  // no map has loaded yet
        found = it != g_natives.end();
        if (found) error = check_call(name, it->second.signature, count, out.sig);
        if (found && error.empty()) {
            out.fn = it->second.fn;
            // Which handle arguments are really integers (Convert* types, texttag).
            out.integer_handle.resize(out.sig.params.size());
            for (size_t i = 0; i < out.sig.params.size(); ++i) {
                const wasm::JassType& param = out.sig.params[i];
                out.integer_handle[i] =
                    param.kind == 'H' && (g_integer_types.count(param.handle) || handle_is_index(param.handle));
            }
        }
    }
    if (!found) {
        if (warn_once(mod, std::string("no native ") + name)) {
            TOME_ERROR("[%s] jass_call(%s): no such native", mod->id.c_str(), name);
        }
        return TOME_ERR_INVALID_ARGUMENT;
    }
    if (!error.empty()) {
        TOME_ERROR("[%s] jass_call: %s", mod->id.c_str(), error.c_str());
        return TOME_ERR_INVALID_ARGUMENT;
    }
    return TOME_OK;
}

tome_result run_jass_call(tome_mod* mod, const char* name, const JassCall& call, const tome_jass_value* args,
                          tome_jass_value* result) {
    if (result) std::memset(result, 0, sizeof(*result));
    if (!call.fn || (!call.sig.params.empty() && !args)) return TOME_ERR_INVALID_ARGUMENT;
    const wasm::JassSignature& sig = call.sig;
    const std::vector<bool>& integer_handle = call.integer_handle;

    void* manager = units::handle_manager();
    std::vector<uint32_t> words(sig.params.size());
    std::vector<float> reals(sig.params.size());
    std::vector<uint32_t> strings(sig.params.size());
    for (size_t i = 0; i < sig.params.size(); ++i) {
        const tome_jass_value& arg = args[i];
        switch (sig.params[i].kind) {
        case 'R':
            reals[i] = arg.real;
            words[i] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&reals[i]));  // natives take a float*
            break;
        case 'B':
            words[i] = arg.boolean != 0;
            break;
        case 'H':
            if (arg.handle && !integer_handle[i]) {
                void* object = handle_object(manager, arg.handle, g_table);
                if (!object) {
                    TOME_ERROR("[%s] jass_call(%s): argument %zu (%s) is %lu, which isn't a live handle",
                               mod->id.c_str(), name, i + 1, sig.params[i].handle.c_str(),
                               static_cast<unsigned long>(arg.handle));
                    return TOME_ERR_INVALID_ARGUMENT;
                }
                int32_t kind = type_of(object);
                if (kind == TOME_OBJECT_NONE) kind = TOME_OBJECT_OTHER;
                if (!handle_type_fits(sig.params[i].handle, kind)) {
                    TOME_ERROR("[%s] jass_call(%s): argument %zu is handle %lu, which isn't a %s", mod->id.c_str(),
                               name, i + 1, static_cast<unsigned long>(arg.handle), sig.params[i].handle.c_str());
                    return TOME_ERR_INVALID_ARGUMENT;
                }
            }
            words[i] = arg.handle;
            break;
        case 'C':
            if (arg.code && !natives::code_seen(arg.code)) {
                TOME_ERROR("[%s] jass_call(%s): argument %zu is code value %lu, which the map script didn't pass to "
                           "a Tome native", mod->id.c_str(), name, i + 1, static_cast<unsigned long>(arg.code));
                return TOME_ERR_INVALID_ARGUMENT;
            }
            words[i] = arg.code;
            break;
        case 'S':
            strings[i] = arg.string ? natives::create_string(arg.string) : 0;
            if (arg.string && !strings[i]) return TOME_ERR_FAILED;  // the game made no string
            break;
        default:  // 'I'
            words[i] = static_cast<uint32_t>(arg.integer);
            break;
        }
    }
    // Strings last: making one may grow the table, which moves the entries.
    for (size_t i = 0; i < sig.params.size(); ++i) {
        if (sig.params[i].kind != 'S' || !strings[i]) continue;
        const void* entry = string_entry(strings[i]);
        if (!entry) return TOME_ERR_FAILED;
        words[i] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(entry));
    }

    const uint32_t word =
        hooks::call_with_stack(call.fn, words.data(), static_cast<uint32_t>(words.size()));
    if (!result) return TOME_OK;
    switch (sig.result.kind) {
    case 'V': break;
    case 'R': std::memcpy(&result->real, &word, sizeof(float)); break;
    case 'B': result->boolean = word != 0; break;
    case 'S': result->string = natives::string_text(string_entry(word)); break;
    default:  result->handle = word; break;  // integer, handle and code are the same word
    }
    return TOME_OK;
}


bool is_player(const void* object) {
    return g_installed && units::is_player(object);
}

int32_t type_of(const void* object) {
    if (!object || !g_installed || !units::game_object()) return TOME_OBJECT_NONE;
    if (units::is_player(object)) return TOME_OBJECT_PLAYER;
    const int32_t kind = kind_of(object);
    return kind != TOME_OBJECT_NONE && agent_alive(object) ? kind : TOME_OBJECT_NONE;
}

AgentId agent_id(const void* object) {
    if (!object || !g_build) return {};
    const uint32_t id = g_build->play.agent_id_offset;
    return {field<uint32_t>(object, id), field<uint32_t>(object, id + 4)};
}

bool agent_id_alive(AgentId id) {
    return g_installed && units::game_object() && (id.a || id.b) && record_alive(id.a, id.b);
}

bool install(uintptr_t game_base, const BuildMappings& build, std::string& report) {
    g_base = game_base;
    g_build = &build;
    units::init(game_base, build);  // events::install does too; objects don't need events
    const auto& p = build.play;
    const auto& o = build.objects;
    g_table = {p.handle_entries_offset, o.handle_count_offset, o.handle_entry_size, o.handle_entry_object,
               p.handle_first};
    g_object_lookup = function<ObjectLookupFn>(o.object_lookup);
    g_unit_get_state = function<GetStateFn>(o.unit_get_state);
    g_unit_set_state = function<SetStateFn>(o.unit_set_state);
    g_unit_get_move_speed = function<GetFloatFn>(o.unit_get_move_speed);
    g_unit_set_move_speed = function<SetFloatFn>(o.unit_set_move_speed);
    g_unit_get_level = function<GetIntFn>(o.unit_get_level);
    g_position_get = function<PositionGetFn>(o.position_get);
    g_position_set = function<PositionSetFn>(o.position_set);
    g_player_set_state = function<PlayerSetStateFn>(o.player_set_state);
    g_vm_get = function<VmGetFn>(o.jass_vm_get);
    g_float_multiply = function<FloatMultiplyFn>(o.float_multiply);
    g_position_set_facing = function<SetFacingFn>(o.position_set_facing);
    g_unit_find_ability = function<FindAbilityFn>(o.unit_find_ability);
    g_ability_max_level = function<GetIntFn>(o.ability_max_level);
    g_unit_get_acquire_range = function<GetFloatFn>(o.unit_get_acquire_range);
    g_unit_set_acquire_range = function<SetFloatFn>(o.unit_set_acquire_range);
    g_unit_get_hero_xp = function<GetIntFn>(o.unit_get_hero_xp);
    g_for_each_agent = function<ForEachAgentFn>(o.for_each_agent);
    for (int i = 0; i < 3; ++i) {
        g_unit_get_hero_stat[i] = function<HeroStatFn>(o.unit_get_hero_stat[i]);
        g_unit_set_hero_stat[i] = function<SetHeroStatFn>(o.unit_set_hero_stat[i]);
    }

    // A property is there if every symbol it needs is mapped (the same
    // check "requires": ["property:..."] makes: core/objects.h).
    const auto mapped = [&](const char* name) { return build.data->find(name) != nullptr; };
    int available = 0, total = 0;
    for (int32_t id = 0; id < static_cast<int32_t>(std::size(g_available)); ++id) {
        const PropertyInfo* info = objects::property(id);
        if (!info) continue;
        const bool ok = !missing_symbol(*info, mapped);
        g_available[id] = ok;
        available += ok;
        ++total;
    }
    // jass_call reads the handle table (handle arguments, new ids) and the
    // script's string table (string arguments and results): all of it mapped.
    const char* const kJassCall[] = {"JassVM::Get",          "CGameWar3.vm_index",      "JassVM.strings",
                                     "JassStringTable.count", "JassStringTable.entries", "JassString.size",
                                     "HandleManager.count",   "HandleEntry.size",        "HandleEntry.object"};
    g_jass_call_ok = std::all_of(std::begin(kJassCall), std::end(kJassCall), mapped);

    std::string error;
    hooks::Target* bind = hooks::find_or_install("BindNative", error);
    if (!bind) {
        report = "BindNative: " + error;
        return false;
    }
    hooks::Handler after;
    after.phase = TOME_HOOK_AFTER;
    after.last = true;  // after mods' handlers, which may change what gets bound
    after.fn = after_bind_native;
    hooks::add(bind, after);
    g_installed = true;
    report = std::to_string(available) + " of " + std::to_string(total) + " properties mapped" +
             (g_jass_call_ok ? "; jass_call learns the natives as maps load them" : "; no jass_call");
    return true;
}

void fill_api(tome_api& api) {
    api.object_type = api_object_type;
    api.object_of = api_object_of;
    api.handle_of = api_handle_of;
    api.player_object = api_player_object;
    api.unit_info = api_unit_info;
    api.get_property = api_get_property;
    api.set_property = api_set_property;
    api.jass_call = api_jass_call;
    api.get_keyed_property = api_get_keyed_property;
    api.set_keyed_property = api_set_keyed_property;
    api.list_objects = api_list_objects;
}

} // namespace tome::objects
