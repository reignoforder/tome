// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// How much of the game's time each mod takes (the mod menu's Details page,
// /tome perf; docs/architecture.md, "Profiling mods"). Every call Tome makes
// into a mod already runs under the crash guard (guard.cpp), which times it
// too: the time the mod's own code took, not that of other mods' callbacks
// it caused (those are theirs). This adds those times up by frame and keeps
// the last kFrames frames, apart from the clock, so it can be unit-tested.

#include <cstdint>
#include <string>
#include <vector>

namespace tome::profile {

// Frames the averages cover: about 2 s at 60 frames a second.
constexpr uint32_t kFrames = 128;
// A mod that averages more than this over a whole window (milliseconds a
// frame) is slow enough to say so in the log, once.
constexpr double kSlowMs = 4.0;

struct Stats {
    uint32_t frames = 0;       // frames the window covers (at most kFrames)
    double average_ms = 0;     // a frame, over those frames
    double peak_ms = 0;        // the most in one of them
    uint64_t calls = 0;        // calls into the mod since it was loaded
    double total_ms = 0;       // their time
    double slowest_ms = 0;     // the longest single call since it was loaded
    std::string slowest;       // what it was ("event handler (tick)"); "" if none
    double lifecycle_ms = 0;   // of total_ms, in tome_mod_load/enable/disable/unload
};

// One mod's times. Not thread-safe: the caller locks.
class Window {
public:
    // `seconds` of the mod's own time in a call during frame `frame`
    // (counting up from 1; calls before the first frame count as frame 0).
    // `what` is the call's description, only read if it's the slowest yet.
    // True the first time a full window averages more than kSlowMs.
    bool add(uint32_t frame, double seconds, const char* what, const char* detail);
    // A lifecycle call (tome_mod_load, enable, disable, unload): counted in
    // the calls and total time only. A slow load isn't a slow frame, nor the
    // slowest call worth finding (it would hide the per-frame ones).
    void add_lifecycle(double seconds);
    // As of frame `now`: the frames before it (the current one is still
    // going on).
    Stats stats(uint32_t now) const;
    void reset() { *this = Window(); }

private:
    void roll_to(uint32_t frame);
    double ms_[kFrames] = {};    // by frame % kFrames
    uint32_t frame_ = 0;         // the frame being added up now
    uint32_t first_ = 0;         // the first frame with a call (the window starts there)
    bool any_ = false;
    bool warned_ = false;
    uint64_t calls_ = 0;
    double total_ms_ = 0;
    double slowest_ms_ = 0;
    std::string slowest_;
    double lifecycle_ms_ = 0;
};

// "0.42 ms a frame on average, 3.10 ms at most (the last 128 frames); slowest
// call: event handler (tick), 3.05 ms; starting and stopping: 512.00 ms". ""
// before the mod's first call.
std::string describe(const Stats& stats);
// "0.42 ms" with two decimals (more for tiny values: "0.004 ms").
std::string format_ms(double ms);

// /tome perf: a line per mod that ran any code ("hello: 0.42 ms a frame,
// 3.10 ms at most; slowest call: event handler (tick), 3.05 ms"), the
// busiest first, at most `max` of them (then "... and 3 more").
struct Named {
    std::string id;
    Stats stats;
};
std::vector<std::string> perf_lines(std::vector<Named> mods, size_t max);

} // namespace tome::profile
