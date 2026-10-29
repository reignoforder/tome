#pragma once

// How the widescreen patch corrects the game's projection matrix
// (patches/widescreen.cpp, docs/known-bugs.md "Stretched picture on
// widescreen"). Kept here, apart from the hook, so the edges can be tested.
//
// The game builds every 3D camera's projection for its viewport in the
// 0.8 x 0.6 UI space, which it stretches to the window: correct only in a
// 4:3 window. The scale goes on the matrix's x (m[0]) and y (m[5]) terms so
// the picture keeps its proportions and never shows less than at 4:3: a
// wider window shows more at the sides (the height stays), a narrower one
// (5:4) more at the top and bottom (the width stays).

namespace tome::widescreen {

struct Scale {
    float x = 1.0f;
    float y = 1.0f;
};

// The scale for a window's client area in pixels; 1, 1 for 4:3 and for an
// empty area (minimized).
Scale projection_scale(int width, int height);

} // namespace tome::widescreen
