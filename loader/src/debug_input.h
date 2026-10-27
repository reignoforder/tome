#pragma once

#include <windows.h>

namespace tome::debug_input {

// Developer aid for UI tests: if TOME_DEBUG_CLICKS is set, plays its script
// (core/debug_script.h: clicks, chat lines, key presses, ...) into the game
// window, timed from this call.
void start(HWND game_window);

} // namespace tome::debug_input
