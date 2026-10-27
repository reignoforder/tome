#pragma once

// Reading players, units and the local selection out of the running game
// (tome.h, API version 8). Main thread, in a game; core/units.h has the
// portable parts. events.cpp turns changes into TOME_EVENT_SELECTION.

#include "core/units.h"
#include "mappings.h"

#include <tome/tome.h>

#include <string>
#include <vector>

namespace tome::units {

// Called by events::install.
void init(uintptr_t game_base, const BuildMappings& build);

// The local player's selection right now; empty outside a game.
Selection read_selection();

// Describes the units of a selection read this frame, with the leader's
// index in `out` (-1 for none).
void describe(const Selection& selection, std::vector<tome_unit>& out, int32_t& leader);

// A player's name, "" for an empty slot or no game.
std::string player_name(int32_t slot);

// The local player's slot, -1 outside a game.
int32_t local_slot();

// Whether a slot holds a person (controller "user") who is in the game.
bool is_human(int32_t slot);

// A unit as mods see it.
void describe_unit(const void* unit, tome_unit& out);

// A CPlayerWar3's slot, and its CPlayerWar3.game_state (1 while in the game).
int32_t player_slot(const void* player);
int32_t player_state(const void* player);

// A hero's level (0 for other units).
int32_t hero_level(const void* unit);

// An item's type id.
uint32_t item_type(const void* item);

// The name of a unit, item or research type ("" if unknown).
const char* object_name(uint32_t id);

// What an order (a COrder) says: its id, and for point and target orders
// the point and the target, if it's a unit or an item.
struct Order {
    uint32_t id = 0;
    int32_t type = TOME_ORDER_IMMEDIATE;
    double x = 0, y = 0;
    void* target_unit = nullptr;
    void* target_item = nullptr;
};
Order read_order(const void* order, int32_t type);

// Fills the version 8 part of the API table.
void fill_api(tome_api& api);

} // namespace tome::units
