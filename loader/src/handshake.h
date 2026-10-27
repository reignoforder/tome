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

namespace tome::handshake {

// Installs the hooks. Called from DllMain after events::install. `report`
// says what happened, for the log once it's open.
bool install(uintptr_t game_base, const BuildMappings& build, std::string& report);

// From events.cpp: a game started (TOME_EVENT_GAME_START), advanced to
// `game_time` seconds, or ended.
void on_game_start();
void on_game_tick(double game_time);
void on_game_end();

} // namespace tome::handshake
