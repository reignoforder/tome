// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Mods' messages in a game's lobby and their say in its start (tome.h:
// lobby_send, on_lobby_message, lobby_players, on_lobby_start, lobby_start;
// core/lobbymsg.h has the wire format; docs/architecture.md, "Lobby
// messages"). The lines travel through the handshake (handshake.cpp), which
// already reads the lobby, sends its lines and hooks its chat and Start.

#include "tome/tome.h"

#include <string>
#include <vector>

namespace tome::lobby_messages {

void fill_api(tome_api& api);

// From handshake.cpp, on the main thread: a lobby chat line from player
// `from` that isn't the handshake's. True if it's a mod message line
// (decoded or not), which isn't shown.
bool receive(int from, bool from_host, const std::string& text);

// From handshake.cpp, the host pressed Start and the handshake allows it:
// every mod's on_lobby_start callback runs. False if one refuses; `reasons`
// gets what they said (for the lobby's chat).
bool start_allowed(std::vector<std::string>& reasons);

// The lobby closed (or another opened): messages in progress are dropped.
void on_lobby_closed();

} // namespace tome::lobby_messages
