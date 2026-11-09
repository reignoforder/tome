// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/widescreen.h"

#include "core/x86.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace tome::widescreen {

Scale projection_scale(int width, int height) {
    Scale s;
    if (width <= 0 || height <= 0) return s;
    // Compare width * 3 with height * 4 in integers, so 4:3 sizes (800x600,
    // 1024x768) are exactly 1, 1.
    long long wide = static_cast<long long>(width) * 3, tall = static_cast<long long>(height) * 4;
    if (wide > tall) {
        s.x = static_cast<float>(static_cast<double>(tall) / static_cast<double>(wide));
    } else if (wide < tall) {
        s.y = static_cast<float>(static_cast<double>(wide) / static_cast<double>(tall));
    }
    return s;
}

float ui_width(int width, int height) {
    if (width <= 0 || height <= 0) return kBaseWidth;
    // As in projection_scale: 4:3 sizes give exactly kBaseWidth.
    if (static_cast<long long>(width) * 3 <= static_cast<long long>(height) * 4) return kBaseWidth;
    return static_cast<float>(static_cast<double>(kUiHeight) * width / height);
}

namespace {

// The console's tiles are 512 x 512 (the fourth 64 wide), drawn 0.256 units
// wide (0.032 for the fourth): 0.0005 units a pixel. The game's bottom row
// (UI\FrameDef\UI\ConsoleUI.fdf) is tile 1 at 0, tile 2 (its lower part) at
// 0.256, tile 3 at 0.512 and tile 4 at 0.768, all from v 0.3125 (tile 2 from
// 0.4140625) down.
constexpr float kTile = 0.256f;
constexpr float kBottomHeight = 0.176f;  // tiles 1, 3 and 4
constexpr float kInfoHeight = 0.15f;     // tile 2
constexpr float kTopHeight = 0.032f;

// Where the bottom row is cut (tile u): tile 1 after the pillar beside the
// minimap's buttons (the portrait's arch starts at 0.82), tile 3 after the
// inventory's art (the command card's frame starts at 0.38).
constexpr float kCutLeft = 0.785f;
constexpr float kCutRight = 0.3125f;
// The wall between: the middle of tile 2, the top of the info panel's open
// arch (a row of stones over nothing), from the start of one stone to the
// end of another (pixels 118 to 338), so copies side by side read as one wall.
constexpr float kWallU0 = 118.0f / 512, kWallU1 = 338.0f / 512;
// The top bar's filler: one slot of tile 1's top row, separator to
// separator (pixels 172 to 342).
constexpr float kSlotU0 = 172.0f / 512, kSlotU1 = 342.0f / 512;
// The top bar's left end (tiles 1 and the first 0.087 of 2) and right end
// (0.341 from the right: tiles 2's last 0.053, 3 and 4); the clock between.
constexpr float kTopLeftEnd = 0.343f;
constexpr float kTopRightEnd = kBaseWidth - 0.341f;

struct Definition {
    const char* name;
    int tile;
    float u0, u1, v0, v1;
};
constexpr Definition kDefinitions[] = {
    {"TomeWideConsoleLeft", 1, 0.0f, kCutLeft, 0.3125f, 1.0f},
    {"TomeWideConsoleArch", 1, kCutLeft, 1.0f, 0.3125f, 1.0f},
    {"TomeWideConsoleInfo", 2, 0.0f, 1.0f, 0.4140625f, 1.0f},
    {"TomeWideConsoleInventory", 3, 0.0f, kCutRight, 0.3125f, 1.0f},
    {"TomeWideConsoleCommand", 3, kCutRight, 1.0f, 0.3125f, 1.0f},
    {"TomeWideConsoleEdge", 4, 0.0f, 1.0f, 0.3125f, 1.0f},
    {"TomeWideConsoleWall", 2, kWallU0, kWallU1, 0.4140625f, 1.0f},
    {"TomeWideConsoleSlot", 1, kSlotU0, kSlotU1, 0.0f, 0.125f},
};

// `count` pieces of `name` filling `width` units from `x`, each as close to
// `natural` units wide as a whole number of them allows (stretched a little,
// never cut).
void fill(std::vector<Piece>& out, const char* name, bool top, float x, float width, float natural, float height) {
    if (width <= 0.0f) return;
    int count = static_cast<int>(std::lround(width / natural));
    if (count < 1) count = 1;
    const float each = width / static_cast<float>(count);
    for (int i = 0; i < count; ++i) out.push_back({name, top, x + each * static_cast<float>(i), each, height});
}

} // namespace

std::string console_fdf() {
    std::string fdf = "// Tome: the console's art in pieces for a wide UI (core/widescreen.cpp)\r\n";
    char buf[512];
    for (const Definition& d : kDefinitions) {
        std::snprintf(buf, sizeof buf,
                      "Frame \"SIMPLEFRAME\" \"%s\" {\r\n"
                      "    DecorateFileNames,\r\n"
                      "    Texture {\r\n"
                      "        File \"ConsoleTexture0%d\",\r\n"
                      "        TexCoord %.6f, %.6f, %.6f, %.6f,\r\n"
                      "        AlphaMode \"ALPHAKEY\",\r\n"
                      "        Anchor TOPLEFT, 0, 0,\r\n"
                      "        Anchor BOTTOMRIGHT, 0, 0,\r\n"
                      "    }\r\n"
                      "}\r\n",
                      d.name, d.tile, d.u0, d.u1, d.v0, d.v1);
        fdf += buf;
    }
    return fdf;
}

