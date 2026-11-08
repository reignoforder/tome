// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/timers.h"
#include "test.h"

#include <cmath>
#include <limits>

using namespace tome::timers;
using tome::game_clock::Micros;

namespace {
Micros us(double seconds) {
    return tome::game_clock::from_seconds(seconds);
}
const int kA = 0, kB = 0;
const void* const a = &kA;
const void* const b = &kB;
} // namespace

TEST(timers_once) {
    Schedule s;
    uint32_t id = 0;
    CHECK(s.start(a, Clock::Real, us(10.0), 0.5, 0, id));
    CHECK(id != 0);
    CHECK(s.take_due(Clock::Real, us(10.4)).empty());
    std::vector<uint32_t> due = s.take_due(Clock::Real, us(10.5));
    CHECK_EQ(due.size(), 1u);
    CHECK_EQ(due[0], id);
    CHECK(!s.running(id));
    CHECK(s.take_due(Clock::Real, us(20.0)).empty());
}

TEST(timers_zero_delay_is_the_next_check) {
    Schedule s;
    uint32_t id = 0;
    CHECK(s.start(a, Clock::Game, us(3.0), 0, 0, id));
    CHECK_EQ(s.take_due(Clock::Game, us(3.0)).size(), 1u);
}

TEST(timers_repeat_keeps_its_schedule) {
    Schedule s;
    uint32_t id = 0;
    CHECK(s.start(a, Clock::Game, us(0.0), 1.0, 1.0, id));
    // Steps of 0.3 s: due at 1.0, 2.0, 3.0, each in the first step past it.
    int fired = 0;
    double fired_at[3] = {};
    for (int step = 1; step <= 10; ++step) {
        const double now = step * 0.3;
        for (uint32_t t : s.take_due(Clock::Game, us(now))) {
            CHECK_EQ(t, id);
            if (fired < 3) fired_at[fired] = now;
            ++fired;
        }
    }
    CHECK_EQ(fired, 3);
    CHECK(std::abs(fired_at[0] - 1.2) < 1e-9);
    CHECK(std::abs(fired_at[1] - 2.1) < 1e-9);
    CHECK(std::abs(fired_at[2] - 3.0) < 1e-9);
    CHECK(s.running(id));
}

TEST(timers_behind_dont_burst) {
    Schedule s;
    uint32_t id = 0;
    CHECK(s.start(a, Clock::Real, us(0.0), 0.1, 0.1, id));
    // A long frame: once, then 0.1 s after it.
    CHECK_EQ(s.take_due(Clock::Real, us(5.0)).size(), 1u);
    CHECK(s.take_due(Clock::Real, us(5.05)).empty());
    CHECK_EQ(s.take_due(Clock::Real, us(5.1)).size(), 1u);
    // An interval shorter than a step: once a step.
    uint32_t fast = 0;
    CHECK(s.start(b, Clock::Game, us(0.0), 0, 0.001, fast));
    CHECK_EQ(s.take_due(Clock::Game, us(0.005)).size(), 1u);
    CHECK_EQ(s.take_due(Clock::Game, us(0.010)).size(), 1u);
}

TEST(timers_order_and_clocks) {
    Schedule s;
    uint32_t late = 0, early = 0, same = 0, game = 0;
    CHECK(s.start(a, Clock::Real, us(0), 2.0, 0, late));
    CHECK(s.start(a, Clock::Real, us(0), 1.0, 0, early));
    CHECK(s.start(b, Clock::Real, us(0), 2.0, 0, same));
    CHECK(s.start(a, Clock::Game, us(0), 0.0, 0, game));
    std::vector<uint32_t> due = s.take_due(Clock::Real, us(3.0));
    CHECK_EQ(due.size(), 3u);
    CHECK_EQ(due[0], early);
    CHECK_EQ(due[1], late);
    CHECK_EQ(due[2], same);
    CHECK(s.running(game));
}

TEST(timers_stop) {
    Schedule s;
    uint32_t x = 0, y = 0, z = 0;
    CHECK(s.start(a, Clock::Real, us(0), 1, 1, x));
    CHECK(s.start(b, Clock::Real, us(0), 1, 1, y));
    CHECK(s.start(a, Clock::Game, us(0), 1, 1, z));
    CHECK(!s.stop(b, x));  // not b's
    CHECK(s.stop(a, x));
    CHECK(!s.stop(a, x));  // already stopped
    s.stop_clock(Clock::Game);
    CHECK(!s.running(z));
    CHECK(s.running(y));
    s.stop_owner(b);
    CHECK_EQ(s.size(), 0u);
}

TEST(timers_held_owners_wait) {
    Schedule s;
    uint32_t held = 0, other = 0;
    CHECK(s.start(a, Clock::Real, us(0), 0.5, 0, held));
    CHECK(s.start(b, Clock::Real, us(0), 0.5, 0, other));
    // a's owner can't run yet (a mod still setting up): its one-shot stays due.
    bool (*hold_a)(const void*) = [](const void* owner) { return owner == a; };
    std::vector<uint32_t> due = s.take_due(Clock::Real, us(1.0), hold_a);
    CHECK_EQ(due.size(), 1u);
    CHECK_EQ(due[0], other);
    CHECK(s.running(held));
    due = s.take_due(Clock::Real, us(1.1));
    CHECK_EQ(due.size(), 1u);
    CHECK_EQ(due[0], held);
    CHECK_EQ(s.size(), 0u);
}

TEST(timers_stop_clock_says_which) {
    Schedule s;
    uint32_t real = 0, g1 = 0, g2 = 0;
    CHECK(s.start(a, Clock::Real, us(0), 1, 1, real));
    CHECK(s.start(a, Clock::Game, us(0), 1, 1, g1));
    CHECK(s.start(b, Clock::Game, us(0), 1, 0, g2));
    std::vector<uint32_t> stopped = s.stop_clock(Clock::Game);
    CHECK_EQ(stopped.size(), 2u);
    CHECK_EQ(stopped[0], g1);
    CHECK_EQ(stopped[1], g2);
    CHECK(s.running(real));
    CHECK(s.stop_clock(Clock::Game).empty());
}

TEST(timers_refuse_bad_times) {
    Schedule s;
    uint32_t id = 0;
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    CHECK(!s.start(a, Clock::Real, us(0), -1, 0, id));
    CHECK(!s.start(a, Clock::Real, us(0), 0, -1, id));
    CHECK(!s.start(a, Clock::Real, us(0), inf, 0, id));
    CHECK(!s.start(a, Clock::Real, us(0), nan, 0, id));
    CHECK(!s.start(a, Clock::Real, us(0), 0, nan, id));
    CHECK_EQ(s.size(), 0u);
}

TEST(timers_whole_microseconds) {
    Schedule s;
    uint32_t tiny = 0, never = 0;
    // An interval under half a microsecond still repeats, once a step.
    CHECK(s.start(a, Clock::Game, 0, 0, 1e-9, tiny));
    CHECK_EQ(s.take_due(Clock::Game, 5000).size(), 1u);
    CHECK_EQ(s.take_due(Clock::Game, 10000).size(), 1u);
    CHECK(s.running(tiny));
    // A delay too long to count is never due, and doesn't overflow.
    CHECK(s.start(a, Clock::Game, 10000, 1e300, 1e300, never));
    CHECK(s.take_due(Clock::Game, tome::game_clock::kMax - 1).size() == 1u);  // only `tiny`
    CHECK(s.running(never));
}
