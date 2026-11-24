// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Desync checks (tome.h, on_checksum; docs/architecture.md, "Desync
// checks"): the wire format and the comparison, apart from the game
// (loader/src/desync.cpp), so they can be unit-tested and fuzzed.
//
// Every kInterval seconds of game time, in a multiplayer game, each
// "required" mod with a checksum callback gives a 64-bit hash of its game
// state, in the same game step on every player's computer (a round). Each
// player's hashes travel as SyncStoredInteger actions, like the handshake's
// sets: they reach every player (the sender too) in order, and the replay
// records them. One action per mod and round:
//   game cache  "TomeSum" (kCacheFile)
//   mission     "1 <round> <mod id>": the protocol version, the round (1, 2, ...)
//   key         the hash, 16 lowercase hex digits
//   value       0
// A player's computer compares the others' hashes with its own for the same
// round and mod. Lockstep games advance together, so its own is normally
// there first; another player's that arrives before it waits (a few rounds
// at most). What arrives comes from other players: malformed actions are
// dropped, and what waits is bounded.

#include "game_clock.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace tome::desync {

constexpr std::string_view kCacheFile = "TomeSum";
constexpr std::string_view kVersion = "1";
// Game time between rounds (3 s), counted in whole microseconds so every
// computer ends a round in the same step (core/game_clock.h).
constexpr game_clock::Micros kInterval = 3 * game_clock::kPerSecond;
// Rounds kept on either side of the latest local one; others' hashes
// outside that window are dropped.
constexpr uint32_t kWindow = 8;
// At most this many of the others' hashes wait for the local one.
constexpr size_t kMaxWaiting = 2048;

struct Part {
    std::string mission;
    std::string key;
};

// The action for one mod's hash in `round` (a valid mod id, round >= 1);
// an empty mission if either doesn't hold.
Part encode(uint32_t round, std::string_view mod_id, uint64_t hash);
// False unless mission and key are what encode makes.
bool parse(std::string_view mission, std::string_view key, uint32_t& round, std::string& mod_id, uint64_t& hash);

// The round a game step ends in: rounds end when game time passes a
// multiple of kInterval. Round n (n >= 1) ends in the first step at or past
// n * kInterval. 0 if `now` hasn't reached the next one since `before` (the
// previous step's time).
uint32_t round_ending(game_clock::Micros before, game_clock::Micros now);

// One of another player's hashes against this computer's.
struct Comparison {
    int player = -1;     // the other player's slot
    std::string mod_id;
    uint32_t round = 0;
    uint64_t local = 0;  // this computer's hash
    uint64_t remote = 0; // theirs
    bool same() const { return local == remote; }
};

class Checker {
public:
    // A new game.
    void reset();
    // This computer's hash for `mod_id` in `round`. Others' that waited for
    // it are compared now, each comparison added to `out`.
    void local(uint32_t round, const std::string& mod_id, uint64_t hash, std::vector<Comparison>& out);
    // Another player's. Compared at once (added to `out`) if the local one
    // is there; else it waits for it (or is dropped outside the window, or
    // when too many wait).
    void remote(int player, uint32_t round, const std::string& mod_id, uint64_t hash, std::vector<Comparison>& out);
    // Whether this is the first comparison of `mod_id` with `player` this
    // game that came out `same` (or not); marks it. The first match and the
    // first difference of each pair are worth telling.
    bool first_report(int player, const std::string& mod_id, bool same);

    size_t waiting() const { return waiting_.size(); }
    size_t kept() const { return local_.size(); }

private:
    void forget_old();
    using Key = std::pair<uint32_t, std::string>;  // round, mod id
    std::map<Key, uint64_t> local_;
    std::map<std::tuple<uint32_t, std::string, int>, uint64_t> waiting_;  // round, mod id, player
    std::set<std::tuple<int, std::string, bool>> reported_;
    uint32_t latest_ = 0;  // the latest local round
};

// "0:42", "1:02:03": game time for the player.
std::string format_game_time(double seconds);

} // namespace tome::desync
