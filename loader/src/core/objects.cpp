// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/objects.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace tome::objects {
namespace {

constexpr uint32_t kUnit = bit(TOME_OBJECT_UNIT);
constexpr uint32_t kItem = bit(TOME_OBJECT_ITEM);
constexpr uint32_t kDestructable = bit(TOME_OBJECT_DESTRUCTABLE);
constexpr uint32_t kPlayer = bit(TOME_OBJECT_PLAYER);

// Every property of units, items and destructables also needs the object
// checks (ObjectLookup and the agent record's fields; objects.cpp,
// type_of): missing_symbol adds them.
constexpr const char* kAgentChecks[] = {"ObjectLookup", "AgentRecord.tag", "AgentRecord.dying", "AgentTag.agent"};
constexpr const char* kPlayerResource[] = {"SyncedInt::Get", "CPlayerWar3.states", "CPlayerWar3::SetState"};

const PropertyInfo kProperties[] = {
    {TOME_PROPERTY_LIFE, "life", kUnit | kItem | kDestructable, kUnit | kItem | kDestructable, Key::None, 0, 0, false,
     {"CUnit::GetState", "CUnit::SetState", "CWidget.vtable.GetLife", "CWidget.vtable.SetLife"}},
    {TOME_PROPERTY_MAX_LIFE, "max_life", kUnit, 0, Key::None, 0, 0, false, {"CUnit::GetState"}},
    {TOME_PROPERTY_MANA, "mana", kUnit, kUnit, Key::None, 0, 0, false, {"CUnit::GetState", "CUnit::SetState"}},
    {TOME_PROPERTY_MAX_MANA, "max_mana", kUnit, 0, Key::None, 0, 0, false, {"CUnit::GetState"}},
    {TOME_PROPERTY_X, "x", kUnit | kItem | kDestructable, kUnit, Key::None, 0, 0, false,
     {"CUnit.vtable.GetPosition", "CPosition::Get", "CPosition::Set"}},
    {TOME_PROPERTY_Y, "y", kUnit | kItem | kDestructable, kUnit, Key::None, 0, 0, false,
     {"CUnit.vtable.GetPosition", "CPosition::Get", "CPosition::Set"}},
    {TOME_PROPERTY_MOVE_SPEED, "move_speed", kUnit, kUnit, Key::None, 0, 0, false,
     {"CUnit::GetMoveSpeed", "CUnit::SetMoveSpeed"}},
    {TOME_PROPERTY_LEVEL, "level", kUnit, 0, Key::None, 0, 0, false, {"CUnit::GetLevel", "CUnit::GetHeroLevel"}},
    {TOME_PROPERTY_TYPE_ID, "type_id", kUnit | kItem | kDestructable, 0, Key::None, 0, 0, false,
     {"CUnit.type_id", "CItem.type_id", "CDestructable.type_id"}},
    {TOME_PROPERTY_OWNER, "owner", kUnit, 0, Key::None, 0, 0, false, {"CUnit.owner"}},
    {TOME_PROPERTY_CHARGES, "charges", kItem, 0, Key::None, 0, 0, false, {"CItem.charges"}},
    {TOME_PROPERTY_SLOT, "slot", kPlayer, 0, Key::None, 0, 0, false, {"CPlayerWar3.slot"}},
    {TOME_PROPERTY_GOLD, "gold", kPlayer, kPlayer, Key::None, 0, 0, false,
     {kPlayerResource[0], kPlayerResource[1], kPlayerResource[2]}},
    {TOME_PROPERTY_LUMBER, "lumber", kPlayer, kPlayer, Key::None, 0, 0, false,
     {kPlayerResource[0], kPlayerResource[1], kPlayerResource[2]}},
    {TOME_PROPERTY_FOOD_CAP, "food_cap", kPlayer, kPlayer, Key::None, 0, 0, false,
     {kPlayerResource[0], kPlayerResource[1], kPlayerResource[2]}},
    {TOME_PROPERTY_FOOD_USED, "food_used", kPlayer, kPlayer, Key::None, 0, 0, false,
     {kPlayerResource[0], kPlayerResource[1], kPlayerResource[2]}},
    {TOME_PROPERTY_FACING, "facing", kUnit, kUnit, Key::None, 0, 0, false,
     {"CUnit.vtable.GetPosition", "CPosition.vtable.GetFacing", "CPosition::SetFacing", "FloatMultiply"}},
    {TOME_PROPERTY_FLY_HEIGHT, "fly_height", kUnit, 0, Key::None, 0, 0, false, {"CUnit.fly_height"}},
    {TOME_PROPERTY_ACQUIRE_RANGE, "acquire_range", kUnit, kUnit, Key::None, 0, 0, false,
     {"CUnit::GetAcquireRange", "CUnit::SetAcquireRange"}},
    {TOME_PROPERTY_HERO_XP, "hero_xp", kUnit, 0, Key::None, 0, 0, true, {"CUnit::GetHeroXP"}},
    {TOME_PROPERTY_STRENGTH, "strength", kUnit, kUnit, Key::Optional, 1, 0, true,
     {"CUnit::GetHeroStr", "CUnit::SetHeroStr"}},
    {TOME_PROPERTY_AGILITY, "agility", kUnit, kUnit, Key::Optional, 1, 0, true,
     {"CUnit::GetHeroAgi", "CUnit::SetHeroAgi"}},
    {TOME_PROPERTY_INTELLIGENCE, "intelligence", kUnit, kUnit, Key::Optional, 1, 0, true,
     {"CUnit::GetHeroInt", "CUnit::SetHeroInt"}},
    {TOME_PROPERTY_ABILITY_LEVEL, "ability_level", kUnit, kUnit, Key::Required, 0, 0, false,
     {"CUnit::FindAbility", "CAbility.level", "CAbility::GetMaxLevel", "CAbility.vtable.LevelUp",
      "CAbility.vtable.LevelDown"}},
    {TOME_PROPERTY_ARMOR, "armor", kUnit, 0, Key::None, 0, 0, false, {"CUnit.armor"}},
    {TOME_PROPERTY_ARMOR_TYPE, "armor_type", kUnit, 0, Key::None, 0, 0, false, {"CUnit.armor_type"}},
    {TOME_PROPERTY_DAMAGE_BASE, "damage_base", kUnit, 0, Key::Optional, 1, 0, false,
     {"CUnit.attack", "CAbilityAttack.damage_base"}},
    {TOME_PROPERTY_DAMAGE_DICE, "damage_dice", kUnit, 0, Key::Optional, 1, 0, false,
     {"CUnit.attack", "CAbilityAttack.damage_dice"}},
    {TOME_PROPERTY_DAMAGE_SIDES, "damage_sides", kUnit, 0, Key::Optional, 1, 0, false,
     {"CUnit.attack", "CAbilityAttack.damage_sides"}},
    {TOME_PROPERTY_ATTACK_COOLDOWN, "attack_cooldown", kUnit, 0, Key::Optional, 1, 0, false,
     {"CUnit.attack", "CAbilityAttack.cooldown"}},
    {TOME_PROPERTY_ATTACK_RANGE, "attack_range", kUnit, 0, Key::Optional, 1, 0, false,
     {"CUnit.attack", "CAbilityAttack.range"}},
};

uint32_t read_u32(uintptr_t address) {
    uint32_t value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

} // namespace

const PropertyInfo* property(int32_t id) {
    for (const PropertyInfo& p : kProperties) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

const PropertyInfo* property_by_name(std::string_view name) {
    for (const PropertyInfo& p : kProperties) {
        if (name == p.name) return &p;
    }
    return nullptr;
}

tome_result check_access(const PropertyInfo& p, int32_t type, uint32_t key, bool set) {
    if (type <= TOME_OBJECT_NONE || type >= 32 || !((set ? p.set_on : p.on) & bit(type))) {
        return TOME_ERR_INVALID_ARGUMENT;
    }
    switch (p.key) {
    case Key::None:     return key == 0 ? TOME_OK : TOME_ERR_INVALID_ARGUMENT;
    case Key::Required: return key != 0 ? TOME_OK : TOME_ERR_INVALID_ARGUMENT;
    case Key::Optional: return key <= (set ? p.max_set_key : p.max_key) ? TOME_OK : TOME_ERR_INVALID_ARGUMENT;
    }
    return TOME_ERR_INVALID_ARGUMENT;
}

bool capability_met(std::string_view capability, const std::function<bool(const char*)>& mapped,
                    std::string& error) {
    constexpr std::string_view kProperty = "property:";
    if (capability.substr(0, kProperty.size()) != kProperty) return true;
    std::string_view name = capability.substr(kProperty.size());
    const PropertyInfo* p = property_by_name(name);
    if (!p) {
        error = "needs the property \"" + std::string(name) + "\", which this version of Tome doesn't have";
        return false;
    }
    if (const char* symbol = missing_symbol(*p, mapped)) {
        error = "needs the property \"" + std::string(name) + "\", which isn't mapped for this game version (" +
                symbol + ")";
        return false;
    }
    return true;
}

const char* missing_symbol(const PropertyInfo& p, const std::function<bool(const char*)>& mapped) {
    for (const char* symbol : p.symbols) {
        if (symbol && !mapped(symbol)) return symbol;
    }
    if (p.on != kPlayer) {
        for (const char* symbol : kAgentChecks) {
            if (!mapped(symbol)) return symbol;
        }
    }
    return nullptr;
}

bool to_int(double value, int32_t& out) {
    if (!std::isfinite(value)) return false;
    double t = std::trunc(value);
    if (t <= static_cast<double>(std::numeric_limits<int32_t>::min())) {
        out = std::numeric_limits<int32_t>::min();
    } else if (t >= static_cast<double>(std::numeric_limits<int32_t>::max())) {
        out = std::numeric_limits<int32_t>::max();
    } else {
        out = static_cast<int32_t>(t);
    }
    return true;
}

bool to_float(double value, float& out) {
    if (!std::isfinite(value)) return false;
    constexpr double kMax = std::numeric_limits<float>::max();
    out = static_cast<float>(value < -kMax ? -kMax : value > kMax ? kMax : value);
    return true;
}

void* handle_object(const void* manager, uint32_t handle, const HandleTable& table) {
    if (!manager || !table.count || !table.object || !table.entry_size || handle < table.first) return nullptr;
    auto base = reinterpret_cast<uintptr_t>(manager);
    uint32_t index = handle - table.first;
    uintptr_t entries = read_u32(base + table.entries);
    if (!entries || index >= read_u32(base + table.count)) return nullptr;
    uintptr_t entry = entries + uintptr_t(index) * table.entry_size;
    if (read_u32(entry) == 0) return nullptr;  // no references: a freed id
    return reinterpret_cast<void*>(static_cast<uintptr_t>(read_u32(entry + table.object)));
}

bool handle_type_fits(std::string_view jass_type, int32_t object_type) {
    if (jass_type == "handle" || jass_type == "agent") return true;
    if (jass_type == "unit") return object_type == TOME_OBJECT_UNIT;
    if (jass_type == "item") return object_type == TOME_OBJECT_ITEM;
    if (jass_type == "destructable") return object_type == TOME_OBJECT_DESTRUCTABLE;
    if (jass_type == "player") return object_type == TOME_OBJECT_PLAYER;
    if (jass_type == "widget") {
        return object_type == TOME_OBJECT_UNIT || object_type == TOME_OBJECT_ITEM ||
               object_type == TOME_OBJECT_DESTRUCTABLE;
    }
    return object_type == TOME_OBJECT_OTHER;
}

bool handle_is_index(std::string_view jass_type) {
    return jass_type == "texttag";
}

std::string check_call(std::string_view native, std::string_view signature, int32_t count,
                       wasm::JassSignature& parsed) {
    if (!wasm::parse_signature(signature, parsed)) {
        return "native " + std::string(native) + " has a signature Tome can't read (" + std::string(signature) + ")";
    }
    if (count < 0 || static_cast<size_t>(count) != parsed.params.size()) {
        return "native " + std::string(native) + " takes " + std::to_string(parsed.params.size()) + " argument" +
               (parsed.params.size() == 1 ? "" : "s") + " (" + std::string(signature) + "), not " +
               std::to_string(count);
    }
    return "";
}

} // namespace tome::objects
