// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/objects.h"
#include "test.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <set>

using namespace tome::objects;

TEST(objects_property_table) {
    // Every tome_property has an entry, found by id and by name, with a unique name.
    std::set<std::string> names;
    for (int32_t id = TOME_PROPERTY_LIFE; id <= TOME_PROPERTY_ATTACK_RANGE; ++id) {
        const PropertyInfo* p = property(id);
        CHECK(p != nullptr);
        if (!p) continue;
        CHECK_EQ(p->id, id);
        CHECK(p->on != 0);
        CHECK((p->set_on & ~p->on) == 0);  // set only where it can be read
        CHECK(p->symbols[0] != nullptr);
        CHECK(names.insert(p->name).second);
        CHECK(property_by_name(p->name) == p);
    }
    CHECK(property(0) == nullptr);
    CHECK(property(TOME_PROPERTY_ATTACK_RANGE + 1) == nullptr);
    CHECK(property_by_name("Life") == nullptr);

    CHECK(property(TOME_PROPERTY_LIFE)->on ==
          (bit(TOME_OBJECT_UNIT) | bit(TOME_OBJECT_ITEM) | bit(TOME_OBJECT_DESTRUCTABLE)));
    CHECK(property(TOME_PROPERTY_LIFE)->set_on == property(TOME_PROPERTY_LIFE)->on);
    CHECK(property(TOME_PROPERTY_MAX_LIFE)->set_on == 0);  // SetUnitState does nothing for it either
    CHECK(property(TOME_PROPERTY_LEVEL)->set_on == 0);
    CHECK(property(TOME_PROPERTY_X)->set_on == bit(TOME_OBJECT_UNIT));  // items and destructables: read only
    CHECK(property(TOME_PROPERTY_STRENGTH)->heroes_only);
    CHECK(property(TOME_PROPERTY_GOLD)->on == bit(TOME_OBJECT_PLAYER));
    CHECK(property(TOME_PROPERTY_CHARGES)->on == bit(TOME_OBJECT_ITEM));
}

