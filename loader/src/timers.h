// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Mods' timers (tome.h, API version 19: timer_start, timer_stop). The
// schedule is core/timers.h; this runs the callbacks, on the main thread:
// real-time timers from each frame (events.cpp, PresentFrame), game-time
// ones from each step of a game (OnNetGameTick), after that step's
// GAME_TICK handlers. A game's timers stop when it ends.

#include "core/game_clock.h"

#include <tome/tome.h>

#include <cstdint>

namespace tome::timers_api {

void fill_api(tome_api& api);

// From events.cpp (and its test dispatch), on the main thread: a frame was
// drawn `delta` seconds after the last one; a game started; it took a step,
// to `now` (game time since it started, the same on every computer); it
// ended.
void on_frame(double delta);
void on_game_start();
void on_game_step(game_clock::Micros now);
void on_game_end();

// For mods that keep their own record of each timer (Wasm mods,
// wasm_mods.cpp): whether a timer will fire again (false inside its last
// callback), and a hook told of each timer that ended without firing
// (its game ended), after the fact, on the main thread. One hook.
bool running(uint32_t timer);
using EndedHook = void (*)(tome_mod* mod, uint32_t timer, tome_timer_fn fn, void* user);
void set_ended_hook(EndedHook hook);

} // namespace tome::timers_api
