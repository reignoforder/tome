// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Desync checks for mods (tome.h, API version 19: on_checksum). The wire
// format and the comparison are core/desync.h; this calls the mods'
// checksum callbacks at the end of each round (a game step, from
// events.cpp), sends their hashes and tells the player what differs.
// Everything runs on the game's main thread, except registration.

#include "core/game_clock.h"

#include <tome/tome.h>

#include <string>

namespace tome::desync_api {

void fill_api(tome_api& api);

// From events.cpp: a game started; it took a step from `before` to `now`
// (game time, the same on every computer; after that step's GAME_TICK
// handlers).
void on_game_start();
void on_game_step(game_clock::Micros before, game_clock::Micros now);

// From handshake.cpp: a SyncStoredInteger action of Tome's checksum cache
// from player `sender`.
void receive(int sender, const std::string& mission, const std::string& key, bool replay);

} // namespace tome::desync_api
