// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/wasm_layout.h"
#include "test.h"

#include <cstring>

using namespace tome;
using namespace tome::wasm;

namespace {

uint32_t u32_at(const std::string& b, uint32_t at) {
    uint32_t v = 0;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}

double f64_at(const std::string& b, uint32_t at) {
    double v = 0;
    std::memcpy(&v, b.data() + at, 8);
    return v;
}

// The string a pointer field points at, in a block encoded for `base`.
std::string str_at(const std::string& b, uint32_t at, uint32_t base) {
    const uint32_t p = u32_at(b, at);
    if (p == 0) return "(null)";
    if (p < base || p - base >= b.size()) return "(outside)";
    return std::string(b.data() + (p - base));
}

// Handles: the kind in the top byte, a count below, so tests can tell them apart.
struct FakeHandles {
    std::vector<const void*> seen;
    Handles fn() {
        return [this](HandleKind kind, const void* object) -> uint32_t {
            if (!object) return 0;
            seen.push_back(object);
            return (uint32_t(kind) << 24) | uint32_t(seen.size());
        };
    }
};

} // namespace

TEST(wasm_layout_event) {
    int item = 0, destructable = 0, u1 = 0, u2 = 0;
    tome_unit units[2] = {};
    units[0] = {sizeof(tome_unit), &u1, 7, 0x68706561, "Peasant", 0, TOME_UNIT_HERO};
    units[1] = {sizeof(tome_unit), &u2, 8, 0x68666f6f, "Footman", 1, 0};
    tome_event_info info{};
    info.size = sizeof(info);
    info.event = TOME_EVENT_SELECTION;
    info.map_name = "Dustwallow Keys";
    info.map_path = "Maps\\(10)DustwallowKeys.w3m";
    info.delta = 0.25;
    info.frame = 42;
    info.game_time = 12.5;
    info.map_description = nullptr;
    info.units = units;
    info.unit_count = 2;
    info.leader = 1;
    info.player = 3;
    info.player_name = "Ana";
    info.text = "gg";
    info.unit = &units[1];
    info.other = nullptr;
    info.item = &item;
    info.object_id = 0x52686d65;
    info.object_name = "Iron Forged Swords";
    info.x = -1.5;
    info.y = 2.5;
    info.amount = 100;
    info.destructable = &destructable;
    info.previous = 99;
    info.script_shadows = 3;

    FakeHandles h;
    const uint32_t base = 0x10000;
    const std::string b = encode_event(info, h.fn(), base);
    CHECK(b.size() > kEventSize);
    CHECK_EQ(u32_at(b, 0), kEventSize);
    CHECK_EQ(u32_at(b, 4), uint32_t(TOME_EVENT_SELECTION));
    CHECK_EQ(str_at(b, 8, base), std::string("Dustwallow Keys"));
    CHECK_EQ(str_at(b, 12, base), std::string("Maps\\(10)DustwallowKeys.w3m"));
    CHECK(f64_at(b, 16) == 0.25);
    CHECK_EQ(u32_at(b, 24), 42u);
    CHECK(f64_at(b, 32) == 12.5);
    CHECK_EQ(u32_at(b, 40), 0u);  // a null string stays null
    // The units: an array after the struct, names after that.
    const uint32_t array = u32_at(b, 44);
    CHECK(array >= base + kEventSize && array % 4 == 0);
    CHECK_EQ(u32_at(b, 48), 2u);
    CHECK_EQ(u32_at(b, 52), 1u);
    const uint32_t first = array - base;
    CHECK_EQ(u32_at(b, first), kUnitSize);
    CHECK_EQ(u32_at(b, first + 4) >> 24, uint32_t(HandleKind::Unit));  // a handle, not the pointer
    CHECK_EQ(u32_at(b, first + 8), 7u);
    CHECK_EQ(u32_at(b, first + 12), 0x68706561u);
    CHECK_EQ(str_at(b, first + 16, base), std::string("Peasant"));
    CHECK_EQ(u32_at(b, first + 24), uint32_t(TOME_UNIT_HERO));
    CHECK_EQ(str_at(b, first + kUnitSize + 16, base), std::string("Footman"));
    CHECK_EQ(u32_at(b, 56), 3u);
    CHECK_EQ(str_at(b, 60, base), std::string("Ana"));
    CHECK_EQ(str_at(b, 64, base), std::string("gg"));
    const uint32_t unit = u32_at(b, 72) - base;
    CHECK_EQ(str_at(b, unit + 16, base), std::string("Footman"));
    CHECK_EQ(u32_at(b, 76), 0u);
    CHECK_EQ(u32_at(b, 80) >> 24, uint32_t(HandleKind::Item));
    CHECK_EQ(str_at(b, 88, base), std::string("Iron Forged Swords"));
    CHECK(f64_at(b, 112) == -1.5);
    CHECK(f64_at(b, 120) == 2.5);
    CHECK(f64_at(b, 128) == 100);
    CHECK_EQ(u32_at(b, 136) >> 24, uint32_t(HandleKind::Destructable));
    CHECK(f64_at(b, 168) == 99);
    CHECK_EQ(u32_at(b, 176), 3u);
    // No game pointer anywhere in the block.
    for (const void* p : h.seen) {
        const uint32_t word = uint32_t(reinterpret_cast<uintptr_t>(p));
        bool found = false;
        for (size_t i = 0; i + 4 <= b.size(); ++i) found = found || u32_at(b, uint32_t(i)) == word;
        CHECK(!found);
    }
    // The size doesn't depend on the base: encode once for it, then again.
    CHECK_EQ(encode_event(info, h.fn(), 0).size(), b.size());
}

