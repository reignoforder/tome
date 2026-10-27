#pragma once

// FNV-1a: small, stable hashes for change detection and generated names
// (not for anything an attacker controls).

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tome {

struct Fnv1a64 {
    uint64_t value = 14695981039346656037ull;

    void mix(const void* data, size_t len) {
        auto bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < len; ++i) {
            value ^= bytes[i];
            value *= 1099511628211ull;
        }
    }
};

inline uint64_t fnv1a64(std::string_view data) {
    Fnv1a64 hash;
    hash.mix(data.data(), data.size());
    return hash.value;
}

inline uint32_t fnv1a32(std::string_view data) {
    uint32_t hash = 2166136261u;
    for (unsigned char c : data) {
        hash ^= c;
        hash *= 16777619u;
    }
    return hash;
}

} // namespace tome
