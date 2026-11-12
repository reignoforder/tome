// SPDX-License-Identifier: GPL-3.0-or-later
#include "lobbymsg.h"

#include "metadata.h"
#include "signature.h"

#include <charconv>

namespace tome::lobbymsg {
namespace {

// A decimal number that is all of `text`, at most `digits` long.
template <typename T>
bool parse_number(std::string_view text, size_t digits, T& out) {
    if (text.empty() || text.size() > digits) return false;
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc() && end == text.data() + text.size();
}

// Splits off what's before the first `sep`; false if there's none or it's empty.
bool take_until(std::string_view& text, char sep, std::string_view& head) {
    const size_t at = text.find(sep);
    if (at == std::string_view::npos || at == 0) return false;
    head = text.substr(0, at);
    text.remove_prefix(at + 1);
    return true;
}

struct Parsed {
    uint32_t id = 0;
    std::string_view mod_id;
    int part = 0;
    int count = 0;
    std::string data;
};

bool parse(std::string_view text, Parsed& out) {
    text.remove_prefix(kPrefix.size());
    std::string_view version, id, mod, part, count;
    if (!take_until(text, ' ', version) || version != kVersion) return false;
    if (!take_until(text, ' ', id) || !parse_number(id, 10, out.id)) return false;
    if (!take_until(text, ' ', mod) || !is_valid_mod_id(mod)) return false;
    if (!take_until(text, '/', part) || !parse_number(part, 2, out.part)) return false;
    if (!take_until(text, ':', count) || !parse_number(count, 2, out.count)) return false;
    if (text.empty() || text[0] != ' ') return false;
    text.remove_prefix(1);
    std::vector<uint8_t> bytes;
    // An empty message is one part with nothing after the colon.
    if (!text.empty() && !signature::base64_decode(text, bytes)) return false;
    out.mod_id = mod;
    out.data.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return out.count >= 1 && out.count <= kMaxParts && out.part >= 1 && out.part <= out.count &&
           out.data.size() <= kChunk && (out.part == out.count || out.data.size() == kChunk);
}

} // namespace

std::vector<std::string> encode(std::string_view mod_id, uint32_t message_id, std::string_view data) {
    std::vector<std::string> lines;
    if (!is_valid_mod_id(mod_id) || data.size() > kMaxMessage) return lines;
    const int count = data.empty() ? 1 : static_cast<int>((data.size() + kChunk - 1) / kChunk);
    const std::string head = std::string(kPrefix) + std::string(kVersion) + " " + std::to_string(message_id) + " " +
                             std::string(mod_id) + " ";
    for (int part = 1; part <= count; ++part) {
        std::string_view chunk = data.substr(static_cast<size_t>(part - 1) * kChunk, kChunk);
        lines.push_back(head + std::to_string(part) + "/" + std::to_string(count) + ": " +
                        signature::base64_encode(reinterpret_cast<const uint8_t*>(chunk.data()), chunk.size()));
    }
    return lines;
}

bool is_message_line(std::string_view text) {
    return text.substr(0, kPrefix.size()) == kPrefix;
}

Reassembler::Result Reassembler::add(int sender, std::string_view text, Message& out) {
    if (!is_message_line(text)) return Result::NotOurs;
    Parsed p;
    if (text.size() > kMaxLine || !parse(text, p)) {
        pending_.erase(sender);
        return Result::Bad;
    }
    if (p.part == 1) {
        Pending fresh;
        fresh.mod_id.assign(p.mod_id);
        fresh.id = p.id;
        fresh.count = p.count;
        pending_[sender] = std::move(fresh);
    } else {
        auto it = pending_.find(sender);
        if (it == pending_.end() || it->second.id != p.id || it->second.mod_id != p.mod_id ||
            it->second.count != p.count || it->second.next != p.part) {
            pending_.erase(sender);
            return Result::Bad;
        }
    }
    Pending& pending = pending_[sender];
    pending.data += p.data;
    pending.next = p.part + 1;
    if (pending.data.size() > kMaxMessage) {  // eleven whole parts would be 1056 bytes
        pending_.erase(sender);
        return Result::Bad;
    }
    if (pending.next <= pending.count) return Result::Partial;
    out.mod_id = std::move(pending.mod_id);
    out.data = std::move(pending.data);
    pending_.erase(sender);
    return Result::Complete;
}

} // namespace tome::lobbymsg
