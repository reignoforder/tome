#pragma once

// The TOME_DEBUG_CLICKS script (debug_input.h): steps separated by ';'.
// Scripts are paced by the game, not a clock: they wait for log lines and
// count presented frames, so they run the same on a fast machine and under
// software rendering. A step may still start at a time, "@ms" after the
// script started or after the last wait ended (optional; 0 without it).
//
//   click:<frame>[,<context>]   clicks the frame with that name (and
//                   context, 0 without one) through the game's own
//                   CControl::Click, on the main thread: no positions, so a
//                   layout change can't send it elsewhere
//   focus:<frame>[,<context>]   gives that frame (a text box) the keyboard
//                   focus, as clicking into it would
//   x,y             left click at a position (fractions of the client area),
//                   through the mouse messages: for what has no name (a
//                   dropdown's items, a text box taking the focus)
//   move:x,y        the cursor there, no click (1 is the last pixel: edges)
//   type:<text>     Enter, the text, Enter: a line in the in-game chat
//   text:<text>     the text only, into whatever has the keyboard
//   key:<name>      a key press with modifiers ("Ctrl+J", core/keys.h)
//   close           WM_CLOSE, like Alt-F4
//   minimize, restore   like Alt-Tab away and back
//   wait:<text>     until tome.log has a line containing the text; each wait
//                   looks only after the line the last one matched (or the
//                   script's start)
//   frames:<n>      until the game has presented n more frames
//   settle          until kSettleFrames frames in a row have had no UI timer
//                   tick: every UI animation (a screen or panel sliding in)
//                   has finished, so the UI takes clicks
//   until:<text>    after an action: until a line containing the text is
//                   logged after it, doing the action again every
//                   kUntilFrames frames (it was dropped: the screen wasn't
//                   taking input yet); the text must be logged as the action
//                   takes effect. Later waits look after that line.
//
// Texts can't contain ';' (it ends the step); they may contain '@' when the
// step ends with a time, which is taken from after the last one.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tome::debug_script {

enum class Kind { Click, ClickFrame, Focus, Move, Type, Text, Key, Close, Minimize, Restore, Wait, Frames, Until, Settle };

// Frames between an until step's tries.
inline constexpr uint32_t kUntilFrames = 30;
// Frames without a UI timer tick that make a settle step's quiet UI.
inline constexpr uint32_t kSettleFrames = 10;

struct Step {
    Kind kind = Kind::Click;
    double x = 0, y = 0;  // Click, Move
    std::string text;     // Type, Text, Key, Wait, Until; ClickFrame, Focus: the frame's name
    int context = 0;      // ClickFrame, Focus
    uint32_t frames = 0;  // Frames
    uint32_t at_ms = 0;
};

// Parses every step it can; the items it can't go to `bad`, as written.
std::vector<Step> parse(std::string_view spec, std::vector<std::string>& bad);

} // namespace tome::debug_script