float middle_shift(float wide) {
    return wide > kBaseWidth ? (wide - kBaseWidth) / 2 : 0.0f;
}

std::vector<ArtPiece> slice_art(float wide, float cut, float tile) {
    if (wide <= kBaseWidth || !(cut > 0.0f && cut < kBaseWidth) || !(tile > 0.0f && tile <= cut)) {
        return {{0.0f, kBaseWidth, 0.0f, kBaseWidth}};
    }
    std::vector<ArtPiece> out;
    out.push_back({0.0f, cut, 0.0f, cut});
    const float right = wide - (kBaseWidth - cut);  // where the art after the cut starts
    // Whole tiles only, each a little wider or narrower to fit: a part of
    // one would show a cut slot.
    const int count = std::max(1, static_cast<int>(std::lround((right - cut) / tile)));
    const float each = (right - cut) / static_cast<float>(count);
    for (int i = 0; i < count; ++i) {
        const float x = cut + each * static_cast<float>(i);
        out.push_back({cut - tile, cut, x, i + 1 == count ? right : x + each});
    }
    out.push_back({cut, kBaseWidth, right, wide});
    return out;
}

std::vector<BlurSample> blur_samples(int count, float radius, float brightness) {
    std::vector<BlurSample> out;
    if (count < 1) return out;
    // Blending copy k over the result at a_k weighs the copies before it by
    // (1 - a_k): for weights w_k proportional to a plain average, w_k =
    // w_{k-1} / (1 + w_{k-1}), so 1 / a_k = 1 / a_1 + k (from 0), and the
    // last copy's weight, a_{n-1}, is brightness / n.
    const float n = static_cast<float>(count);
    const float inv_a1 = n / brightness - n + 1;
    constexpr float kGoldenAngle = 2.39996323f;
    out.reserve(static_cast<size_t>(count));
    for (int k = 0; k < count; ++k) {
        const float r = radius * std::sqrt((k + 0.5f) / n), angle = k * kGoldenAngle;
        out.push_back({r * std::cos(angle), r * std::sin(angle), 1.0f / (inv_a1 + k)});
    }
    return out;
}

std::vector<Piece> console_layout(float wide) {
    if (!(wide > kBaseWidth)) wide = kBaseWidth;
    const float e = middle_shift(wide);
    const float left = kCutLeft * kTile;               // 0.20096: the left piece's width
    const float arch = (1.0f - kCutLeft) * kTile;      // to the end of tile 1
    const float inventory = kCutRight * kTile;         // tile 3 up to the cut
    const float command = (1.0f - kCutRight) * kTile;  // the rest of tile 3
    const float wall = (kWallU1 - kWallU0) * kTile;
    const float slot = (kSlotU1 - kSlotU0) * kTile;

    std::vector<Piece> out;
    out.push_back({"TomeWideConsoleLeft", false, 0.0f, left, kBottomHeight});
    fill(out, "TomeWideConsoleWall", false, left, e, wall, kInfoHeight);
    out.push_back({"TomeWideConsoleArch", false, left + e, arch, kBottomHeight});
    out.push_back({"TomeWideConsoleInfo", false, kTile + e, kTile, kInfoHeight});
    out.push_back({"TomeWideConsoleInventory", false, 2 * kTile + e, inventory, kBottomHeight});
    fill(out, "TomeWideConsoleWall", false, 2 * kTile + inventory + e, e, wall, kInfoHeight);
    out.push_back({"TomeWideConsoleCommand", false, wide - kBaseWidth + 2 * kTile + inventory, command,
                   kBottomHeight});
    out.push_back({"TomeWideConsoleEdge", false, wide - (kBaseWidth - 3 * kTile), kBaseWidth - 3 * kTile,
                   kBottomHeight});
    fill(out, "TomeWideConsoleSlot", true, kTopLeftEnd, e, slot, kTopHeight);
    fill(out, "TomeWideConsoleSlot", true, kTopRightEnd + e, e, slot, kTopHeight);
    return out;
}

bool constant_operands(const uint8_t* code, size_t available, uint32_t constant, std::vector<size_t>& out) {
    out.clear();
    size_t at = 0;
    while (at < available) {
        x86::Insn insn;
        std::string error;
        if (!x86::decode(code + at, available - at, insn, error)) return false;
        const uint8_t op = code[at];
        if (op == 0xC3 || op == 0xC2) return true;  // ret, ret N
        if (insn.length >= 5 && (code[at + insn.length - 5] & 0xC7) == 0x05) {
            uint32_t disp = 0;
            for (int i = 0; i < 4; ++i) disp |= static_cast<uint32_t>(code[at + insn.length - 4 + i]) << (8 * i);
            if (disp == constant) out.push_back(at + insn.length - 4);
        }
        at += insn.length;
    }
    return false;
}

} // namespace tome::widescreen
