// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Messages between players for mods (tome.h: net_send, on_net_message;
// core/netmsg.h has the wire format; docs/architecture.md, "Messages
// between players"). They travel as SyncStoredInteger actions, sent and
// received through the handshake (handshake.cpp), which already hooks them.

#include "tome/tome.h"

#include <cstdint>
#include <string>

namespace tome::net_messages {

void fill_api(tome_api& api);

// From handshake.cpp, on the main thread: a SyncStoredInteger action of
// Tome's message cache from player `sender` (its mission, key and value).
void receive(int sender, const std::string& mission, const std::string& key, int32_t value, bool replay);

// A game starts: messages in progress from the last one are dropped.
void on_game_start();

} // namespace tome::net_messages
