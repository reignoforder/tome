#pragma once

// The TOME_DEBUG_CLICKS script (debug_input.h): steps separated by ';',
// each at a time in ms since the script started or, after a wait step, since
// that wait ended. Waiting for log lines instead of guessing how long the
// game takes keeps a script working on slow machines (software rendering).
//
//   x,y@ms          left click; x and y are fractions of the client area
//   move:x,y@ms     the cursor there, no click (1 is the last pixel: edges)
//   type:<text>@ms  Enter, the text, Enter: a line in the in-game chat
//   text:<text>@ms  the text only, into whatever has the keyboard
//   key:<name>@ms   a key press with modifiers ("Ctrl+J", core/keys.h)
//   close@ms        WM_CLOSE, like Alt-F4
//   minimize@ms, restore@ms   like Alt-Tab away and back
//   wait:<text>@ms  from then on, until tome.log has a line containing the
//                   text; each wait looks only after the line the last one
//                   matched (or the script's start)
//
// Texts can't contain ';' (it ends the step); they may contain '@', since
// the time is taken from after the last one.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tome::debug_script {

enum class Kind { Click, Move, Type, Text, Key, Close, Minimize, Restore, Wait };

struct Step {
    Kind kind = Kind::Click;
    double x = 0, y = 0;  // Click, Move
    std::string text;     // Type, Text, Key, Wait
    uint32_t at_ms = 0;
};

// Parses every step it can; the items it can't go to `bad`, as written.
std::vector<Step> parse(std::string_view spec, std::vector<std::string>& bad);

} // namespace tome::debug_script
