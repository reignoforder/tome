// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Mods' timers (tome.h, timer_start and timer_stop): which timers are due
// on a clock, apart from the callbacks (loader/src/timers.cpp), so it can
// be unit-tested.
//
// Two clocks, each a number of microseconds that only grows
// (core/game_clock.h): the real one (the sum of the frames' deltas) and
// the game's (game time, from the game's steps; it starts at 0 with each
// game). A timer is due once the clock reaches its time; firing it sets the next time `interval` after that,
// or, for a timer that fell behind (an interval shorter than a frame or a
// step, or a long frame), `interval` after now: at most once per frame or
// step, never a burst. Times are whole microseconds, converted from the
// mods' seconds without floating point, so the game clock's timers fire in
// the same step on every player's computer, whatever their FPU settings.

#include "game_clock.h"

#include <cstdint>
#include <vector>

namespace tome::timers {

enum class Clock { Real = 0, Game = 1 };

class Schedule {
public:
    // A new timer on `clock` (whose time is `now`): due `delay` seconds
    // from now, then every `interval` seconds (0: only once; more than 0
    // but under half a microsecond: every microsecond). Its id, never
    // 0. `owner` is whatever the caller groups timers by. False (no timer)
    // for a delay or interval that's negative, infinite or not a number.
    bool start(const void* owner, Clock clock, game_clock::Micros now, double delay, double interval, uint32_t& id);
    // Stops one of `owner`'s timers. False if it isn't one that's running.
    bool stop(const void* owner, uint32_t id);
    // Stops every timer of `owner`, or of `clock` (the ids it stopped).
    void stop_owner(const void* owner);
    std::vector<uint32_t> stop_clock(Clock clock);

    // The timers of `clock` due at `now`, in the order they're due (then
    // started). Each is rescheduled, or removed if it fires only once,
    // before the caller runs it, so a callback may start and stop timers.
    // Timers of an owner `held` says can't run now stay due, for a later
    // call (a mod still setting up on another thread).
    std::vector<uint32_t> take_due(Clock clock, game_clock::Micros now, bool (*held)(const void* owner) = nullptr);

    bool running(uint32_t id) const;
    size_t size() const { return timers_.size(); }

private:
    struct Timer {
        uint32_t id;
        const void* owner;
        Clock clock;
        game_clock::Micros due;
        game_clock::Micros interval;  // 0: only once
    };
    std::vector<Timer> timers_;
    uint32_t next_id_ = 1;
};

} // namespace tome::timers
