// SPDX-License-Identifier: GPL-3.0-or-later
#include "profile.h"

#include <algorithm>
#include <cstdio>

namespace tome::profile {

void Window::roll_to(uint32_t frame) {
    if (frame <= frame_) return;
    // Frames without a call took nothing; a gap of a whole window clears it.
    const uint32_t gap = frame - frame_;
    if (gap >= kFrames) {
        for (double& ms : ms_) ms = 0;
    } else {
        for (uint32_t f = frame_ + 1; f <= frame; ++f) ms_[f % kFrames] = 0;
    }
    frame_ = frame;
}

bool Window::add(uint32_t frame, double seconds, const char* what, const char* detail) {
    if (!(seconds >= 0)) seconds = 0;
    const double ms = seconds * 1000.0;
    if (!any_) {
        any_ = true;
        first_ = frame;
        frame_ = frame;
        ms_[frame % kFrames] = 0;
    }
    // A call from another thread may report an older frame: it counts in the current one.
    roll_to(frame);
    ms_[frame_ % kFrames] += ms;
    ++calls_;
    total_ms_ += ms;
    if (ms > slowest_ms_) {
        slowest_ms_ = ms;
        slowest_ = what ? what : "call";
        if (detail && *detail) slowest_ += std::string(" ") + detail;
    }
    if (warned_ || frame_ < first_ + kFrames) return false;
    const Stats s = stats(frame_);
    if (s.frames < kFrames || s.average_ms <= kSlowMs) return false;
    warned_ = true;
    return true;
}

void Window::add_lifecycle(double seconds) {
    if (!(seconds >= 0)) seconds = 0;
    const double ms = seconds * 1000.0;
    ++calls_;
    total_ms_ += ms;
    lifecycle_ms_ += ms;
}

Stats Window::stats(uint32_t now) const {
    Stats s;
    s.lifecycle_ms = lifecycle_ms_;
    s.calls = calls_;
    s.total_ms = total_ms_;
    s.slowest_ms = slowest_ms_;
    s.slowest = slowest_;
    if (!any_ || now <= first_) return s;
    // Completed frames: [first_, now - 1], the last kFrames of them.
    const uint32_t last = now - 1;
    const uint32_t count = last - first_ + 1 < kFrames ? last - first_ + 1 : kFrames;
    double sum = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t f = last - i;
        // Frames past the last one added up took nothing; so did frames the
        // ring has already reused.
        const double ms = f > frame_ || frame_ - f >= kFrames ? 0.0 : ms_[f % kFrames];
        sum += ms;
        if (ms > s.peak_ms) s.peak_ms = ms;
    }
    s.frames = count;
    s.average_ms = sum / count;
    return s;
}

std::string format_ms(double ms) {
    char buf[32];
    if (ms > 0 && ms < 0.01) {
        std::snprintf(buf, sizeof(buf), "%.3f ms", ms);
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f ms", ms);
    }
    return buf;
}

std::string describe(const Stats& s) {
    if (s.calls == 0) return "";
    std::string text;
    if (s.frames > 0) {
        text = format_ms(s.average_ms) + " a frame on average, " + format_ms(s.peak_ms) + " at most (the last " +
               std::to_string(s.frames) + " frames)";
    } else {
        text = "no frames yet";
    }
    if (!s.slowest.empty()) text += "; slowest call: " + s.slowest + ", " + format_ms(s.slowest_ms);
    if (s.lifecycle_ms > 0) text += "; starting and stopping: " + format_ms(s.lifecycle_ms);
    return text;
}

std::vector<std::string> perf_lines(std::vector<Named> mods, size_t max) {
    std::erase_if(mods, [](const Named& m) { return m.stats.calls == 0; });
    std::stable_sort(mods.begin(), mods.end(), [](const Named& a, const Named& b) {
        if (a.stats.average_ms != b.stats.average_ms) return a.stats.average_ms > b.stats.average_ms;
        return a.stats.total_ms > b.stats.total_ms;
    });
    std::vector<std::string> lines;
    for (size_t i = 0; i < mods.size() && i < max; ++i) {
        const Stats& s = mods[i].stats;
        std::string line = mods[i].id + ": ";
        line += s.frames ? format_ms(s.average_ms) + " a frame, " + format_ms(s.peak_ms) + " at most" : "no frames yet";
        if (!s.slowest.empty()) line += "; slowest call: " + s.slowest + ", " + format_ms(s.slowest_ms);
        lines.push_back(std::move(line));
    }
    if (mods.size() > max) lines.push_back("... and " + std::to_string(mods.size() - max) + " more");
    return lines;
}

} // namespace tome::profile
