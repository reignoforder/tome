#pragma once

#include <windows.h>

namespace tome::debug_input {

// Developer aid for UI tests: if TOME_DEBUG_CLICKS is set, posts left clicks
// to the game window. Format: "x,y@ms;x,y@ms;..." where x and y are fractions
// of the client area and ms is the delay since this call. An item
// "type:<text>@ms" instead presses Enter, types the text and presses Enter
// again: a line in the in-game chat. The text can't contain ';'. "close@ms"
// closes the window (WM_CLOSE, like Alt-F4); "minimize@ms" and "restore@ms"
// minimize it and bring it back, like Alt-Tab away and back.
void start(HWND game_window);

} // namespace tome::debug_input
