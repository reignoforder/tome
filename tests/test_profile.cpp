// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/profile.h"
#include "test.h"

#include <cmath>

using namespace tome::profile;

namespace {
bool near(double a, double b) {
    return std::abs(a - b) < 1e-9;
}
} // namespace

TEST(profile_empty) {
    Window w;
    Stats s = w.stats(100);
    CHECK_EQ(s.calls, 0u);
    CHECK_EQ(s.frames, 0u);
    CHECK(describe(s).empty());
}

TEST(profile_adds_up_by_frame) {
    Window w;
    w.add(10, 0.001, "event handler", "(tick)");
    w.add(10, 0.002, "event handler", "(tick)");
    w.add(11, 0.0005, "native", "Foo");
    // As of frame 10: nothing completed yet.
    CHECK_EQ(w.stats(10).frames, 0u);
    Stats s = w.stats(12);
    CHECK_EQ(s.frames, 2u);
    CHECK(near(s.average_ms, 1.75));
    CHECK(near(s.peak_ms, 3.0));
    CHECK_EQ(s.calls, 3u);
    CHECK(near(s.total_ms, 3.5));
    CHECK(near(s.slowest_ms, 2.0));
    CHECK_EQ(s.slowest, std::string("event handler (tick)"));
    // Frames without calls count as nothing.
    s = w.stats(16);
    CHECK_EQ(s.frames, 6u);
    CHECK(near(s.average_ms, 3.5 / 6));
}

TEST(profile_window_slides) {
    Window w;
    w.add(1, 0.010, "a", nullptr);  // 10 ms in frame 1
    for (uint32_t f = 2; f <= kFrames + 1; ++f) w.add(f, 0.001, "b", nullptr);
    Stats s = w.stats(kFrames + 2);
    CHECK_EQ(s.frames, kFrames);
    CHECK(near(s.average_ms, 1.0));  // frame 1 left the window
    CHECK(near(s.peak_ms, 1.0));
    CHECK(near(s.slowest_ms, 10.0));  // but stays the slowest call
    CHECK_EQ(s.slowest, std::string("a"));
    // A long pause: nothing left in the window.
    s = w.stats(kFrames * 5);
    CHECK(near(s.average_ms, 0.0));
    w.add(kFrames * 5, 0.002, "c", nullptr);
    s = w.stats(kFrames * 5 + 1);
    CHECK(near(s.peak_ms, 2.0));
}

TEST(profile_older_frame_counts_now) {
    Window w;
    w.add(5, 0.001, "a", nullptr);
    w.add(3, 0.001, "b", nullptr);  // another thread, behind
    Stats s = w.stats(6);
    CHECK(near(s.peak_ms, 2.0));
}

TEST(profile_slow_warns_once) {
    Window w;
    bool warned = false;
    int warnings = 0;
    for (uint32_t f = 1; f <= kFrames * 3; ++f) {
        if (w.add(f, (kSlowMs + 1) / 1000.0, "tick", nullptr)) {
            ++warnings;
            if (!warned) CHECK(f >= kFrames + 1);
            warned = true;
        }
    }
    CHECK_EQ(warnings, 1);
    Window fine;
    for (uint32_t f = 1; f <= kFrames * 3; ++f) CHECK(!fine.add(f, (kSlowMs - 1) / 1000.0, "tick", nullptr));
}

TEST(profile_describe) {
    Window w;
    w.add(1, 0.00042, "event handler", "(tick)");
    w.add(2, 0.0031, "event handler", "(tick)");
    CHECK_EQ(describe(w.stats(3)),
             std::string("1.76 ms a frame on average, 3.10 ms at most (the last 2 frames); slowest call: event "
                         "handler (tick), 3.10 ms"));
    CHECK_EQ(format_ms(0.004), std::string("0.004 ms"));
    CHECK_EQ(format_ms(0), std::string("0.00 ms"));
    Window early;
    early.add(0, 0.001, "tome_mod_load", nullptr);
    CHECK_EQ(describe(early.stats(0)), std::string("no frames yet; slowest call: tome_mod_load, 1.00 ms"));
}

TEST(profile_perf_lines) {
    std::vector<Named> mods(4);
    mods[0].id = "idle";  // ran nothing: left out
    mods[1].id = "light";
    mods[1].stats.calls = 5;
    mods[1].stats.frames = 10;
    mods[1].stats.average_ms = 0.1;
    mods[1].stats.peak_ms = 0.5;
    mods[2].id = "heavy";
    mods[2].stats.calls = 9;
    mods[2].stats.frames = 10;
    mods[2].stats.average_ms = 2;
    mods[2].stats.peak_ms = 4;
    mods[2].stats.slowest = "native Foo";
    mods[2].stats.slowest_ms = 3.5;
    mods[3].id = "early";
    mods[3].stats.calls = 1;
    std::vector<std::string> lines = perf_lines(mods, 8);
    CHECK_EQ(lines.size(), 3u);
    CHECK_EQ(lines[0], std::string("heavy: 2.00 ms a frame, 4.00 ms at most; slowest call: native Foo, 3.50 ms"));
    CHECK_EQ(lines[1], std::string("light: 0.10 ms a frame, 0.50 ms at most"));
    CHECK_EQ(lines[2], std::string("early: no frames yet"));
    lines = perf_lines(mods, 1);
    CHECK_EQ(lines.size(), 2u);
    CHECK_EQ(lines[1], std::string("... and 2 more"));
}

TEST(profile_lifecycle_calls_stay_out_of_the_frames) {
    Window w;
    // A 600 ms load at frame 0, then 1 ms a frame.
    w.add_lifecycle(0.6);
    bool warned = false;
    for (uint32_t f = 1; f <= 300; ++f) warned |= w.add(f, 0.001, "event handler", "(tick)");
    CHECK(!warned);
    const Stats s = w.stats(301);
    CHECK_EQ(s.frames, kFrames);
    CHECK(std::abs(s.average_ms - 1.0) < 1e-9);
    CHECK(std::abs(s.peak_ms - 1.0) < 1e-9);
    CHECK_EQ(s.slowest, std::string("event handler (tick)"));
    CHECK_EQ(s.calls, 301u);
    CHECK(std::abs(s.total_ms - 900.0) < 1e-6);
    CHECK(std::abs(s.lifecycle_ms - 600.0) < 1e-9);
    CHECK(describe(s).find("; starting and stopping: 600.00 ms") != std::string::npos);
}
