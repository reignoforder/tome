#include "units.h"

#include <cstring>

namespace tome::units {
namespace {

uint32_t read_u32(uintptr_t address) {
    uint32_t value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

} // namespace

uint32_t classify(uint32_t type_id, uint32_t raw_flags, const FlagBits& bits) {
    uint32_t flags = 0;
    if (raw_flags & bits.structure) flags |= TOME_UNIT_STRUCTURE;
    char first = static_cast<char>(type_id >> 24);
    if (first >= 'A' && first <= 'Z' && !(raw_flags & bits.not_hero)) flags |= TOME_UNIT_HERO;
    if (raw_flags & bits.dead) flags |= TOME_UNIT_DEAD;
    return flags;
}

void type_code(uint32_t type_id, char out[5]) {
    for (int i = 0; i < 4; ++i) out[i] = static_cast<char>(type_id >> (24 - 8 * i));
    out[4] = '\0';
}

int32_t chat_recipient(int32_t raw) {
    switch (raw) {
    case 0: return TOME_CHAT_ALL;
    case 1: return TOME_CHAT_ALLIES;
    case 2: return TOME_CHAT_OBSERVERS;
    default: return TOME_CHAT_PRIVATE;
    }
}

int32_t pause_player(uint32_t player, uint32_t by_trigger) {
    if (by_trigger || player == 0xFF) return -1;
    return static_cast<int32_t>(player);
}

bool Results::first(int32_t event, int32_t player) {
    std::pair<int32_t, int32_t> key(event, player);
    for (const auto& seen : seen_) {
        if (seen == key) return false;
    }
    seen_.push_back(key);
    return true;
}

// The first half of the game's own lookup (0x282E30), without the part that
// makes a new handle.
uint32_t find_handle(const void* manager, const void* object, uint32_t key, const HandleLayout& layout) {
    if (!manager || !object) return 0;
    auto base = reinterpret_cast<uintptr_t>(manager);
    uint32_t mask = read_u32(base + layout.mask);
    uintptr_t buckets = read_u32(base + layout.buckets);
    if (mask == 0xFFFFFFFF || !buckets) return 0;
    uintptr_t bucket = buckets + (mask & key) * 12;
    uint32_t link = read_u32(bucket);
    // The game ends the list at a value <= 0 (a tagged terminator).
    for (int32_t node = static_cast<int32_t>(read_u32(bucket + 8)); node > 0;
         node = static_cast<int32_t>(read_u32(link + static_cast<uint32_t>(node) + 4))) {
        if (read_u32(static_cast<uint32_t>(node)) != key) continue;
        uint32_t id = read_u32(static_cast<uint32_t>(node) + 0x18);
        uintptr_t entries = read_u32(base + layout.entries);
        if (id < layout.first || !entries) return 0;
        // The handle table must still point back at this object.
        bool same = read_u32(entries + (id - layout.first) * 12 + 4) == reinterpret_cast<uintptr_t>(object);
        return same ? id : 0;
    }
    return 0;
}

} // namespace tome::units