TEST(wasm_layout_event_from_an_older_struct) {
    // A tome_event_info of API version 5: no selection, chat or unit fields.
    tome_event_info info{};
    info.size = offsetof(tome_event_info, map_description);
    info.event = TOME_EVENT_TICK;
    info.frame = 7;
    info.player = 5;  // not part of a version 5 struct: not read
    FakeHandles h;
    const std::string b = encode_event(info, h.fn(), 0);
    CHECK_EQ(b.size(), size_t(kEventSize));
    CHECK_EQ(u32_at(b, 24), 7u);
    CHECK_EQ(u32_at(b, 56), 0u);
    CHECK_EQ(u32_at(b, 8), 0u);
}

TEST(wasm_layout_units_info_shadows) {
    int u = 0;
    tome_unit units[1] = {{sizeof(tome_unit), &u, 9, 1, "Grunt", 2, 0}};
    FakeHandles h;
    std::string b = encode_units(units, 1, h.fn(), 100);
    CHECK_EQ(u32_at(b, 0), kUnitSize);
    CHECK_EQ(str_at(b, 16, 100), std::string("Grunt"));
    CHECK_EQ(u32_at(b, 20), 2u);

    b = encode_game_info("1.27b", "1.27.1.7085", TOME_EDITION_REIGN_OF_CHAOS, 64);
    CHECK_EQ(b.size(), size_t(kGameInfoSize + 6 + 12));
    CHECK_EQ(str_at(b, 4, 64), std::string("1.27b"));
    CHECK_EQ(str_at(b, 8, 64), std::string("1.27.1.7085"));
    CHECK_EQ(u32_at(b, 12), uint32_t(TOME_EDITION_REIGN_OF_CHAOS));

    tome_script_shadow s[1] = {{sizeof(tome_script_shadow), "bj_lastCreatedUnit", "integer", "unit", 12}};
    b = encode_shadows(s, 1, 0);
    CHECK_EQ(str_at(b, 4, 0), std::string("bj_lastCreatedUnit"));
    CHECK_EQ(str_at(b, 12, 0), std::string("unit"));
    CHECK_EQ(u32_at(b, 16), 12u);
}

TEST(wasm_layout_signatures) {
    JassSignature s;
    CHECK(parse_signature("(Hunit;RS)V", s));
    CHECK_EQ(s.params.size(), size_t(3));
    CHECK(s.params[0].kind == 'H' && s.params[0].handle == "unit");
    CHECK(s.params[1].kind == 'R' && s.params[2].kind == 'S');
    CHECK(s.result.kind == 'V');
    CHECK(parse_signature("()Hunit;", s) && s.params.empty() && s.result.handle == "unit");
    CHECK(parse_signature("(IBC)I", s));
    CHECK(!parse_signature("", s));
    CHECK(!parse_signature("(I", s));
    CHECK(!parse_signature("(V)I", s));      // nothing isn't a parameter
    CHECK(!parse_signature("(X)I", s));
    CHECK(!parse_signature("(H;)I", s));     // no handle type
    CHECK(!parse_signature("(Hun it;)I", s));
    CHECK(!parse_signature("()I extra", s));
    CHECK(!parse_signature("(I)", s));
}

TEST(wasm_layout_native_args_and_results) {
    JassSignature s;
    CHECK(parse_signature("(ISHunit;S)Hunit;", s));
    std::vector<NativeArg> args(4);
    args[0].word = 42;
    args[1].text = "hello";
    args[2].word = 0x100005;
    args[3].null = true;
    const std::string b = encode_native_args(s, args, 1000);
    CHECK_EQ(u32_at(b, 0), 42u);
    CHECK_EQ(str_at(b, 4, 1000), std::string("hello"));
    CHECK_EQ(u32_at(b, 8), 0x100005u);
    CHECK_EQ(u32_at(b, 12), 0u);  // a null string

    CHECK(valid_native_result(s, args, 0));
    CHECK(valid_native_result(s, args, 0x100005));  // its own unit argument
    CHECK(!valid_native_result(s, args, 42));       // an integer argument, not a unit
    CHECK(!valid_native_result(s, args, 0x100006)); // made up

    JassSignature other;
    CHECK(parse_signature("(Hitem;)Hunit;", other));
    std::vector<NativeArg> item(1);
    item[0].word = 0x100005;
    CHECK(!valid_native_result(other, item, 0x100005));  // an item isn't a unit

    JassSignature code;
    CHECK(parse_signature("()C", code));
    CHECK(!valid_native_result(code, {}, 1));
    JassSignature integer;
    CHECK(parse_signature("()I", integer));
    CHECK(valid_native_result(integer, {}, 12345));
}
