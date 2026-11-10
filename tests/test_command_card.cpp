// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/command_card.h"
#include "test.h"

#include <cstdint>

using namespace tome::command_card;

namespace {
constexpr uint32_t kMinusEleven = static_cast<uint32_t>(-11);
constexpr uint32_t kIntMin = 0x80000000u;
} // namespace

TEST(command_card_grid) {
    CHECK(in_grid(0, 0));
    CHECK(in_grid(3, 2));
    CHECK(!in_grid(4, 0));
    CHECK(!in_grid(0, 3));
    CHECK(!in_grid(static_cast<uint32_t>(-1), 0));  // negative: past the grid as unsigned
    CHECK(!in_grid(0, kMinusEleven));
    CHECK(!in_grid(kIntMin, kIntMin));
}

TEST(command_card_hiding_positions) {
    CHECK(hides(0, kMinusEleven));
    CHECK(hides(kIntMin, kIntMin));
    // Only those two pairs (1.28's note): the halves alone or mixed don't.
    CHECK(!hides(1, kMinusEleven));
    CHECK(!hides(kIntMin, 0));
    CHECK(!hides(0, kIntMin));
    CHECK(!hides(kIntMin, kMinusEleven));
    CHECK(!hides(0, 0));
}

TEST(command_card_what_setters_store) {
    // A cell: both, as the game does.
    Stored s = stored(2, 1);
    CHECK(s.x && s.y);
    // The hiding pairs: both, though they're outside the grid.
    s = stored(0, kMinusEleven);
    CHECK(s.x && s.y);
    s = stored(kIntMin, kIntMin);
    CHECK(s.x && s.y);
    // Anything else outside the grid: that coordinate is dropped (the
    // button keeps its default there), the other is kept.
    s = stored(static_cast<uint32_t>(-5), 1);
    CHECK(!s.x && s.y);
    s = stored(1, 7);
    CHECK(s.x && !s.y);
    s = stored(100000, 100000);
    CHECK(!s.x && !s.y);
    s = stored(2, kMinusEleven);  // not a hiding pair
    CHECK(s.x && !s.y);
}
