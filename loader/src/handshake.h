// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The multiplayer handshake for "sync": "required" mods (core/handshake.h has
// the protocol and the decisions; docs/architecture.md, "Multiplayer
// handshake"). This side reads the game and acts:
//   CGameChatroom::OnChatReceived (before)  Tome's lobby lines: handled, not shown
//   CGameChatroom::StartGame (before)       the host refuses while the sets differ
//   PresentFrame (before)                   twice a second in a lobby: who's there,
//                                           what to send (NetSendLobbyEvent)
//   OnNetSyncStoredInteger (before)         the sets players send at the start of
//                                           a game, also in replays
// Everything runs on the game's main thread.

#include "mappings.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace tome::handshake {

// Installs the hooks. Called from DllMain after events::install. `report`
// says what happened, for the log once it's open.
bool install(uintptr_t game_base, const BuildMappings& build, std::string& report);

// From events.cpp: a game started (TOME_EVENT_GAME_START), advanced to
// `game_time` seconds, or ended.
void on_game_start();
void on_game_tick(double game_time);
void on_game_end();

// The current game's host: its player slot (0-based, Player(n) in JASS), live
// and in replays (in single player, the local player's). -1 outside a game,
// or where the build doesn't map NetGetPlayerNumber. Main thread.
int host_slot();

// Whether a game runs (from its start to its end), and whether it's a replay.
bool in_game();
bool in_replay();
// Whether the current game took its first step: the players are in place,
// the game lock is final and actions may be sent. Main thread.
bool game_stepped();

// Sends one SyncStoredInteger action, which reaches every player (this one
// too) and the replay. False, with nothing sent, outside a game or in a
// replay. Main thread.
bool send_sync_action(std::string_view file, std::string_view mission, std::string_view key, int32_t value);

} // namespace tome::handshake
