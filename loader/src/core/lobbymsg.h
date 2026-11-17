// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Mods' messages in a game's lobby (tome.h, lobby_send and
// on_lobby_message; docs/architecture.md, "Lobby messages"). This is the
// wire format; loader/src/lobby_messages.cpp sends and receives it.
//
// A message rides in lobby chat lines, as the handshake's mod sets do
// (core/handshake.h): sent to one player at a time (NetSendLobbyEvent), the
// sender stamped by the host's router, hidden from the chat by Tome on the
// other side. Each line carries one part:
//   "[Tome] Msg 1 <message id> <mod id> <part>/<count>: <base64>"
// the protocol version, a number the sender gives each message, the mod
// it's for, the part (1 .. count) and up to kChunk bytes of the message in
// base64 (no NUL, which the chat can't hold). A message is at most
// kMaxMessage bytes, so at most kMaxParts parts; each line fits kMaxLine.
//
// What arrives comes from other players: Reassembler keeps at most one
// message in progress per sender (a client's parts arrive in a row, since
// it sends them all at once), drops anything malformed, and never holds
// more than kMaxMessage bytes per sender.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace tome::lobbymsg {

constexpr std::string_view kPrefix = "[Tome] Msg ";
constexpr std::string_view kVersion = "1";
constexpr size_t kMaxMessage = 1024;
// 96 bytes are 128 base64 characters; with the longest header (a 64-byte
// mod id, a 10-digit message id) a line stays under kMaxLine.
constexpr size_t kChunk = 96;
constexpr int kMaxParts = static_cast<int>((kMaxMessage + kChunk - 1) / kChunk);
constexpr size_t kMaxLine = 250;

// The lines of a message for mod `mod_id` (a valid mod id; data at most
// kMaxMessage bytes, possibly empty). Empty if either doesn't hold.
std::vector<std::string> encode(std::string_view mod_id, uint32_t message_id, std::string_view data);

// Whether a chat line is one of these by its prefix (decoded or not).
bool is_message_line(std::string_view text);

struct Message {
    std::string mod_id;
    std::string data;
};

class Reassembler {
public:
    enum class Result {
        NotOurs,   // not a message line: someone's chat (shown as usual)
        Partial,   // a part of a message in progress
        Complete,  // `out` has a whole message
        Bad,       // malformed or out of order: dropped (and the sender's message in progress with it)
    };
    // A chat line from player `sender`.
    Result add(int sender, std::string_view text, Message& out);
    // Forgets every message in progress (a new lobby), or one sender's (it left).
    void reset() { pending_.clear(); }
    void forget(int sender) { pending_.erase(sender); }
    size_t pending() const { return pending_.size(); }

private:
    struct Pending {
        std::string mod_id;
        uint32_t id = 0;
        int count = 0;
        int next = 1;
        std::string data;
    };
    std::map<int, Pending> pending_;
};

} // namespace tome::lobbymsg
