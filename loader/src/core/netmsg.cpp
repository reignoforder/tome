// SPDX-License-Identifier: GPL-3.0-or-later
#include "netmsg.h"

#include "metadata.h"
#include "signature.h"

#include <charconv>

namespace tome::netmsg {

std::vector<Part> encode(std::string_view mod_id, uint32_t message_id, std::string_view data) {
    std::vector<Part> parts;
    if (!is_valid_mod_id(mod_id) || data.size() > kMaxMessage) return parts;
    const int count = data.empty() ? 1 : static_cast<int>((data.size() + kChunk - 1) / kChunk);
    const std::string mission = std::string(kVersion) + " " + std::to_string(message_id) + " " + std::string(mod_id);
    for (int part = 0; part < count; ++part) {
        std::string_view chunk = data.substr(static_cast<size_t>(part) * kChunk, kChunk);
        parts.push_back({mission, signature::base64_encode(reinterpret_cast<const uint8_t*>(chunk.data()), chunk.size()),
                         part | (count << 8)});
    }
    return parts;
}

bool parse_mission(std::string_view mission, uint32_t& id, std::string& mod_id) {
    if (mission.substr(0, kVersion.size()) != kVersion || mission.size() <= kVersion.size() ||
        mission[kVersion.size()] != ' ') {
        return false;
    }
    mission.remove_prefix(kVersion.size() + 1);
    const size_t space = mission.find(' ');
    if (space == std::string_view::npos || space == 0) return false;
    std::string_view number = mission.substr(0, space);
    auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), id);
    if (ec != std::errc() || end != number.data() + number.size()) return false;
    std::string_view mod = mission.substr(space + 1);
    if (!is_valid_mod_id(mod)) return false;
    mod_id.assign(mod);
    return true;
}

Reassembler::Result Reassembler::add(int sender, std::string_view mission, std::string_view key, int32_t value,
                                     Message& out) {
    uint32_t id = 0;
    std::string mod_id;
    const int part = value & 0xFF;
    const int count = (value >> 8) & 0xFF;
    std::vector<uint8_t> bytes;
    const bool well_formed = (value >> 16) == 0 && count >= 1 && count <= kMaxParts && part < count &&
                             parse_mission(mission, id, mod_id) && signature::base64_decode(key, bytes) &&
                             bytes.size() <= kChunk && (part == count - 1 || bytes.size() == kChunk);
    if (!well_formed) {
        pending_.erase(sender);
        return Result::Bad;
    }
    if (part == 0) {
        Pending fresh;
        fresh.mod_id = std::move(mod_id);
        fresh.id = id;
        fresh.count = count;
        pending_[sender] = std::move(fresh);
    } else {
        auto it = pending_.find(sender);
        if (it == pending_.end() || it->second.id != id || it->second.mod_id != mod_id ||
            it->second.count != count || it->second.next != part) {
            pending_.erase(sender);
            return Result::Bad;
        }
    }
    Pending& p = pending_[sender];
    p.data.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    p.next = part + 1;
    if (p.data.size() > kMaxMessage) {  // six whole parts would be 1134 bytes
        pending_.erase(sender);
        return Result::Bad;
    }
    if (p.next < p.count) return Result::Partial;
    out.mod_id = std::move(p.mod_id);
    out.data = std::move(p.data);
    pending_.erase(sender);
    return Result::Complete;
}

} // namespace tome::netmsg