TEST(objects_check_access) {
    const PropertyInfo& x = *property(TOME_PROPERTY_X);
    CHECK_EQ(check_access(x, TOME_OBJECT_UNIT, 0, true), TOME_OK);
    CHECK_EQ(check_access(x, TOME_OBJECT_ITEM, 0, false), TOME_OK);
    CHECK_EQ(check_access(x, TOME_OBJECT_ITEM, 0, true), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(check_access(x, TOME_OBJECT_PLAYER, 0, false), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(check_access(x, TOME_OBJECT_NONE, 0, false), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(check_access(x, TOME_OBJECT_UNIT, 1, false), TOME_ERR_INVALID_ARGUMENT);  // takes no key
    // An optional key: 0 the base value (get and set), 1 with bonuses (get only).
    const PropertyInfo& str = *property(TOME_PROPERTY_STRENGTH);
    CHECK_EQ(check_access(str, TOME_OBJECT_UNIT, 0, true), TOME_OK);
    CHECK_EQ(check_access(str, TOME_OBJECT_UNIT, 1, false), TOME_OK);
    CHECK_EQ(check_access(str, TOME_OBJECT_UNIT, 1, true), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(check_access(str, TOME_OBJECT_UNIT, 2, false), TOME_ERR_INVALID_ARGUMENT);
    // A key it needs: an ability's id.
    const PropertyInfo& ability = *property(TOME_PROPERTY_ABILITY_LEVEL);
    CHECK_EQ(check_access(ability, TOME_OBJECT_UNIT, 0, false), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(check_access(ability, TOME_OBJECT_UNIT, 0x41486277, true), TOME_OK);
}

TEST(objects_capability_met) {
    std::set<std::string> mapped = {"CUnit::GetState",   "CUnit::SetState",   "ObjectLookup",
                                    "AgentRecord.tag",   "AgentRecord.dying", "AgentTag.agent",
                                    "SyncedInt::Get",    "CPlayerWar3.states", "CPlayerWar3::SetState"};
    auto has = [&](const char* name) { return mapped.count(name) != 0; };
    std::string error;
    CHECK(capability_met("property:mana", has, error));
    CHECK(capability_met("property:max_life", has, error));
    CHECK(!capability_met("property:x", has, error));
    CHECK(error.find("x") != std::string::npos && error.find("CUnit.vtable.GetPosition") != std::string::npos);
    error.clear();
    CHECK(!capability_met("property:wings", has, error));
    CHECK(error.find("this version of Tome") != std::string::npos);
    // Other capabilities aren't checked yet.
    CHECK(capability_met("event:frame", has, error));

    // A unit's property also needs the agent checks every use makes, as
    // objects::install decides; a player's doesn't.
    mapped.erase("AgentRecord.dying");
    error.clear();
    CHECK(!capability_met("property:mana", has, error));
    CHECK(error.find("AgentRecord.dying") != std::string::npos);
    CHECK(capability_met("property:gold", has, error));
    CHECK(missing_symbol(*property(TOME_PROPERTY_GOLD), has) == nullptr);
    CHECK(std::strcmp(missing_symbol(*property(TOME_PROPERTY_MANA), has), "AgentRecord.dying") == 0);
}

TEST(objects_to_int) {
    int32_t n = 0;
    CHECK(to_int(12.9, n) && n == 12);
    CHECK(to_int(-12.9, n) && n == -12);
    CHECK(to_int(1e12, n) && n == std::numeric_limits<int32_t>::max());
    CHECK(to_int(-1e12, n) && n == std::numeric_limits<int32_t>::min());
    CHECK(!to_int(std::nan(""), n));
    CHECK(!to_int(std::numeric_limits<double>::infinity(), n));
    float f = 0;
    CHECK(to_float(522.5, f) && f == 522.5f);
    CHECK(to_float(1e300, f) && f == std::numeric_limits<float>::max());
    CHECK(!to_float(-std::numeric_limits<double>::infinity(), f));
}

TEST(objects_handle_object) {
    static_assert(sizeof(void*) == 4, "the game's structures hold 32-bit pointers");
    const HandleTable table = {0x19c, 0x198, 12, 4, 0x100000};
    uint32_t a = 0, b = 0;  // stand-ins for objects
    auto addr = [](const void* p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); };
    uint32_t entries[3 * 3] = {
        1, addr(&a), 0,  // 0x100000: one reference
        0, addr(&b), 0,  // 0x100001: freed (no references)
        3, addr(&b), 0,  // 0x100002
    };
    alignas(4) unsigned char manager[0x200] = {};
    auto put = [&](uint32_t offset, uint32_t value) { std::memcpy(manager + offset, &value, 4); };
    put(0x19c, addr(entries));
    put(0x198, 3);

    CHECK(handle_object(manager, 0x100000, table) == &a);
    CHECK(handle_object(manager, 0x100001, table) == nullptr);
    CHECK(handle_object(manager, 0x100002, table) == &b);
    CHECK(handle_object(manager, 0x100003, table) == nullptr);  // past the ids handed out
    CHECK(handle_object(manager, 0xFFFFF, table) == nullptr);
    CHECK(handle_object(manager, 0, table) == nullptr);
    CHECK(handle_object(nullptr, 0x100000, table) == nullptr);
    HandleTable unmapped = table;
    unmapped.count = 0;
    CHECK(handle_object(manager, 0x100000, unmapped) == nullptr);
}

TEST(objects_index_handles) {
    CHECK(handle_is_index("texttag"));
    CHECK(!handle_is_index("image"));      // its natives don't check the id
    CHECK(!handle_is_index("ubersplat"));
    CHECK(!handle_is_index("unit"));
    CHECK(!handle_is_index("unitstate"));  // a Convert* type: learned from BindNative
}

TEST(objects_check_call) {
    tome::wasm::JassSignature sig;
    CHECK_EQ(check_call("SetUnitX", "(Hunit;R)V", 2, sig), std::string());
    CHECK(sig.params.size() == 2 && sig.params[0].kind == 'H' && sig.params[1].kind == 'R' && sig.result.kind == 'V');
    CHECK_EQ(check_call("GetUnitName", "(Hunit;)S", 1, sig), std::string());
    CHECK(sig.result.kind == 'S');
    std::string error = check_call("SetUnitX", "(Hunit;R)V", 1, sig);
    CHECK(error.find("takes 2 arguments") != std::string::npos);
    CHECK(check_call("GetLocalPlayer", "()Hplayer;", 0, sig).empty());
    CHECK(check_call("GetLocalPlayer", "()Hplayer;", 1, sig).find("takes 0 arguments") != std::string::npos);
    CHECK(check_call("Broken", "(Hunit", 1, sig).find("can't read") != std::string::npos);
    CHECK(!check_call("SetUnitX", "(Hunit;R)V", -1, sig).empty());
}

TEST(objects_handle_type_fits) {
    CHECK(handle_type_fits("unit", TOME_OBJECT_UNIT));
    CHECK(!handle_type_fits("unit", TOME_OBJECT_ITEM));
    CHECK(!handle_type_fits("unit", TOME_OBJECT_OTHER));  // a timer as a unit
    CHECK(handle_type_fits("widget", TOME_OBJECT_DESTRUCTABLE));
    CHECK(!handle_type_fits("widget", TOME_OBJECT_PLAYER));
    CHECK(handle_type_fits("player", TOME_OBJECT_PLAYER));
    CHECK(handle_type_fits("timer", TOME_OBJECT_OTHER));
    CHECK(!handle_type_fits("timer", TOME_OBJECT_UNIT));   // a unit as a timer
    CHECK(!handle_type_fits("group", TOME_OBJECT_PLAYER));
    CHECK(handle_type_fits("handle", TOME_OBJECT_UNIT));
    CHECK(handle_type_fits("agent", TOME_OBJECT_OTHER));
    CHECK(!handle_type_fits("timer", TOME_OBJECT_NONE));
}
