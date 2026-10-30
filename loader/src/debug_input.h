#pragma once

#include <windows.h>

namespace tome::debug_input {

// In developer builds (TOME_DEV), if TOME_DEBUG_CLICKS is set, parses it and, if it has wait steps, has the
// log keep its lines from now on (until the script ends). Called as soon as
// the log is open; start() then plays it.
void prepare();

// Developer aid for UI tests: if TOME_DEBUG_CLICKS is set, plays its script
// (core/debug_script.h: clicks, chat lines, key presses, waits for log
// lines, ...) into the game window, timed from this call.
void start(HWND game_window);

} // namespace tome::debug_input
