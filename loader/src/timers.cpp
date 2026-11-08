// SPDX-License-Identifier: GPL-3.0-or-later
#include "timers.h"

#include "core/timers.h"
#include "frames.h"
#include "guard.h"
#include "log.h"
#include "mod_handle.h"
#include "unwind.h"

#include <windows.h>

#include <atomic>
#include <map>
#include <set>
#include <vector>

namespace tome::timers_api {
namespace {

using timers::Clock;

struct Callback {
    tome_mod* mod;
    tome_timer_fn fn;
    void* user;
    bool once;  // no interval: gone from the schedule once it's due
};

// Registration may happen on any thread (real-time timers); callbacks run
// on the main thread with the lock released, so they may start and stop
// timers.
SRWLOCK g_lock = SRWLOCK_INIT;
timers::Schedule g_schedule;
std::map<uint32_t, Callback> g_callbacks;  // by timer id
std::set<const tome_mod*> g_revoking;      // mods with their one revoker registered
std::atomic<int> g_count{0};               // timers running, so frames without any cost nothing
game_clock::Micros g_real_now = 0;  // real time, the sum of the frames' deltas
game_clock::Micros g_game_now = 0;  // game time in this game (events.cpp counts it)
bool g_in_game = false; // main thread
std::atomic<EndedHook> g_ended_hook{nullptr};  // set from a Wasm mod's load (any thread)

void update_count() {
    g_count.store(static_cast<int>(g_schedule.size()), std::memory_order_relaxed);
}

// A mod still in its tome_mod_load or tome_mod_enable on another thread
// (the bootstrap thread's first load): the guard would skip the call, and a
// timer that fires only once would be lost. Its due timers wait instead.
bool starting_elsewhere(const void* owner) {
    const unsigned long starting =
        static_cast<const tome_mod*>(owner)->starting_on.load(std::memory_order_acquire);
    return starting && starting != GetCurrentThreadId();
}

// One revoker per mod, however many timers it starts: a mod starting a
// timer every frame mustn't grow its revoker list.
void ensure_revoker(tome_mod* mod) {
    if (!g_revoking.insert(mod).second) return;
    mod->add_revoker([mod] {
        ExclusiveLock hold(g_lock);
        g_schedule.stop_owner(mod);
        std::erase_if(g_callbacks, [mod](const auto& e) { return e.second.mod == mod; });
        g_revoking.erase(mod);
        update_count();
    });
}

tome_result TOME_CALL api_timer_start(tome_mod* mod, int32_t clock, double delay, double interval, tome_timer_fn fn,
                                      void* user, uint32_t* timer) {
    if (timer) *timer = 0;
    if (!mod || !fn || (clock != TOME_CLOCK_REAL && clock != TOME_CLOCK_GAME)) return TOME_ERR_INVALID_ARGUMENT;
    if (clock == TOME_CLOCK_GAME) {
        if (!frames::on_main_thread()) {
            TOME_ERROR("[%s] timer_start(TOME_CLOCK_GAME) called off the game's main thread; ignored",
                       mod->id.c_str());
            return TOME_ERR_WRONG_THREAD;
        }
        if (!g_in_game) return TOME_ERR_UNAVAILABLE;
    }
    uint32_t id = 0;
    {
        ExclusiveLock hold(g_lock);
        const Clock c = clock == TOME_CLOCK_GAME ? Clock::Game : Clock::Real;
        if (!g_schedule.start(mod, c, c == Clock::Game ? g_game_now : g_real_now, delay, interval, id)) {
            hold.unlock();
            TOME_ERROR("[%s] timer_start: the delay and interval must be 0 or more seconds (got %g and %g)",
                       mod->id.c_str(), delay, interval);
            return TOME_ERR_INVALID_ARGUMENT;
        }
        g_callbacks[id] = {mod, fn, user, interval <= 0};
        ensure_revoker(mod);
        update_count();
    }
    if (timer) *timer = id;
    return TOME_OK;
}

tome_result TOME_CALL api_timer_stop(tome_mod* mod, uint32_t timer) {
    if (!mod) return TOME_ERR_INVALID_ARGUMENT;
    ExclusiveLock hold(g_lock);
    if (!g_schedule.stop(mod, timer)) return TOME_ERR_INVALID_ARGUMENT;
    g_callbacks.erase(timer);
    update_count();
    return TOME_OK;
}

void fire(Clock clock, game_clock::Micros now) {
    std::vector<uint32_t> due;
    {
        ExclusiveLock hold(g_lock);
        due = g_schedule.take_due(clock, now, starting_elsewhere);
        update_count();
    }
    // Dropped if a crash guard further out abandons this round.
    OnUnwind release([&] { std::vector<uint32_t>().swap(due); });
    for (uint32_t id : due) {
        Callback cb;
        {
            ExclusiveLock hold(g_lock);
            auto it = g_callbacks.find(id);
            // Stopped (or its mod disabled) by an earlier callback.
            if (it == g_callbacks.end()) continue;
            cb = it->second;
            // A timer that fired for the last time is gone from the schedule.
            if (cb.once) g_callbacks.erase(it);
        }
        guard::run(cb.mod, "timer callback", nullptr, [&] { cb.fn(cb.mod, id, cb.user); });
    }
}

} // namespace

void fill_api(tome_api& api) {
    api.timer_start = api_timer_start;
    api.timer_stop = api_timer_stop;
}

void on_frame(double delta) {
    game_clock::Micros now;
    {
        ExclusiveLock hold(g_lock);
        g_real_now += game_clock::from_seconds(delta);
        now = g_real_now;
    }
    if (g_count.load(std::memory_order_relaxed) == 0) return;
    fire(Clock::Real, now);
}

void on_game_start() {
    on_game_end();  // a game that didn't say it ended
    ExclusiveLock hold(g_lock);
    g_in_game = true;
    g_game_now = 0;
}

void on_game_step(game_clock::Micros now) {
    if (!g_in_game) return;
    if (now > g_game_now) g_game_now = now;
    if (g_count.load(std::memory_order_relaxed) == 0) return;
    fire(Clock::Game, g_game_now);
}

void on_game_end() {
    std::vector<std::pair<uint32_t, Callback>> ended;
    {
        ExclusiveLock hold(g_lock);
        g_in_game = false;
        for (uint32_t id : g_schedule.stop_clock(Clock::Game)) {
            auto it = g_callbacks.find(id);
            if (it == g_callbacks.end()) continue;
            ended.emplace_back(id, it->second);
            g_callbacks.erase(it);
        }
        update_count();
    }
    const EndedHook hook = g_ended_hook.load(std::memory_order_acquire);
    if (!hook) return;
    for (const auto& [id, cb] : ended) hook(cb.mod, id, cb.fn, cb.user);
}

bool running(uint32_t timer) {
    SharedLock hold(g_lock);
    return g_schedule.running(timer);
}

void set_ended_hook(EndedHook hook) {
    g_ended_hook.store(hook, std::memory_order_release);
}

} // namespace tome::timers_api
