// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Game time as a whole number of microseconds, so every player's computer
// counts it the same way. The game hands Tome each step as a float (0.005 s)
// and mods give timer delays as doubles; summed or compared as floating
// point in 32-bit code, they'd go through the x87 FPU, whose rounding
// depends on the thread's precision setting (Direct3D sets single
// precision, -opengl leaves double), which can differ between players.
// These conversions read the number's bits with integer arithmetic only:
// the same result whatever the FPU's settings. Game timers' due times and
// desync rounds (core/timers.h, core/desync.h) count in these units.

#include <cstdint>

namespace tome::game_clock {

using Micros = int64_t;

constexpr Micros kPerSecond = 1000000;
// What seconds() gives for a time too large to count (about 146,000 years).
constexpr Micros kMax = Micros(1) << 62;

// `s` seconds, rounded to the nearest microsecond (halves up). 0 for a
// negative number or NaN; kMax for one past it (infinity too).
Micros from_seconds(double s);
Micros from_seconds(float s);

// For display and mods' event info only: dividing is floating point again.
inline double to_seconds(Micros us) {
    return static_cast<double>(us) / static_cast<double>(kPerSecond);
}

} // namespace tome::game_clock
