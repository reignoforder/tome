// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/game_clock.h"
#include "test.h"

#include <cmath>
#include <limits>

using namespace tome::game_clock;

TEST(game_clock_from_seconds) {
    CHECK_EQ(from_seconds(0.0), 0);
    CHECK_EQ(from_seconds(-0.0), 0);
    CHECK_EQ(from_seconds(-1.0), 0);
    CHECK_EQ(from_seconds(1.0), 1000000);
    CHECK_EQ(from_seconds(0.5), 500000);
    CHECK_EQ(from_seconds(0.3), 300000);
    CHECK_EQ(from_seconds(3600.25), 3600250000LL);
    CHECK_EQ(from_seconds(1e-6), 1);
    CHECK_EQ(from_seconds(4e-7), 0);   // under half: down
    CHECK_EQ(from_seconds(6e-7), 1);   // over half: up
    CHECK_EQ(from_seconds(0.0000015), 2);  // 1.50000000000000003e-6: up
    CHECK_EQ(from_seconds(1e-300), 0);
    CHECK_EQ(from_seconds(std::numeric_limits<double>::denorm_min()), 0);
    CHECK_EQ(from_seconds(std::numeric_limits<double>::infinity()), kMax);
    CHECK_EQ(from_seconds(std::nan("")), 0);
    CHECK_EQ(from_seconds(1e300), kMax);
    CHECK_EQ(from_seconds(1e12), 1000000000000000000LL);
    CHECK_EQ(from_seconds(9007199254740992.0), kMax);  // 2^53 s
}

TEST(game_clock_from_float) {
    // The game's step: 0.005f is 0.004999999888... s.
    CHECK_EQ(from_seconds(0.005f), 5000);
    CHECK_EQ(from_seconds(0.0f), 0);
    CHECK_EQ(from_seconds(-0.005f), 0);
    CHECK_EQ(from_seconds(1.0f), 1000000);
    CHECK_EQ(from_seconds(0.1f), 100000);
    CHECK_EQ(from_seconds(std::numeric_limits<float>::infinity()), kMax);
    CHECK_EQ(from_seconds(std::nanf("")), 0);
    CHECK_EQ(from_seconds(std::numeric_limits<float>::max()), kMax);
    CHECK_EQ(from_seconds(std::numeric_limits<float>::denorm_min()), 0);
    CHECK_EQ(from_seconds(16777216.0f), 16777216000000LL);  // 2^24 s
}

TEST(game_clock_matches_rounding) {
    // Against floating point over a range of values: the same as llround,
    // except where s * 1e6 rounds near a half (floating point's product is
    // rounded; the conversion isn't).
    int near_half = 0;
    for (int i = 0; i < 200000; ++i) {
        const double s = i * 0.0001234567 + 0.37;
        const float f = static_cast<float>(s);
        for (const double v : {s, static_cast<double>(f)}) {
            const double product = v * 1e6;
            const Micros expected = static_cast<Micros>(std::llround(product));
            const Micros got = v == s ? from_seconds(s) : from_seconds(f);
            if (got == expected) continue;
            ++near_half;
            CHECK(std::abs(got - expected) == 1);
            CHECK(std::abs(product - std::floor(product) - 0.5) < 1e-6);
        }
    }
    CHECK(near_half < 100);
    // Exact values (Python's fractions): 0.1234565 is just under 123456.5 us,
    // 2.0000005 just over 2000000.5.
    CHECK_EQ(from_seconds(0.1234565), 123456);
    CHECK_EQ(from_seconds(2.0000005), 2000001);
}

TEST(game_clock_steps_sum_exactly) {
    // An hour of the game's steps: exactly 3600 s, where a float sum drifts.
    Micros t = 0;
    for (int i = 0; i < 720000; ++i) t += from_seconds(0.005f);
    CHECK_EQ(t, 3600 * kPerSecond);
    CHECK_EQ(to_seconds(t), 3600.0);
}
