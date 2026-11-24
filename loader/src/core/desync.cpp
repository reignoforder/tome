// SPDX-License-Identifier: GPL-3.0-or-later
#include "desync.h"

#include "metadata.h"

#include <charconv>
#include <cmath>
#include <cstdio>

namespace tome::desync {

Part encode(uint32_t round, std::string_view mod_id, uint64_t hash) {
    Part part;
    if (round == 0 || !is_valid_mod_id(mod_id)) return part;
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));
    part.mission = std::string(kVersion) + " " + std::to_string(round) + " " + std::string(mod_id);
    part.key = hex;
    return part;
}

bool parse(std::string_view mission, std::string_view key, uint32_t& round, std::string& mod_id, uint64_t& hash) {
    if (mission.size() <= kVersion.size() || mission.substr(0, kVersion.size()) != kVersion ||
        mission[kVersion.size()] != ' ') {
        return false;
    }
    mission.remove_prefix(kVersion.size() + 1);
    const size_t space = mission.find(' ');
    if (space == std::string_view::npos || space == 0) return false;
    std::string_view number = mission.substr(0, space);
    // Digits only, no leading zero: one spelling per round.
    if (number[0] == '0') return false;
    auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), round);
    if (ec != std::errc() || end != number.data() + number.size() || round == 0) return false;
    std::string_view mod = mission.substr(space + 1);
    if (!is_valid_mod_id(mod)) return false;
    if (key.size() != 16) return false;
    for (char c : key) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    auto [hex_end, hex_ec] = std::from_chars(key.data(), key.data() + key.size(), hash, 16);
    if (hex_ec != std::errc() || hex_end != key.data() + key.size()) return false;
    mod_id.assign(mod);
    return true;
}

uint32_t round_ending(game_clock::Micros before, game_clock::Micros now) {
    if (now <= before || now < kInterval) return 0;
    const game_clock::Micros next = now / kInterval;
    if (next > 4000000000) return 0;  // past any game's length
    // Ended in this step if `before` was still short of it.
    if (before >= next * kInterval) return 0;
    return static_cast<uint32_t>(next);
}

void Checker::reset() {
    local_.clear();
    waiting_.clear();
    reported_.clear();
    latest_ = 0;
}

void Checker::forget_old() {
    const uint32_t oldest = latest_ > kWindow ? latest_ - kWindow : 0;
    std::erase_if(local_, [&](const auto& e) { return e.first.first < oldest; });
    std::erase_if(waiting_, [&](const auto& e) { return std::get<0>(e.first) < oldest; });
}

void Checker::local(uint32_t round, const std::string& mod_id, uint64_t hash, std::vector<Comparison>& out) {
    local_[{round, mod_id}] = hash;
    if (round > latest_) {
        latest_ = round;
        forget_old();
    }
    for (auto it = waiting_.lower_bound({round, mod_id, -1000}); it != waiting_.end();) {
        const auto& [r, mod, player] = it->first;
        if (r != round || mod != mod_id) break;
        out.push_back({player, mod_id, round, hash, it->second});
        it = waiting_.erase(it);
    }
}

void Checker::remote(int player, uint32_t round, const std::string& mod_id, uint64_t hash,
                     std::vector<Comparison>& out) {
    auto mine = local_.find({round, mod_id});
    if (mine != local_.end()) {
        out.push_back({player, mod_id, round, mine->second, hash});
        return;
    }
    // Rounds this computer has passed without a hash for the mod: it
    // doesn't run it, or it wasn't there yet. Nothing to compare.
    if (round <= latest_ && latest_ > 0) return;
    if (round > latest_ + kWindow || waiting_.size() >= kMaxWaiting) return;
    waiting_[{round, mod_id, player}] = hash;
}

bool Checker::first_report(int player, const std::string& mod_id, bool same) {
    return reported_.insert({player, mod_id, same}).second;
}

std::string format_game_time(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0) seconds = 0;
    const unsigned long long total = static_cast<unsigned long long>(seconds);
    char buf[32];
    if (total >= 3600) {
        std::snprintf(buf, sizeof(buf), "%llu:%02llu:%02llu", total / 3600, total / 60 % 60, total % 60);
    } else {
        std::snprintf(buf, sizeof(buf), "%llu:%02llu", total / 60, total % 60);
    }
    return buf;
}

} // namespace tome::desync
