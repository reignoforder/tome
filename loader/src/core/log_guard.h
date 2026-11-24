// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Limits on what untrusted text can do to tome.log (docs/known-bugs.md,
// "Tome's own surface"): a map's TomeLogString/TomeLogInteger can't forge
// lines or fill the disk.

#include "core/log_line.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace tome::log {

// `text` made safe to put inside one log line: control characters (C0, DEL)
// become '?', so it can't start a new line or forge one, and it's cut to at
// most `max_bytes` (at a UTF-8 character boundary) with "..." after it.
std::string printable(std::string_view text, size_t max_bytes);

// For a guard that refuses something untrusted input can make it refuse
// over and over (a map in a loop, a host, bad textures): whether to log the
// refusal numbered `total` (from 1). The first kFirst in full, then every
// kEvery-th, so tome.log can't be flooded and a long run still shows up.
struct Throttle {
    static constexpr unsigned long kFirst = 20;
    static constexpr unsigned long kEvery = 1000;
    static constexpr bool logs(unsigned long total) { return total <= kFirst || total % kEvery == 0; }
};

// How big tome.log may grow. Past `soft` bytes only warnings and errors are
// written (a crash report still gets in); past `hard`, nothing. The first
// line dropped gets a notice, once.
class SizeCap {
public:
    static constexpr uint64_t kSoft = 32ull << 20;
    static constexpr uint64_t kHard = kSoft + (1ull << 20);

    SizeCap(uint64_t soft = kSoft, uint64_t hard = kHard) : soft_(soft), hard_(hard) {}

    struct Verdict {
        bool write;   // write the line
        bool notice;  // first write the notice (notice_text) once, before it
    };
    // A line of `bytes` at `level`; counts it if it's written.
    Verdict take(Level level, size_t bytes);
    // Counts the notice line written for a Verdict's `notice`.
    void count(size_t bytes) { written_ += bytes; }

    uint64_t written() const { return written_; }

    static constexpr const char* notice_text =
        "tome.log reached its size limit: from now on only warnings and errors are written";

private:
    uint64_t soft_;
    uint64_t hard_;
    uint64_t written_ = 0;
    bool noticed_ = false;
};

} // namespace tome::log
