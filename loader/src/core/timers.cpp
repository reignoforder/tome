// SPDX-License-Identifier: GPL-3.0-or-later
#include "timers.h"

#include <algorithm>
#include <cmath>

namespace tome::timers {
namespace {

bool usable(double seconds) {
    return std::isfinite(seconds) && seconds >= 0;
}

} // namespace

bool Schedule::start(const void* owner, Clock clock, game_clock::Micros now, double delay, double interval,
                     uint32_t& id) {
    if (!usable(delay) || !usable(interval)) return false;
    game_clock::Micros every = game_clock::from_seconds(interval);
    if (every == 0 && interval > 0) every = 1;  // still repeating
    const game_clock::Micros after = game_clock::from_seconds(delay);
    // Past kMax: never due (an hour-long game is 3.6e9).
    const game_clock::Micros due = after >= game_clock::kMax - now ? game_clock::kMax : now + after;
    id = next_id_++;
    if (next_id_ == 0) next_id_ = 1;  // after 4 billion timers; ids still running are far apart by then
    timers_.push_back({id, owner, clock, due, every});
    return true;
}

bool Schedule::stop(const void* owner, uint32_t id) {
    auto it = std::find_if(timers_.begin(), timers_.end(),
                           [&](const Timer& t) { return t.id == id && t.owner == owner; });
    if (it == timers_.end()) return false;
    timers_.erase(it);
    return true;
}

void Schedule::stop_owner(const void* owner) {
    std::erase_if(timers_, [&](const Timer& t) { return t.owner == owner; });
}

std::vector<uint32_t> Schedule::stop_clock(Clock clock) {
    std::vector<uint32_t> ids;
    for (const Timer& t : timers_) {
        if (t.clock == clock) ids.push_back(t.id);
    }
    std::erase_if(timers_, [&](const Timer& t) { return t.clock == clock; });
    return ids;
}

std::vector<uint32_t> Schedule::take_due(Clock clock, game_clock::Micros now, bool (*held)(const void* owner)) {
    std::vector<size_t> due;  // indexes into timers_
    for (size_t i = 0; i < timers_.size(); ++i) {
        const Timer& t = timers_[i];
        if (t.clock == clock && t.due <= now && !(held && held(t.owner))) due.push_back(i);
    }
    if (due.empty()) return {};
    // Earliest first; started earlier first among equal times (the vector
    // keeps the order they were started in).
    std::stable_sort(due.begin(), due.end(), [&](size_t a, size_t b) { return timers_[a].due < timers_[b].due; });
    std::vector<uint32_t> ids;
    ids.reserve(due.size());
    bool finished = false;
    for (size_t i : due) {
        Timer& t = timers_[i];
        ids.push_back(t.id);
        if (t.interval == 0) {
            t.id = 0;  // its last time: removed below (no timer has id 0)
            finished = true;
            continue;
        }
        const game_clock::Micros base = t.due + t.interval <= now ? now : t.due;
        t.due = t.interval >= game_clock::kMax - base ? game_clock::kMax : base + t.interval;
    }
    if (finished) std::erase_if(timers_, [](const Timer& t) { return t.id == 0; });
    return ids;
}

bool Schedule::running(uint32_t id) const {
    return std::any_of(timers_.begin(), timers_.end(), [&](const Timer& t) { return t.id == id; });
}

} // namespace tome::timers
