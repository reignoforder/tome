// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Command card button positions (docs/known-bugs.md, "Command card button
// positions"): what command-card-guard (patches/command_card_guard.cpp)
// stores and places. Portable and pure, so it's unit-tested
// (tests/test_command_card.cpp).
//
// The card is a grid of 4 columns by 3 rows. A button's position comes from
// the game data (Buttonpos in CommandFunc.txt, CustomKeys.txt, a map's
// object data). Maps hide a button with one of two positions outside the
// grid, (0, -11) and (INT_MIN, INT_MIN). 1.27a made the setters drop any
// coordinate outside the grid (1.26a placed the button by indexing the grid
// with it, and crashed), which dropped those two too, so the button shows;
// 1.28 brought them back and ignores every other bad position.

#include <cstdint>

namespace tome::command_card {

constexpr uint32_t kColumns = 4;
constexpr uint32_t kRows = 3;

// True if (x, y) is a cell of the grid.
constexpr bool in_grid(uint32_t x, uint32_t y) { return x < kColumns && y < kRows; }

// True if (x, y) is one of the two positions that hide a button.
constexpr bool hides(uint32_t x, uint32_t y) {
    return (x == 0 && y == static_cast<uint32_t>(-11)) || (x == 0x80000000u && y == 0x80000000u);
}

// What a setter given (x, y) stores, as 1.28 does: both if they hide the
// button; otherwise each coordinate that is inside the grid (1.27b's own
// check), and the other is left as it was (0 when the setter is
// CCommandCard::AddButton, which resets both first).
struct Stored {
    bool x;
    bool y;
};
constexpr Stored stored(uint32_t x, uint32_t y) {
    if (hides(x, y)) return {true, true};
    return {x < kColumns, y < kRows};
}

} // namespace tome::command_card
