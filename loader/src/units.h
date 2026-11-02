// SPDX-License-Identifier: GPL-3.0-or-later
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

// A widget (an order's or a spell's target) sorted by its vtable; each is
// null unless the widget is of that kind.
struct Widget {
    void* unit = nullptr;
    void* item = nullptr;
    void* destructable = nullptr;
};
Widget classify_widget(void* widget);

// What an order (a COrder) says: its id, and for point and target orders
// the point and the target.
struct Order {
    uint32_t id = 0;
    int32_t type = TOME_ORDER_IMMEDIATE;
    double x = 0, y = 0;
    Widget target;
};
Order read_order(const void* order, int32_t type);

// What a spell event's ability (a CAbility) says: its type, its target and
// target point (through the spell ability's virtual functions, as
// GetSpellTargetUnit and GetSpellTargetX do).
struct Spell {
    uint32_t ability_id = 0;
    double x = 0, y = 0;
    Widget target;
};
Spell read_spell(void* ability);

// A destructable's type id.
uint32_t destructable_type(const void* destructable);

// The JASS handle id the map script has for an agent (unit, item,
// destructable, dialog...), or 0; never makes one.
uint32_t handle_of(const void* agent);

// The unit that has an ability (CAbility.owner), or null.
void* ability_owner(const void* ability);

// A hero's experience, from its hero ability (CAbilityHero.xp).
int32_t hero_xp(const void* hero_ability);

// A player's gold, lumber or food (tome_resource) as GetPlayerState gives
// it; false if there's no game or no such player.
bool player_resource(int32_t slot, int32_t resource, int32_t& value);

// The time of day in hours (GetFloatGameState), or -1 outside a game.
double time_of_day();

// Fills the version 8 part of the API table.
void fill_api(tome_api& api);

} // namespace tome::units
