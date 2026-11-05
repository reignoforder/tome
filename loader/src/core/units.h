// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Units, the selection, chat and pause as mods see them (tome.h, API version
// 8): the game's raw values turned into tome_unit flags, chat recipients and
// pausing players, the comparison that tells a changed selection, and the
// lookup of a unit's existing JASS handle id. The memory reads are in
// loader/src/units.cpp.

#include <tome/tome.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace tome::units {

// UnitFlag.* from the mapping.
struct FlagBits {
    uint32_t dead = 0;
    uint32_t structure = 0;
    uint32_t not_hero = 0;
};

// TOME_UNIT_* flags from a unit's type id and CUnit.flags. A hero is what
// IsUnitType(u, UNIT_TYPE_HERO) says: a type id starting with A-Z, and not
// marked UnitFlag.not_hero.
uint32_t classify(uint32_t type_id, uint32_t raw_flags, const FlagBits& bits);

// "hpea" for 'hpea'; NUL-terminated.
void type_code(uint32_t type_id, char out[5]);

// CGameUI::DisplayChatMessage's recipient (0 all, 1 allies, 2 observers or
// referees, anything else private) as a tome_chat_recipient.
int32_t chat_recipient(int32_t raw);

// The player CGameUI::SetPaused reports: its player slot, or -1 for none
// (0xFF) and for a trigger's PauseGame (a5 = 1), which passes the local
// player.
int32_t pause_player(uint32_t player, uint32_t by_trigger);

// CPlayerWar3::OnArrowKey's event (the JASS event id + 0x80200, from
// EVENT_PLAYER_ARROW_LEFT_DOWN to EVENT_PLAYER_ARROW_UP_UP) as a
// tome_arrow_key and whether it was pressed; false for anything else.
bool arrow_key(uint32_t raw_event, int32_t& key, int32_t& pressed);

// The player states TOME_EVENT_PLAYER_RESOURCE watches (tome_resource, the
// JASS playerstate values), and what a stored value is divided by: the game
// keeps gold and lumber (and what was gathered of them) times 10.
inline constexpr int32_t kWatchedResources[] = {TOME_RESOURCE_GOLD, TOME_RESOURCE_LUMBER, TOME_RESOURCE_FOOD_CAP,
                                                 TOME_RESOURCE_FOOD_USED};
int32_t player_state_scale(int32_t state);

// The last value seen of each watched resource per player slot (16 in
// 1.27b: 12 players and 4 neutral ones).
class ResourceWatch {
public:
    static constexpr int32_t kSlots = 16;

    // Records `value`; true (with the value before it in `previous`) if one
    // was recorded since reset() and it differs.
    bool update(int32_t slot, int32_t resource, int32_t value, int32_t& previous);
    void reset();

private:
    static constexpr int32_t kResources = sizeof(kWatchedResources) / sizeof(kWatchedResources[0]);
    int32_t values_[kSlots][kResources] = {};
    bool known_[kSlots][kResources] = {};
};

// The time of day (hours, 0 to 24) going from `before` to `now`: 1 if day
// began (6:00 passed, or a jump into [6, 18) from night), -1 if night began
// (18:00), else 0. A negative `before` means no earlier reading: 0.
int32_t day_change(double before, double now);

// TOME_EVENT_PLAYER_VICTORY, DEFEAT and LEFT once per player and game: the
// game's functions for them have several callers each.
class Results {
public:
    // True the first time `event` is reported for `player` since reset().
    bool first(int32_t event, int32_t player);
    void reset() { seen_.clear(); }

private:
    std::vector<std::pair<int32_t, int32_t>> seen_;
};

// One selected unit, by its object and agent id: a new unit at a freed
// unit's address still counts as a change.
struct SelectedUnit {
    uintptr_t object = 0;
    uint32_t id[2] = {0, 0};

    bool operator==(const SelectedUnit&) const = default;
};

struct Selection {
    std::vector<SelectedUnit> units;  // in the group's order
    uintptr_t leader = 0;             // the active unit's object, or 0

    bool operator==(const Selection&) const = default;
};

// Where the handle manager keeps its agent-to-handle hash (HandleManager.*,
// HandleId.first).
struct HandleLayout {
    uint32_t buckets = 0;  // offset of the bucket array pointer
    uint32_t mask = 0;     // offset of the bucket mask
    uint32_t entries = 0;  // offset of the handle table pointer
    uint32_t first = 0;    // the first handle id
};

// The handle id the map script already has for `object` (whose agent id's
// second word is `key`), or 0 if there's none. Only reads: the game's own
// lookup (0x282E30) makes a handle when there's none, and a handle made on
// one client only would shift the ids of every later one there.
uint32_t find_handle(const void* manager, const void* object, uint32_t key, const HandleLayout& layout);

} // namespace tome::units
