// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// How the widescreen patch corrects the game's picture (patches/widescreen.cpp,
// docs/known-bugs.md "Stretched picture on widescreen"). Kept here, apart from
// the hooks, so it can be tested.
//
// The 3D view: the game builds every 3D camera's projection for its viewport
// in the 0.8 x 0.6 UI space, which it stretches to the window: correct only
// in a 4:3 window. projection_scale corrects the matrix's x (m[0]) and y
// (m[5]) terms so the picture keeps its proportions and never shows less
// than at 4:3.
//
// The 2D UI: the game converts between UI units and the screen's 0..1 as if
// the screen were always 0.8 units wide. With ui_width instead, the UI's
// root spans the whole window in units of the same size both ways, so every
// frame keeps its shape and frames anchored to the screen's edges go to the
// window's edges, as in 1.29. The console's art, made for 0.8 units, is then
// laid out again in pieces (console_layout): the minimap's end at the left
// edge, the portrait, info panel and inventory in the middle, the command
// card at the right edge, and wall between.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tome::widescreen {

struct Scale {
    float x = 1.0f;
    float y = 1.0f;
};

// The scale for a window's client area in pixels; 1, 1 for 4:3 and for an
// empty area (minimized).
Scale projection_scale(int width, int height);

// The UI's width in UI units for a client area: 0.6 * width / height, but
// never less than the game's 0.8 (a 4:3 or narrower window, or an empty one,
// keeps 0.8).
float ui_width(int width, int height);

// The memory operands in a function's code that read the float at
// `constant` (an absolute address: `[disp32]`, ModRM mod 0 rm 5, the
// operand last in the instruction, as in divss/mulss xmm, [c] and
// fdiv/fmul [c]): the offsets of their 4 address bytes from `code`, up to
// the first ret (`available` bytes of `code` readable). False if an instruction isn't known (core/x86.h) or there's no
// ret within `available`.
bool constant_operands(const uint8_t* code, size_t available, uint32_t constant, std::vector<size_t>& out);

// The UI's height in UI units, whatever the window.
constexpr float kUiHeight = 0.6f;
// The width the game's UI is laid out for.
constexpr float kBaseWidth = 0.8f;

// --- the console's art -------------------------------------------------------------

// The FDF that defines the console's pieces (SIMPLEFRAMEs, one texture
// each: the race's console tiles through DecorateFileNames, cut with
// TexCoord), and where Tome's overlay puts it (listed in Tome.toc).
constexpr const char* kConsoleFdfPath = "UI\\FrameDef\\UI\\TomeWideConsole.fdf";
std::string console_fdf();

// One piece of the console's art: an instance of the FDF frame `name`, its
// bottom-left (bottom pieces) or top-left (top pieces) corner `x` units from
// the console's left edge, `width` x `height` units.
struct Piece {
    const char* name;
    bool top;
    float x;
    float width;
    float height;
};

// The console's art for a UI `wide` units across: the bottom row in pieces
// and the top bar's filler between its two ends (the game's own top-bar
// tiles stay, anchored to the edges). At kBaseWidth the pieces add up to the
// game's own bottom row, with no fillers. `wide` below kBaseWidth counts as
// kBaseWidth.
std::vector<Piece> console_layout(float wide);

// How far the middle of the console (portrait, info panel, inventory, the
// clock) moves right for a UI `wide` units across: half the extra width.
float middle_shift(float wide);

// --- warped art ----------------------------------------------------------------------

// Full-screen models on the menus that are stretched across the window, as
// the game draws them at any shape, instead of drawn in proportion: art
// whose frames the game anchors to the window's edges, which in proportion
// would leave those frames off the art and black at the sides (Battle.net's
// login doors, its chat screen's button bar). By FDF frame name; the layer
// that dims a screen behind a dialog (a CHighlightFrame) is warped too.
inline constexpr const char* kWarpedFrames[] = {
    "BattleNetMainBackground",  // BattleNetMain.fdf: the login screen's doors
};

// Full-screen models drawn in pieces on a wider UI: art [0, cut] at the
// window's left in proportion, [cut, kBaseWidth] at its right, and between
// them the `tile` units of art before `cut` repeated, as many whole times
// as fit best, each stretched a little to fill the gap (art units: the model
// as made for 0..kBaseWidth). For art with frames anchored on both sides
// that a stretch would leave off it: Battle.net's button bar, whose buttons
// hang from the left, its end from the right, and whose slots repeat.
struct SlicedFrame {
    const char* name;  // FDF frame name
    float cut;
    float tile;
};
inline constexpr SlicedFrame kSlicedFrames[] = {
    // BattleNetChatPanel.fdf: the bar under the ad, its buttons' slots 0.0547
    // apart; cut after the last slot (the profile button's), before the
    // bolts and the right end.
    {"ButtonBarBackground", 0.607f, 0.0547f},
};

// One piece of a sliced model: the art from art_from to art_to drawn from
// at_from to at_to of the UI (units from its left edge).
struct ArtPiece {
    float art_from;
    float art_to;
    float at_from;
    float at_to;
};

// The pieces of art sliced at `cut` with a `tile` repeated, for a UI `wide`
// units across, left to right. At kBaseWidth or narrower (or for a cut or
// tile out of range) one piece: the art as it is.
std::vector<ArtPiece> slice_art(float wide, float cut, float tile);

// --- the boxed screens' side fill ---------------------------------------------------

// The loading and score screens are laid out at kBaseWidth in the middle of
// a UI `wide` units across: their box is box_margin in from each side (the
// same as middle_shift).
inline float box_margin(float wide) { return middle_shift(wide); }

// One copy of the blurred art drawn beside a boxed screen: shifted by dx, dy
// (UI units) and blended over the copies before it at `alpha` (0..1 of the
// art's own alpha).
struct BlurSample {
    float dx;
    float dy;
    float alpha;
};

// `count` copies spread evenly over a disc `radius` across (a Vogel spiral),
// with alphas a_k (1 / a_k = 1 / a_1 + k) chosen so that blending them one
// over the other in order gives `brightness` times their plain average.
// Empty for a count below 1.
std::vector<BlurSample> blur_samples(int count, float radius, float brightness);

} // namespace tome::widescreen
