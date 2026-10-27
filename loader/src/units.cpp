#include "units.h"

#include "frames.h"
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

// GCC warns that __thiscall is meant for class methods, but it does apply it
// to function pointer types (see mod_menu.cpp).
#pragma GCC diagnostic ignored "-Wattributes"

namespace tome::units {
namespace {

using GetPlayerFn = void*(__thiscall*)(void* game, int slot);
using TypeNameFn = const char*(__fastcall*)(uint32_t type_id, uint32_t index);
using StringTextFn = const char*(__thiscall*)(const void* string);
using HeroLevelFn = int(__thiscall*)(const void* unit);
using OrderTargetFn = void*(__thiscall*)(const void* order);

// The selection holds at most this many (CSelectionWar3, 0x28DBE0); more
// would mean the list isn't what it should be.
constexpr size_t kMaxSelected = 12;

uintptr_t g_base = 0;
const BuildMappings* g_build = nullptr;
GetPlayerFn g_get_player = nullptr;
TypeNameFn g_type_name = nullptr;
StringTextFn g_string_text = nullptr;
HeroLevelFn g_hero_level = nullptr;
OrderTargetFn g_order_target = nullptr;
uintptr_t g_unit_vtable = 0, g_item_vtable = 0;
FlagBits g_flag_bits;
HandleLayout g_handle_layout;

template <typename T>
T field(const void* object, uint32_t offset) {
    T value;
    std::memcpy(&value, static_cast<const char*>(object) + offset, sizeof(value));
    return value;
}

void* game() {
    return g_base ? *reinterpret_cast<void**>(g_base + g_build->game.game_instance) : nullptr;
}

void* player(int32_t slot) {
    void* g = game();
    return g && slot >= 0 ? g_get_player(g, slot) : nullptr;
}

} // namespace

void describe_unit(const void* unit, tome_unit& out) {
    const auto& p = g_build->play;
    out = {};
    out.size = sizeof(out);
    out.object = const_cast<void*>(unit);
    out.type_id = field<uint32_t>(unit, p.unit_type_offset);
    const char* name = g_type_name(out.type_id, 0);
    out.name = name ? name : "";
    out.owner = field<int32_t>(unit, p.unit_owner_offset);
    out.flags = classify(out.type_id, field<uint32_t>(unit, p.unit_flags_offset), g_flag_bits);
    void* g = game();
    void* handles = g ? field<void*>(g, p.handles_offset) : nullptr;
    out.handle = find_handle(handles, unit, field<uint32_t>(unit, p.agent_id_offset + 4), g_handle_layout);
}

namespace {

int32_t TOME_CALL api_selection(tome_unit* units, uint32_t unit_size, int32_t max, int32_t* leader) {
    if (leader) *leader = -1;
    if (!g_build || max < 0 || (max > 0 && (!units || unit_size == 0))) return 0;
    if (!frames::on_main_thread()) {
        TOME_ERROR("selection called off the game's main thread (thread %lu); ignored", GetCurrentThreadId());
        return 0;
    }
    std::vector<tome_unit> described;
    int32_t index = -1;
    describe(read_selection(), described, index);
    if (leader) *leader = index;
    // A mod built against an older tome.h passes a smaller struct.
    size_t copy = std::min<size_t>(unit_size, sizeof(tome_unit));
    for (int32_t i = 0; i < max && i < static_cast<int32_t>(described.size()); ++i) {
        auto slot = reinterpret_cast<char*>(units) + static_cast<size_t>(i) * unit_size;
        std::memset(slot, 0, unit_size);
        std::memcpy(slot, &described[i], copy);
        reinterpret_cast<tome_unit*>(slot)->size = static_cast<uint32_t>(copy);
    }
    return static_cast<int32_t>(described.size());
}

} // namespace

void init(uintptr_t game_base, const BuildMappings& build) {
    g_base = game_base;
    g_build = &build;
    const auto& p = build.play;
    g_get_player = reinterpret_cast<GetPlayerFn>(game_base + p.get_player);
    g_type_name = reinterpret_cast<TypeNameFn>(game_base + p.unit_type_name);
    g_string_text = reinterpret_cast<StringTextFn>(game_base + build.jass.string_get_text);
    g_hero_level = reinterpret_cast<HeroLevelFn>(game_base + p.unit_hero_level);
    g_order_target = reinterpret_cast<OrderTargetFn>(game_base + p.order_target);
    g_unit_vtable = game_base + p.unit_vtable;
    g_item_vtable = game_base + p.item_vtable;
    g_flag_bits = {p.unit_flag_dead, p.unit_flag_structure, p.unit_flag_not_hero};
    g_handle_layout = {p.handle_buckets_offset, p.handle_mask_offset, p.handle_entries_offset, p.handle_first};
}

Selection read_selection() {
    Selection out;
    void* g = game();
    if (!g) return out;
    const auto& p = g_build->play;
    void* local = player(field<uint16_t>(g, p.local_player_offset));
    const char* selection = local ? field<const char*>(local, p.player_selection_offset) : nullptr;
    if (!selection) return out;
    out.leader = field<uintptr_t>(selection, p.selection_leader_offset);
    // A unit group: nodes with the next node at +4 and the unit at +8, ended
    // by a value <= 0 (0x290820).
    const char* group = selection + p.selection_units_offset;
    for (auto node = field<int32_t>(group, p.group_first_offset); node > 0 && out.units.size() < kMaxSelected;
         node = field<int32_t>(reinterpret_cast<const void*>(node), 4)) {
        auto unit = field<const char*>(reinterpret_cast<const void*>(node), 8);
        if (!unit) continue;
        SelectedUnit s;
        s.object = reinterpret_cast<uintptr_t>(unit);
        s.id[0] = field<uint32_t>(unit, p.agent_id_offset);
        s.id[1] = field<uint32_t>(unit, p.agent_id_offset + 4);
        out.units.push_back(s);
    }
    return out;
}

void describe(const Selection& selection, std::vector<tome_unit>& out, int32_t& leader) {
    out.assign(selection.units.size(), tome_unit{});
    leader = -1;
    for (size_t i = 0; i < selection.units.size(); ++i) {
        describe_unit(reinterpret_cast<const void*>(selection.units[i].object), out[i]);
        if (selection.units[i].object == selection.leader) leader = static_cast<int32_t>(i);
    }
}

std::string player_name(int32_t slot) {
    void* p = player(slot);
    if (!p) return "";
    const char* text = g_string_text(static_cast<const char*>(p) + g_build->play.player_name_offset);
    return text ? text : "";
}

int32_t local_slot() {
    void* g = game();
    return g ? field<uint16_t>(g, g_build->play.local_player_offset) : -1;
}

bool is_human(int32_t slot) {
    void* p = player(slot);
    return p && field<int32_t>(p, g_build->play.player_controller_offset) == 0 &&
           field<int32_t>(p, g_build->play.player_state_offset) == 1;
}

int32_t player_slot(const void* player) {
    return player ? field<uint8_t>(player, g_build->play.player_slot_offset) : -1;
}

int32_t player_state(const void* player) {
    return player ? field<int32_t>(player, g_build->play.player_state_offset) : 0;
}

int32_t hero_level(const void* unit) {
    if (!unit) return 0;
    const auto& p = g_build->play;
    uint32_t flags = classify(field<uint32_t>(unit, p.unit_type_offset), field<uint32_t>(unit, p.unit_flags_offset),
                              g_flag_bits);
    return (flags & TOME_UNIT_HERO) ? g_hero_level(unit) : 0;
}

uint32_t item_type(const void* item) {
    return item ? field<uint32_t>(item, g_build->play.item_type_offset) : 0;
}

const char* object_name(uint32_t id) {
    const char* name = id ? g_type_name(id, 0) : nullptr;
    return name ? name : "";
}

Order read_order(const void* order, int32_t type) {
    const auto& p = g_build->play;
    Order out;
    out.type = type;
    out.id = field<uint32_t>(order, p.order_id_offset);
    if (type == TOME_ORDER_IMMEDIATE) return out;
    out.x = field<float>(order, p.order_x_offset);
    out.y = field<float>(order, p.order_y_offset);
    if (type != TOME_ORDER_TARGET) return out;
    void* target = g_order_target(order);
    uintptr_t vtable = target ? field<uintptr_t>(target, 0) : 0;
    if (vtable == g_unit_vtable) out.target_unit = target;
    if (vtable == g_item_vtable) out.target_item = target;
    return out;
}

void fill_api(tome_api& api) {
    api.selection = api_selection;
}

} // namespace tome::units
