#pragma once

// The TOME_DEBUG_CLICKS script (debug_input.h): steps separated by ';',
// each at a time in ms since the script started.
//
//   x,y@ms          left click; x and y are fractions of the client area
//   type:<text>@ms  Enter, the text, Enter: a line in the in-game chat
//   text:<text>@ms  the text only, into whatever has the keyboard
//   key:<name>@ms   a key press with modifiers ("Ctrl+J", core/keys.h)
//   close@ms        WM_CLOSE, like Alt-F4
//   minimize@ms, restore@ms   like Alt-Tab away and back
//
// Texts can't contain ';' (it ends the step); they may contain '@', since
// the time is taken from after the last one.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tome::debug_script {

enum class Kind { Click, Type, Text, Key, Close, Minimize, Restore };

struct Step {
    Kind kind = Kind::Click;
    double x = 0, y = 0;  // Click
    std::string text;     // Type, Text, Key
    uint32_t at_ms = 0;
};

// Parses every step it can; the items it can't go to `bad`, as written.
std::vector<Step> parse(std::string_view spec, std::vector<std::string>& bad);

} // namespace tome::debug_script
