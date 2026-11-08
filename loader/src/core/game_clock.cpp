// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_clock.h"

#include <cstring>

namespace tome::game_clock {
namespace {

// mantissa * 2^exponent seconds in microseconds, rounded (halves up), for
// a mantissa below 2^53. All integer: the product m * 10^6 needs up to 73
// bits, kept as hi * 2^32 + lo (lo below 2^32).
Micros scale(uint64_t mantissa, int exponent) {
    if (mantissa == 0) return 0;
    const uint64_t lo_part = (mantissa & 0xFFFFFFFFu) * uint64_t(kPerSecond);  // below 2^52
    uint64_t hi = (mantissa >> 32) * uint64_t(kPerSecond) + (lo_part >> 32);    // below 2^43
    uint64_t lo = lo_part & 0xFFFFFFFFu;
    if (exponent >= 0) {
        // At least 2^52 s when the mantissa has its 53 bits; small mantissas
        // (floats) may still fit.
        if (exponent >= 30 || (hi >> (30 - exponent)) != 0) return kMax;
        const uint64_t value = ((hi << 32) | lo) << exponent;
        return value >= uint64_t(kMax) ? kMax : Micros(value);
    }
    const int shift = -exponent;
    if (shift > 76) return 0;  // under half a microsecond
    // Add half of the last place kept, then drop the bits below it.
    if (shift - 1 < 32) {
        lo += uint64_t(1) << (shift - 1);
        hi += lo >> 32;
        lo &= 0xFFFFFFFFu;
    } else {
        hi += uint64_t(1) << (shift - 1 - 32);
    }
    uint64_t value;
    if (shift >= 32) {
        value = hi >> (shift - 32);
    } else {
        if ((hi >> (30 + shift)) != 0) return kMax;  // 2^62 us or more
        value = (hi << (32 - shift)) | (lo >> shift);
    }
    return value >= uint64_t(kMax) ? kMax : Micros(value);
}

} // namespace

Micros from_seconds(double s) {
    uint64_t bits;
    std::memcpy(&bits, &s, sizeof(bits));
    if (bits >> 63) return 0;  // negative (or -0)
    const int biased = int(bits >> 52) & 0x7FF;
    const uint64_t fraction = bits & ((uint64_t(1) << 52) - 1);
    if (biased == 0x7FF) return fraction ? 0 : kMax;  // NaN, infinity
    if (biased == 0) return scale(fraction, -1074);    // subnormal
    return scale(fraction | (uint64_t(1) << 52), biased - 1075);
}

Micros from_seconds(float s) {
    uint32_t bits;
    std::memcpy(&bits, &s, sizeof(bits));
    if (bits >> 31) return 0;
    const int biased = int(bits >> 23) & 0xFF;
    const uint32_t fraction = bits & ((uint32_t(1) << 23) - 1);
    if (biased == 0xFF) return fraction ? 0 : kMax;
    if (biased == 0) return scale(fraction, -149);
    return scale(fraction | (uint32_t(1) << 23), biased - 150);
}

} // namespace tome::game_clock
