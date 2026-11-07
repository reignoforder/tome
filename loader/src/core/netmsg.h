// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Messages between players for mods (tome.h, net_send and on_net_message;
// docs/architecture.md, "Messages between players"). This is the wire
// format; loader/src/net_messages.cpp sends and receives it.
//
// A message rides in the game's SyncStoredInteger actions, as the
// handshake's mod sets do: they reach every player of a game (the sender
// too) in the same game step, in order, and the replay records them. The
// sender byte is stamped by the host's router. Each action carries one part:
//   game cache  "TomeNet" (kCacheFile): marks Tome's message actions
//   mission     "1 <message id> <mod id>": the protocol version, a number the
//               sender gives each message, and the mod it's for
//   key         up to kChunk bytes of the message, in base64 (no NUL, which
//               the game's text fields can't hold)
//   value       part | count << 8 (part 0 .. count - 1)
// A message is at most kMaxMessage bytes, so at most kMaxParts parts.
//
// What arrives comes from other players: Reassembler keeps at most one
// message in progress per sender (a player's parts arrive in a row, since
// its game sends them all at once), drops anything malformed, and never
// holds more than kMaxMessage bytes per sender.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace tome::netmsg {

constexpr std::string_view kCacheFile = "TomeNet";
constexpr std::string_view kVersion = "1";
constexpr size_t kMaxMessage = 1024;
// 189 bytes are 252 base64 characters: within the key's 0x100 bytes.
constexpr size_t kChunk = 189;
constexpr int kMaxParts = static_cast<int>((kMaxMessage + kChunk - 1) / kChunk);

struct Part {
    std::string mission;
    std::string key;
    int32_t value = 0;
};

// The parts of a message for mod `mod_id` (a valid mod id; data at most
// kMaxMessage bytes, possibly empty). Empty if either doesn't hold.
std::vector<Part> encode(std::string_view mod_id, uint32_t message_id, std::string_view data);

struct Message {
    std::string mod_id;
    std::string data;
};

class Reassembler {
public:
    enum class Result {
        Partial,   // a part of a message in progress
        Complete,  // `out` has a whole message
        Bad,       // malformed or out of order: dropped (and the sender's message in progress with it)
    };
    // A part from player `sender` (its mission, key and value).
    Result add(int sender, std::string_view mission, std::string_view key, int32_t value, Message& out);
    // Forgets every message in progress (a new game).
    void reset() { pending_.clear(); }
    size_t pending() const { return pending_.size(); }

private:
    struct Pending {
        std::string mod_id;
        uint32_t id = 0;
        int count = 0;
        int next = 0;
        std::string data;
    };
    std::map<int, Pending> pending_;
};

// The mission of a part: false unless it's "<kVersion> <id> <mod id>" with a
// valid mod id.
bool parse_mission(std::string_view mission, uint32_t& id, std::string& mod_id);

} // namespace tome::netmsg
