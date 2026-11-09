// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Widescreen (docs/known-bugs.md, "Stretched picture on widescreen";
 * docs/reverse-engineering.md, "Camera projection" and "The 2D UI's
 * coordinates").
 *
 * 1.27b lays everything out for a 4:3 screen and stretches it to the window.
 * In a 16:9 window units and buildings are a third too wide, no more of the
 * map shows than at 4:3, and the UI is stretched the same way.
 *
 * The 2D UI (where mapped). The game turns UI units (0.8 x 0.6) into the
 * screen's 0..1 and back through six small functions (DDCToNDC, NDCToDDC,
 * their Rect and Width forms) that divide or multiply x by 0.8. Their
 * operands are pointed at the UI's real width instead (widescreen::ui_width:
 * 0.6 x the window's aspect), so a UI unit is as wide as it's tall: every
 * frame and text keeps its shape, and since the UI's root is the whole
 * screen in those units, frames anchored to its edges go to the window's
 * edges, as in 1.29. The mouse goes through the same conversions.
 *
 * In a game, what the game laid out from the left edge for a 4:3 screen is
 * then laid out again, every frame (follow_ui; the game builds and moves
 * parts of its UI as it goes): the console's bottom art becomes pieces
 * (widescreen::console_layout: the minimap's end at the left edge, the
 * portrait's arch, info panel and inventory in the middle, the command card
 * at the right edge, wall between, and slots in the top bar each side of the
 * clock; simple frames from Tome's FDF, widescreen::console_fdf, children of
 * the console), the game's own bottom tiles go off the screen, and what sat
 * in the middle of a 4:3 screen (portrait, info panel, life and mana text,
 * clock, inventory cover) moves to the middle of this one.
 *
 * The menus fill the window, as in 1.29: their frames go to the window's
 * edges as the game anchors them, and their panels are the glue manager's
 * sprite layers, models made for 4:3 that are drawn in proportion at the
 * side they belong to (the left panels and border on the left, the right
 * ones on the right; on_ortho), over the 3D background, which fills the
 * window. The loading and score screens are one piece of art each with
 * frames placed on it: they're laid out at 4:3 in the middle of the window
 * (box_screen, box_sprite). A full-screen model that's neither (or not yet:
 * its first frame) gets the game's 0.8 across its flat camera, stretched
 * across the window as the game draws it; with the setting off, all are.
 * Some art is warped, stretched across the window on purpose
 * (widescreen::kWarpedFrames, and the layer that dims a screen behind a
 * dialog): its frames sit at the window's edges, off art kept in proportion.
 * Some is sliced (widescreen::kSlicedFrames, draw_sliced): its left part at
 * the window's left, its right end at the right, and a part of it repeated
 * between (Battle.net's button bar, whose buttons hang from the left).
 * Beside the loading and score screens, the same art is drawn again,
 * stretched across the window, blurred and darkened (on_sprite_render).
 *
 * The 3D view. With the 2D part the world frame is the window's shape, so
 * its camera is too. Without it (not mapped, or a window narrower than 4:3)
 * an AFTER handler on C44Matrix::Perspective scales the world camera's
 * matrix (only the calls made from WorldView::SetupCamera, or the world
 * frame's CSpriteFrame::SetupCamera, which unit picking uses) by (4/3) /
 * the window's aspect, so the view keeps its height and shows more at the
 * sides; 3D models inside UI frames (portraits) are left alone.
 *
 * The player can turn it off (Tome > Settings > Widescreen,
 * tome_settings.h). The 2D part follows from the next screen (the next menu
 * screen, or the next game: text is laid out as it's made and kept); the 3D
 * view alone (no 2D part) from the next frame. Everything here runs on the
 * main thread.
 */
#include "patches.h"

#include "core/widescreen.h"
#include "events.h"
#include "game_build.h"
#include "hooks.h"
#include "log.h"
#include "main_thread.h"
#include "patch.h"
#include "natives.h"
#include "tome/tome.h"
#include "tome_settings.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <iterator>
#include <vector>

// GCC warns that __thiscall is meant for class methods, but it does apply it
// to these function pointer types.
#pragma GCC diagnostic ignored "-Wattributes"

namespace tome::patches {
namespace {

int g_in_world_camera = 0;  // world camera setups running (main thread)
// Per CSpriteFrame::SetupCamera running, innermost last: what its BEFORE
// handler found it to be (main thread).
enum class Sprite { Model, World, Screen };
struct SpriteCamera {
    Sprite kind;
    char* frame;
};
std::vector<SpriteCamera> g_sprite_cameras;

GameUiGetFn g_game_ui_get = nullptr;
uint32_t g_world_frame_offset = 0;  // CGameUI.world_frame
bool g_wide_ui = false;             // the 2D part is installed

// --- the UI's width ----------------------------------------------------------------

// The UI's width in UI units now: the window's, or the game's 0.8 with the
// setting off or without the 2D part. The same in games and menus: keeping
// 0.8 in the menus left part of the game's UI (built under the loading
// screen) laid out for 0.8 in games, the top bar's text among it.
// The setting as it was when the current screen was made (latched by
// CGlueMgr::SetScreen: a menu screen, or the loading screen a game's UI is
// built under). The UI lays its text out as it's made and keeps it, so the
// width it was made for has to stay until the next screen.
bool g_wide_setting = true;
// The width for the last client area that wasn't empty (main thread). A
// minimized window has none; laying the UI out for 0.8 meanwhile would
// leave what's made then (chat lines in a game that goes on) at 0.8.
float g_last_wide = widescreen::kBaseWidth;

float ui_width() {
    if (!g_wide_ui || !g_wide_setting) return widescreen::kBaseWidth;
    int width = 0, height = 0;
    main_thread::client_size(width, height);
    if (width > 0 && height > 0) g_last_wide = widescreen::ui_width(width, height);
    return g_last_wide;
}

// The width the six conversions read: their x operands point here instead
// of at the game's 0.8 (DDCWidth, which other code reads too), so they run
// as the game's code with no hook. A float on 1.27b, a double on 1.26a
// (x87 code: fmul/fdiv QWORD). Set on the main thread whenever ui_width()
// may change: a new screen (the setting), the window's size.
float g_conversion_width = widescreen::kBaseWidth;
double g_conversion_width_double = widescreen::kBaseWidth;

void refresh_width() {
    g_conversion_width = ui_width();
    g_conversion_width_double = g_conversion_width;
}

// --- the game UI laid out again -------------------------------------------------------

using SetPointFn = void(__thiscall*)(void* layout, int point, void* relative, int relative_point, float x, float y,
                                     int update);
using SetSizeFn = void(__thiscall*)(void* layout, float size);
using CreateSimpleFrameFn = void*(__fastcall*)(const char* name, void* parent, int context);
using VoidMethod = void(__thiscall*)(void* object);

struct Ui {
    SetPointFn set_point = nullptr;
    SetSizeFn set_width = nullptr, set_height = nullptr;
    VoidMethod clear_points = nullptr;
    CreateSimpleFrameFn create_simple_frame = nullptr;
    uint32_t frame_layout = 0;  // CFrame.layout
    uint32_t points = 0;        // CLayoutFrame.points
    uintptr_t relative_point = 0;  // CLayoutFrame.relative_point_vtable (absolute)
    uint32_t dependents = 0;    // CLayoutFrame.dependents
    uint32_t width = 0;         // CLayoutFrame.width (height after it)
    uint32_t scale = 0;         // CLayoutFrame.scale
    uint32_t console = 0, info_panel = 0, clock = 0;  // CGameUI fields
    uint32_t inventory_cover = 0;  // CInfoPanel.inventory_cover
    uint32_t console_upper = 0, portrait_layer = 0, minimap_layer = 0, resource_bar = 0;  // CGameUI fields
    uint32_t shown = 0, show_flag = 0;  // CSimpleFrame.shown, .show_flag
    uint32_t hide_slot = 0, show_slot = 0;
    uint32_t root_update_slot = 0;  // CLayoutFrame.vtable.UpdateScreenRect (on the layout)
};
Ui g_ui;

constexpr int kTopLeft = 0, kBottomLeft = 6, kBottomRight = 8;

template <class T>
T field(const void* object, uint32_t offset) {
    T value;
    std::memcpy(&value, static_cast<const char*>(object) + offset, sizeof value);
    return value;
}

void* point_of(void* layout, int point) {
    return field<void*>(layout, g_ui.points + 4 * static_cast<uint32_t>(point));
}

// Whether a point is relative to a frame. One relative to the screen
// (SetPointAbsolute: the inventory cover, tooltips kept on the screen) holds
// x where the other holds the frame, and is smaller: it's never read as one.
bool relative_to_frame(const void* point) {
    return point && field<uintptr_t>(point, 0) == g_ui.relative_point;
}

// A point of a frame, as read back.
struct Anchor {
    void* relative = nullptr;
    int relative_point = 0;
    float x = 0, y = 0;
};

// False if it has no such point, or one relative to the screen.
bool read_anchor(void* layout, int point, Anchor& out) {
    void* p = layout ? point_of(layout, point) : nullptr;
    if (!relative_to_frame(p)) return false;
    out = {field<void*>(p, 4), field<int>(p, 8), field<float>(p, 0xC), field<float>(p, 0x10)};
    return true;
}

// A point of the console's middle (portrait, info panel, clock, inventory
// cover), moved right by middle_shift. The game may place it again (the UI
// is built in steps); whatever x it has that isn't the one we set is the
// game's, and the shift goes on top of that.
struct Moved {
    void* layout;
    int point;
    float base;     // the game's x
    float applied;  // the x we set
};

// A tile of the console's bottom row (ConsoleUI.fdf), moved far off the
// screen while the pieces replace it (neither a width of 0 nor clearing its
// points stopped it drawing where it was). `anchor` is its point as the game
// made it, put back at 4:3.
constexpr float kAway = -10.0f;
struct Tile {
    void* layout;
    int point;
    Anchor anchor;
};

// One game UI's state. A game's UI may be made where the last one was, so
// it's the same UI only within one map load (natives::map_load_count):
// otherwise the old UI's console, tiles and pieces (freed with it) would
// be used.
struct Laid {
    void* ui = nullptr;
    unsigned map_loads = 0;
    void* console = nullptr;
    float wide = 0;       // the layout of the pieces applied (0: none yet)
    float root_wide = 0;  // the width the root was last given (0: none yet)
    std::vector<Tile> tiles;
    std::vector<Moved> moved;
    std::unordered_map<std::string, std::vector<void*>> pieces;  // made so far, by FDF name
    int next_context = 0;
    bool pieces_shown = false;
    int tile_scans = 0;          // frames find_tiles found no tiles in
    bool custom_console = false; // gave up: not the game's own console art
};
Laid g_laid;

void call_slot(void* object, uint32_t slot) {
    auto vtable = field<void**>(object, 0);
    reinterpret_cast<VoidMethod>(vtable[slot / sizeof(void*)])(object);
}

void show_simple(void* frame, bool show) {
    const int flag = show ? 1 : 0;
    std::memcpy(static_cast<char*>(frame) + g_ui.show_flag, &flag, sizeof flag);
    call_slot(frame, show ? g_ui.show_slot : g_ui.hide_slot);
}

bool about(float v, float want) {
    return v > want - 0.0005f && v < want + 0.0005f;
}

// The console's bottom tiles, by their points and sizes as ConsoleUI.fdf
// makes them: 0.256 or 0.032 wide, 0.176 or 0.15 high, at the bottom.
void find_tiles() {
    void* console = g_laid.console;  // a simple frame: its layout is the frame itself
    std::vector<Tile> tiles;
    for (char* node = field<char*>(console, g_ui.dependents); reinterpret_cast<intptr_t>(node) > 0;
         node = field<char*>(node, 8)) {
        void* layout = field<void*>(node, 0xC);
        Anchor a;
        const bool bottom = read_anchor(layout, kBottomLeft, a) || read_anchor(layout, kBottomRight, a);
        if (!bottom || a.relative != console || a.y != 0.0f) continue;
        const float width = field<float>(layout, g_ui.width), height = field<float>(layout, g_ui.width + 4);
        if ((about(width, 0.256f) || about(width, 0.032f)) && (about(height, 0.176f) || about(height, 0.15f))) {
            tiles.push_back({layout, relative_to_frame(point_of(layout, kBottomLeft)) ? kBottomLeft : kBottomRight, a});
        }
    }
    if (tiles.size() == 4) g_laid.tiles = std::move(tiles);
}

// Follows one point of the middle: shifted by `shift` from the game's x.
void follow_point(void* layout, int point, float shift) {
    Anchor a;
    if (!read_anchor(layout, point, a)) return;
    auto it = g_laid.moved.begin();
    while (it != g_laid.moved.end() && (it->layout != layout || it->point != point)) ++it;
    if (it == g_laid.moved.end()) it = g_laid.moved.insert(it, {layout, point, a.x, a.x});
    if (!about(a.x, it->applied)) it->base = a.x;  // the game placed it again
    const float want = it->base + shift;
    if (about(a.x, want)) return;
    g_ui.set_point(layout, point, a.relative, a.relative_point, want, a.y, 1);
    it->applied = want;
}

// Which side of its relative frame a point is on: -1 left, 1 right, 0 the middle.
int side(int point) {
    switch (point) {
    case 0: case 3: case 6: return -1;  // TOPLEFT, LEFT, BOTTOMLEFT
    case 2: case 5: case 8: return 1;   // TOPRIGHT, RIGHT, BOTTOMRIGHT
    default: return 0;
    }
}

// Which bottom corner a point is: -1 BOTTOMLEFT, 1 BOTTOMRIGHT, 0 neither.
int bottom_side(int point) {
    return point == kBottomLeft ? -1 : point == kBottomRight ? 1 : 0;
}

bool is_ours(void* layout) {
    for (const Tile& t : g_laid.tiles) {
        if (t.layout == layout) return true;
    }
    for (auto& [name, made] : g_laid.pieces) {
        for (void* frame : made) {
            if (frame == layout) return true;
        }
    }
    return false;
}

// The frames anchored to `container`'s bottom corners (it spans the whole
// UI): what the game put in the middle of a 4:3 screen (x from 0.19 to 0.61
// there) moves to the middle of this one, right by `shift` from the left
// corner or left by it from the right one. Frames held by both corners
// (they stretch, like the chat box) stay; so do the console's tiles and
// pieces, and the top bar (its tiles hang from the top corners). Frames the game
// makes later (the life and mana text, made with the first selection) are
// caught on the frame after.
constexpr float kMiddleFrom = 0.19f, kMiddleTo = 0.61f;
void follow_children(void* container, float shift) {
    if (!container) return;
    for (char* node = field<char*>(container, g_ui.dependents); reinterpret_cast<intptr_t>(node) > 0;
         node = field<char*>(node, 8)) {
        void* layout = field<void*>(node, 0xC);
        if (!layout || is_ours(layout)) continue;
        // Its points on the container's bottom corners: all of them move, so
        // a frame held by two points on one corner keeps its size.
        int sides = 0, count = 0;
        int on[9];
        for (int point = 0; point < 9; ++point) {
            Anchor a;
            if (!read_anchor(layout, point, a) || a.relative != container) continue;
            const int s = bottom_side(a.relative_point);
            if (s == 0) continue;
            sides |= s < 0 ? 1 : 2;
            on[count++] = point;
        }
        if (sides != 1 && sides != 2) continue;  // held by both sides, or by neither
        auto it = g_laid.moved.begin();
        while (it != g_laid.moved.end() && (it->layout != layout || it->point != on[0])) ++it;
        Anchor a;
        read_anchor(layout, on[0], a);
        const float base = it != g_laid.moved.end() && about(a.x, it->applied) ? it->base : a.x;
        const float at = sides == 1 ? base : widescreen::kBaseWidth + base;  // on a 4:3 screen
        if (at < kMiddleFrom || at > kMiddleTo) continue;
        for (int i = 0; i < count; ++i) follow_point(layout, on[i], sides == 1 ? shift : -shift);
    }
}

// The inventory's cover: the game puts its BOTTOMRIGHT at x 0.6 of the
// screen (a point relative to the screen, which read_anchor doesn't read).
// It goes on the console's bottom-left instead, the same place at 4:3.
constexpr float kCoverRight = 0.6f;
void follow_cover(void* cover, float shift) {
    Anchor a;
    const bool ours = read_anchor(cover, kBottomRight, a) && a.relative == g_laid.console;
    const float want = kCoverRight + shift;
    if (ours && about(a.x, want)) return;
    g_ui.clear_points(cover);
    g_ui.set_point(cover, kBottomRight, g_laid.console, kBottomLeft, want, 0.0f, 1);
}

// The n-th piece made from `name` for this UI, made if needed.
void* piece(const std::string& name, size_t n) {
    std::vector<void*>& made = g_laid.pieces[name];
    while (made.size() <= n) {
        void* frame = g_ui.create_simple_frame(name.c_str(), g_laid.console, 0x7E00 + g_laid.next_context++);
        if (!frame) {
            TOME_WARN("widescreen: couldn't make the console piece %s", name.c_str());
            return nullptr;
        }
        made.push_back(frame);
    }
    return made[n];
}

// Lays the pieces out for a UI `wide` units across (none at 4:3).
void lay_pieces(float wide) {
    g_laid.wide = wide;
    std::unordered_map<std::string, size_t> used;
    if (wide > widescreen::kBaseWidth) {
        for (const widescreen::Piece& p : widescreen::console_layout(wide)) {
            void* frame = piece(p.name, used[p.name]++);
            if (!frame) continue;
            g_ui.set_width(frame, p.width);
            g_ui.set_height(frame, p.height);
            // A new simple frame fills its parent: only this point may hold it.
            const int point = p.top ? kTopLeft : kBottomLeft;
            g_ui.clear_points(frame);
            g_ui.set_point(frame, point, g_laid.console, point, p.x, 0.0f, 1);
        }
    }
    // Pieces left over from a wider window go off the screen, like the tiles
    // (at 4:3 all of them are hidden with the rest, in follow_ui).
    for (auto& [name, made] : g_laid.pieces) {
        for (size_t i = used[name]; i < made.size(); ++i) {
            g_ui.clear_points(made[i]);
            g_ui.set_point(made[i], kBottomLeft, g_laid.console, kBottomLeft, kAway, 0.0f, 1);
        }
    }
}

// Once per frame in a game (from the world camera's setup): lays the game
// UI out for the window's width, and keeps it so.
void follow_ui() {
    void* ui = g_game_ui_get(0, 0);
    if (!ui) return;
    const unsigned map_loads = natives::map_load_count();
    if (ui != g_laid.ui || map_loads != g_laid.map_loads) {
        g_laid = {};
        g_laid.ui = ui;
        g_laid.map_loads = map_loads;
    }
    if (!g_laid.console) g_laid.console = field<void*>(ui, g_ui.console);
    if (!g_laid.console) return;
    const float wide = ui_width();
    const bool wider = wide > widescreen::kBaseWidth;
    const float shift = widescreen::middle_shift(wide);
    // The game UI's root takes the screen's width when it's made and when
    // the window changes size; after the setting changes, it has to again.
    if (g_laid.root_wide != wide) {
        const bool first = g_laid.root_wide == 0;  // made at this width
        g_laid.root_wide = wide;
        if (!first) call_slot(static_cast<char*>(ui) + g_ui.frame_layout, g_ui.root_update_slot);
    }

    // The tiles first: the middle's frames are found apart from them, and
    // the middle moves only with the art. A console whose tiles aren't the
    // game's (a map's own ConsoleUI.fdf) stays as the map made it, its
    // middle too, and isn't looked through again.
    constexpr int kTileScans = 30;
    if (g_laid.tiles.empty() && !g_laid.custom_console) {
        find_tiles();
        if (g_laid.tiles.empty() && ++g_laid.tile_scans >= kTileScans) {
            g_laid.custom_console = true;
            TOME_INFO("widescreen: the console's art isn't the game's own (a map's ConsoleUI.fdf?); "
                      "it and the console's middle stay where the map put them");
        }
    }
    if (g_laid.tiles.empty()) return;
    // The middle of the console, the top bar and the portrait's and
    // minimap's layers (full-screen CFrames: their layout at CFrame.layout).
    char* portrait_layer = field<char*>(ui, g_ui.portrait_layer);
    char* minimap_layer = field<char*>(ui, g_ui.minimap_layer);
    follow_children(g_laid.console, shift);
    follow_children(field<void*>(ui, g_ui.console_upper), shift);
    if (portrait_layer) follow_children(portrait_layer + g_ui.frame_layout, shift);
    if (minimap_layer) follow_children(minimap_layer + g_ui.frame_layout, shift);
    // The clock's model is drawn from its frame's corner at 0, 0 (the model
    // itself sits at the top middle): it moves like the middle.
    if (char* clock = field<char*>(ui, g_ui.clock)) follow_point(clock + g_ui.frame_layout, kBottomLeft, shift);
    // Two strings hang off the resource bar's right edge but sit left of it,
    // under the clock (0.417 and 0.361 from the right, the bar 0.338 wide).
    if (void* bar = field<void*>(ui, g_ui.resource_bar)) {
        for (char* node = field<char*>(bar, g_ui.dependents); reinterpret_cast<intptr_t>(node) > 0;
             node = field<char*>(node, 8)) {
            void* layout = field<void*>(node, 0xC);
            Anchor a;
            if (!read_anchor(layout, kTopLeft, a) || a.relative != bar || side(a.relative_point) != 1) continue;
            auto it = g_laid.moved.begin();
            while (it != g_laid.moved.end() && (it->layout != layout || it->point != kTopLeft)) ++it;
            const float base = it != g_laid.moved.end() && about(a.x, it->applied) ? it->base : a.x;
            if (base < -0.35f) follow_point(layout, kTopLeft, -shift);
        }
    }
    // The inventory's cover is placed on the screen, not on the info panel.
    if (void* info = field<void*>(ui, g_ui.info_panel)) {
        if (void* cover = field<void*>(info, g_ui.inventory_cover)) follow_cover(cover, shift);
    }

    for (Tile& t : g_laid.tiles) {
        Anchor now;
        if (!read_anchor(t.layout, t.point, now)) continue;
        const bool away = about(now.x, t.anchor.x + kAway);
        if (!away && !about(now.x, t.anchor.x)) t.anchor = now;  // the game placed it again
        if (wider == away) continue;
        g_ui.set_point(t.layout, t.point, t.anchor.relative, t.anchor.relative_point,
                       t.anchor.x + (wider ? kAway : 0.0f), t.anchor.y, 1);
    }
    // Laid out again: every piece is shown or hidden again below, the new
    // ones too.
    const bool relaid = wide != g_laid.wide;
    if (relaid) lay_pieces(wide);
    // With the console: it's hidden in cinematics, and Hide doesn't reach a
    // simple frame's children.
    const bool show = wider && field<int>(g_laid.console, g_ui.shown) != 0;
    if (show == g_laid.pieces_shown && !relaid) return;
    g_laid.pieces_shown = show;
    for (auto& [name, made] : g_laid.pieces) {
        for (void* frame : made) show_simple(frame, show);
    }
}

// --- the menus ------------------------------------------------------------------------

using GlueGetFn = void*(*)();
using GetCurrentFn = void*(*)();
GlueGetFn g_glue_get = nullptr;
GetCurrentFn g_current_screen = nullptr;

void update_root(void* frame) {
    if (frame) call_slot(static_cast<char*>(frame) + g_ui.frame_layout, g_ui.root_update_slot);
}

// The loading screen and the score screen are laid out at 4:3 in the middle
// of the window, in proportion: each is one piece of art (a full-screen
// model made for 0.8 x 0.6) with frames placed on it, which a stretched art
// leaves off their places (the loading screen's minimap preview off its
// border). The screen's frame (CGlueMgr.screen, which its frames hang from)
// is held by its corners `box_margin` in from the window's sides, and so are
// its full-screen models drawn flat (the loading and score screens' art sits
// in the screen's frame, so it's in the box with it). The root
// stays the window (the mouse, the cursor, and the game's UI, which is made
// under the loading screen). Latched with the setting when a screen is made.
//
// Frames come and go with screens (a screen's models may be freed before
// the next screen is made), so Tome changes one only while the game uses it:
// a model while its camera is set up (box_sprite, on_ortho), the screen's
// frame while it's the current one (box_screen). A frame it boxed is
// remembered by address and taken for the same one only if its corners
// still hold it where Tome put them (holds_box). Boxed frames stay boxed and
// go with their screen; a model is put back if it's drawn when the box is
// gone (the setting turned off).
bool g_boxed = false;
// In a game: the world was drawn since the last SetScreen. Its full-screen
// models are stretched as before, not put in proportion as menus' panels.
bool g_in_game = false;
uint32_t g_screen_loading = 0, g_screen_score = 0, g_glue_screen_offset = 0;
uint32_t g_glue_top_left = 0, g_glue_top_right = 0;  // CGlueMgr.sprite_top_left, _top_right

float box_margin() {
    return g_boxed ? widescreen::box_margin(g_conversion_width) : 0.0f;
}

struct Boxed {
    char* layout = nullptr;
    void* relative = nullptr;  // the frame it filled
    float margin = 0;          // as held now
};
std::vector<Boxed> g_boxes;
constexpr size_t kMaxBoxes = 64;  // the oldest are forgotten (their screens are long gone)

// Whether a (live) frame's corners are where Tome put them for `b`.
bool holds_box(const Boxed& b) {
    Anchor top_left, bottom_right;
    return read_anchor(b.layout, kTopLeft, top_left) && read_anchor(b.layout, kBottomRight, bottom_right) &&
           top_left.relative == b.relative && bottom_right.relative == b.relative &&
           top_left.relative_point == kTopLeft && bottom_right.relative_point == kBottomRight &&
           about(top_left.x, b.margin) && about(bottom_right.x, -b.margin) && top_left.y == 0.0f &&
           bottom_right.y == 0.0f;
}

// Holds a (live) frame by its corners, `margin` in from the sides of the
// frame it filled. A point keeps its offset multiplied by the frame's scale
// (CLayoutFrame.points), so it's given margin / scale: what it holds, and
// what holds_box reads back, is the margin.
void hold(Boxed& b, float margin) {
    if (about(margin, b.margin)) return;
    b.margin = margin;
    const float scale = field<float>(b.layout, g_ui.scale);
    const float offset = scale > 0.0f ? margin / scale : margin;
    g_ui.clear_points(b.layout);
    g_ui.set_point(b.layout, kTopLeft, b.relative, kTopLeft, offset, 0.0f, 1);
    g_ui.set_point(b.layout, kBottomRight, b.relative, kBottomRight, -offset, 0.0f, 1);
}

// The box Tome put a live frame (its layout) in, or null.
Boxed* boxed(char* layout) {
    for (auto it = g_boxes.begin(); it != g_boxes.end(); ++it) {
        if (it->layout != layout) continue;
        if (holds_box(*it)) return &*it;
        g_boxes.erase(it);  // another frame at the same address
        return nullptr;
    }
    return nullptr;
}

// Boxes a live frame (its layout) the game made to fill its parent: TOPLEFT
// and BOTTOMRIGHT on the same frame's, at 0, 0. Null if it isn't one.
Boxed* box_frame(char* layout) {
    if (Boxed* b = boxed(layout)) return b;
    Anchor top_left, bottom_right;
    if (!read_anchor(layout, kTopLeft, top_left) || !read_anchor(layout, kBottomRight, bottom_right) ||
        top_left.relative != bottom_right.relative || top_left.relative_point != kTopLeft ||
        bottom_right.relative_point != kBottomRight || top_left.x != 0.0f || top_left.y != 0.0f ||
        bottom_right.x != 0.0f || bottom_right.y != 0.0f) {
        return nullptr;
    }
    if (g_boxes.size() >= kMaxBoxes) g_boxes.erase(g_boxes.begin());
    g_boxes.push_back({layout, top_left.relative, 0.0f});
    hold(g_boxes.back(), box_margin());
    return &g_boxes.back();
}

// Boxes a full-screen model drawn flat (a live sprite frame): in the box it's
// in proportion.
void box_sprite(char* frame) {
    box_frame(frame + g_ui.frame_layout);
}

// A boxed model being drawn (its camera set up): to the box's margin now, or
// put back as the game made it when there's no box. The margin its camera
// moves by (0: not boxed).
float follow_sprite(char* frame) {
    Boxed* b = boxed(frame + g_ui.frame_layout);
    if (!b) return 0.0f;
    const float margin = box_margin();
    hold(*b, margin);
    if (margin > 0.0f) return margin;
    g_boxes.erase(g_boxes.begin() + (b - g_boxes.data()));
    return 0.0f;
}

// The current screen's frame into the box, or to its margin now (the
// window's size may have changed).
void box_screen() {
    void* glue = g_glue_get ? g_glue_get() : nullptr;
    void* screen = glue ? field<void*>(glue, g_glue_screen_offset) : nullptr;
    if (!screen) return;
    char* layout = static_cast<char*>(screen) + g_ui.frame_layout;
    const float margin = box_margin();
    if (Boxed* b = boxed(layout)) {
        hold(*b, margin);
    } else if (margin > 0.0f) {
        box_frame(layout);
    }
}

// --- warped and sliced art --------------------------------------------------------------

// Menu art that isn't drawn in proportion (core/widescreen.h): warped
// (widescreen::kWarpedFrames, and the highlight frame that dims a screen
// behind a dialog) is stretched across the window; sliced
// (widescreen::kSlicedFrames) is drawn in pieces (draw_sliced, on_ortho).
// The named frames are looked up once a frame (events::frames): frames come
// and go with screens.
using GetFrameByNameFn = void*(__fastcall*)(const char* name, int context);
GetFrameByNameFn g_frame_by_name = nullptr;
uintptr_t g_highlight_vtable = 0;  // 0: not mapped
constexpr size_t kWarpedCount = std::size(widescreen::kWarpedFrames);
constexpr size_t kSlicedCount = std::size(widescreen::kSlicedFrames);
struct Named {
    uint32_t presented = 0;  // the frame they were looked up in
    bool looked = false;
    void* warped[kWarpedCount] = {};
    void* sliced[kSlicedCount] = {};
};
Named g_named;

const Named& named() {
    const uint32_t presented = events::frames();
    if (!g_named.looked || g_named.presented != presented) {
        g_named.looked = true;
        g_named.presented = presented;
        for (size_t i = 0; i < kWarpedCount; ++i) g_named.warped[i] = g_frame_by_name(widescreen::kWarpedFrames[i], 0);
        for (size_t i = 0; i < kSlicedCount; ++i) g_named.sliced[i] = g_frame_by_name(widescreen::kSlicedFrames[i].name, 0);
    }
    return g_named;
}

bool warped(char* frame) {
    if (g_highlight_vtable && field<uintptr_t>(frame, 0) == g_highlight_vtable) return true;
    for (void* f : named().warped) {
        if (f == frame) return true;
    }
    return false;
}

const widescreen::SlicedFrame* sliced(char* frame) {
    const Named& n = named();
    for (size_t i = 0; i < kSlicedCount; ++i) {
        if (n.sliced[i] == frame) return &widescreen::kSlicedFrames[i];
    }
    return nullptr;
}

// Beside a boxed screen's art (a full-screen model in the box), the same art
// is drawn stretched across the window, blurred and darkened, so the
// window's sides carry its colors instead of black. Before the game renders
// the model, Tome renders it into each side with a camera that stretches it
// across the window (on_ortho, g_fill_pass), many times over, shifted and
// at a fraction of its alpha (the blur); then the game renders it as it
// does, in the box. One model a frame gets the copy (the score screen has
// three in the box).
using GetRectFn = int(__thiscall*)(void* layout, float* rect);
using SetViewportFn = void(__stdcall*)(float x0, float x1, float y0, float y1, float z0, float z1);
struct Fill {
    VoidMethod setup_camera = nullptr;  // CSpriteFrame::SetupCamera
    uint32_t render = 0;             // CSpriteFrame.vtable.Render
    uint32_t alpha = 0;              // CFrame.alpha
    uint32_t apply_alpha = 0;        // CFrame.vtable.ApplyAlpha
    uint32_t get_rect = 0;           // CLayoutFrame.vtable.GetRect
    SetViewportFn set_viewport = nullptr;  // GxXformSetViewport
    char** device = nullptr;               // GxDevice.instance
    uint32_t viewport = 0;                 // CGxDevice.viewport
};
Fill g_fill;
bool g_fill_pass = false;       // the stretched render is running (main thread)
// A piece of sliced art being rendered (draw_sliced): the art from
// g_piece_from to g_piece_to, in the viewport set for it.
bool g_piece_pass = false;
float g_piece_from = 0, g_piece_to = widescreen::kBaseWidth;
float g_fill_dx = 0, g_fill_dy = 0;  // the shift of the copy being rendered (UI units)
float g_fill_from = 0, g_fill_to = 1;  // the part of the window it's rendered in (the viewport, 0..1)
// The frame (events::frames) whose copy is drawn: only the first model in
// the box drawn in a frame (the bottom layer) gets one.
uint32_t g_fill_presented = 0;
bool g_fill_done = false;
// The copy is blurred: rendered kBlurSamples times, shifted across a disc
// kBlurRadius across (UI units; 0.04 is about 55 pixels of a 720-pixel-high
// window), averaged, at kFillBrightness of the art's own brightness.
constexpr int kBlurSamples = 24;
constexpr float kBlurRadius = 0.04f;
constexpr float kFillBrightness = 0.4f;

// Whether a live frame's rect is the box: a boxed screen's art.
bool fills_box(char* layout) {
    const float margin = box_margin();
    if (margin == 0.0f) return false;
    float rect[4] = {0, 0, 0, 0};  // {bottom, left, top, right}
    reinterpret_cast<GetRectFn>(field<void**>(layout, 0)[g_fill.get_rect / sizeof(void*)])(layout, rect);
    return about(rect[1], margin) && about(rect[3], margin + widescreen::kBaseWidth) && about(rect[0], 0.0f) &&
           about(rect[2], 0.6f);
}

// BEFORE CSpriteFrame::Render and CModelFrame::Render(this): the stretched,
// darkened copy of a boxed screen's art, rendered first. The game's own
// render follows with the camera set up again as it was.
// A sliced full-screen model (on a menu, its camera stretched as the game's)
// rendered in pieces, each in its part of the window (the viewport) with a
// camera on its part of the art (on_ortho). True if it was: the game's own
// render is skipped.
bool draw_sliced(char* frame) {
    if (g_boxed) return false;
    // The width first: at 4:3 or with the setting off, no frame lookups.
    const float wide = ui_width();
    if (wide <= widescreen::kBaseWidth) return false;
    const widescreen::SlicedFrame* s = sliced(frame);
    if (!s) return false;
    char* device = *g_fill.device;
    if (!device) return false;
    float viewport[6];
    std::memcpy(viewport, device + g_fill.viewport, sizeof viewport);
    g_piece_pass = true;
    for (const widescreen::ArtPiece& piece : widescreen::slice_art(wide, s->cut, s->tile)) {
        g_piece_from = piece.art_from;
        g_piece_to = piece.art_to;
        g_fill.set_viewport(piece.at_from / wide, piece.at_to / wide, viewport[2], viewport[3], viewport[4], viewport[5]);
        g_fill.setup_camera(frame);
        call_slot(frame, g_fill.render);
    }
    g_piece_pass = false;
    g_fill.set_viewport(viewport[0], viewport[1], viewport[2], viewport[3], viewport[4], viewport[5]);
    g_fill.setup_camera(frame);
    return true;
}

void TOME_CALL on_sprite_render(tome_mod*, tome_hook_call* call, void*) {
    if (g_fill_pass || g_piece_pass || call->arg_count < 1 || !call->args[0]) return;
    char* frame = reinterpret_cast<char*>(call->args[0]);
    if (draw_sliced(frame)) {
        call->skip_original = 1;
        return;
    }
    const uint32_t presented = events::frames();
    if (g_fill_done && presented == g_fill_presented) return;
    if (!fills_box(frame + g_ui.frame_layout)) return;
    g_fill_done = true;
    g_fill_presented = presented;
    const uint8_t alpha = field<uint8_t>(frame, g_fill.alpha);
    // Only in the window's sides, outside the box: the viewport is set to
    // each side in turn, and the camera to that side's part of the
    // stretched picture (on_ortho), so nothing in the box is drawn over (the
    // score screen draws some of its art before its layers in the box).
    // Each copy is shifted to a point of a disc and blended over the ones
    // before, so the result is kFillBrightness times their plain average
    // (widescreen::blur_samples).
    char* device = *g_fill.device;
    if (!device) return;
    float viewport[6];
    std::memcpy(viewport, device + g_fill.viewport, sizeof viewport);
    const float side = box_margin() / g_conversion_width;  // a side's width, 0..1
    const float sides[2][2] = {{0.0f, side}, {1.0f - side, 1.0f}};
    static const std::vector<widescreen::BlurSample> samples =
        widescreen::blur_samples(kBlurSamples, kBlurRadius, kFillBrightness);
    g_fill_pass = true;
    for (const auto& part : sides) {
        g_fill_from = part[0];
        g_fill_to = part[1];
        g_fill.set_viewport(part[0], part[1], viewport[2], viewport[3], viewport[4], viewport[5]);
        for (const widescreen::BlurSample& sample : samples) {
            g_fill_dx = sample.dx;
            g_fill_dy = sample.dy;
            g_fill.setup_camera(frame);
            const uint8_t pass_alpha = static_cast<uint8_t>(std::lround(sample.alpha * alpha));
            std::memcpy(frame + g_fill.alpha, &pass_alpha, 1);
            call_slot(frame, g_fill.apply_alpha);
            call_slot(frame, g_fill.render);
        }
    }
    g_fill.set_viewport(viewport[0], viewport[1], viewport[2], viewport[3], viewport[4], viewport[5]);
    g_fill_from = 0.0f;
    g_fill_to = 1.0f;
    g_fill_dx = g_fill_dy = 0.0f;
    std::memcpy(frame + g_fill.alpha, &alpha, 1);
    call_slot(frame, g_fill.apply_alpha);
    g_fill_pass = false;
    g_fill.setup_camera(frame);
}

// BEFORE and AFTER CGlueMgr::SetScreen(screen, force): the setting is
// latched for the new screen, and the menus' root and the screen frame the
// new screen is laid out on (the loading screen's own) take the screen's
// width again, in case it changed since they last did. After, the new
// screen is boxed.
void TOME_CALL on_set_screen(tome_mod*, tome_hook_call*, void*) {
    g_wide_setting = tome_settings::widescreen();
    refresh_width();
    update_root(g_glue_get());
    update_root(g_current_screen());
}

void TOME_CALL on_set_screen_before(tome_mod* mod, tome_hook_call* call, void* user) {
    if (call->arg_count >= 1) {
        const uint32_t screen = call->args[0];
        g_boxed = screen == g_screen_loading || screen == g_screen_score;
    }
    g_in_game = false;
    g_fill_done = false;
    on_set_screen(mod, call, user);
}

void TOME_CALL on_set_screen_after(tome_mod* mod, tome_hook_call* call, void* user) {
    on_set_screen(mod, call, user);
    box_screen();
}

// --- the 3D view -------------------------------------------------------------------

// BEFORE and AFTER WorldView::SetupCamera(this, delta, rect).
void TOME_CALL on_world_camera_enter(tome_mod*, tome_hook_call*, void*) {
    ++g_in_world_camera;
    // The world is drawn only in a game: the loading screen is gone, and
    // nothing calls SetScreen until the score screen. Its box goes with it,
    // so the game's own full-screen models aren't taken for its art.
    g_boxed = false;
    g_in_game = true;
    if (g_wide_ui) follow_ui();
}

void TOME_CALL on_world_camera_leave(tome_mod*, tome_hook_call*, void*) {
    if (g_in_world_camera > 0) --g_in_world_camera;
}

// Whether a CSpriteFrame::SetupCamera call is the world frame's.
bool is_world_frame(tome_hook_call* call) {
    if (call->arg_count < 1 || !call->args[0]) return false;
    void* game_ui = g_game_ui_get(0, 0);
    return game_ui && *reinterpret_cast<void**>(static_cast<char*>(game_ui) + g_world_frame_offset) ==
                          reinterpret_cast<void*>(call->args[0]);
}

// BEFORE and AFTER CSpriteFrame::SetupCamera(this): UI models' cameras and
// the world frame's (unit picking).
// Whether a sprite frame fills the screen: its TOPLEFT and BOTTOMRIGHT on
// its parent's, both at 0, 0, and so up to a frame with no points (a screen
// frame, its rect set directly). A menu screen's background is one, a model
// made for the whole 0.8 x 0.6 screen; placed models (the logo, a button's
// face, portraits) have their own extent. Only known once the 2D part is
// installed.
// A point relative to the screen holds an x where this reads the frame:
// such a frame isn't taken to fill anything.
uint32_t g_frame_layout = 0, g_points = 0;
bool fills_screen(const char* layout, int depth) {
    if (depth > 8) return false;
    const char* corner[2];
    std::memcpy(&corner[0], layout + g_points + 4 * 0, sizeof corner[0]);  // TOPLEFT
    std::memcpy(&corner[1], layout + g_points + 4 * 8, sizeof corner[1]);  // BOTTOMRIGHT
    if (!corner[0] && !corner[1]) {
        for (int point = 1; point < 8; ++point) {
            const char* p;
            std::memcpy(&p, layout + g_points + 4 * point, sizeof p);
            if (p) return false;
        }
        return true;  // no points: the root
    }
    if (!relative_to_frame(corner[0]) || !relative_to_frame(corner[1])) return false;
    const char* relative[2];
    for (int i = 0; i < 2; ++i) {
        int relative_point;
        float x, y;
        std::memcpy(&relative[i], corner[i] + 4, sizeof relative[i]);
        std::memcpy(&relative_point, corner[i] + 8, sizeof relative_point);
        std::memcpy(&x, corner[i] + 0xC, sizeof x);
        std::memcpy(&y, corner[i] + 0x10, sizeof y);
        if (relative_point != (i == 0 ? 0 : 8) || x != 0.0f || y != 0.0f) return false;
    }
    return relative[0] && relative[0] == relative[1] && fills_screen(relative[0], depth + 1);
}

// A model made for the whole screen: a sprite frame that fills it, or whose
// every point is on a frame that does, at x 0 (the loading screen's bar,
// held by its bottom-left corner only). Not the cursor: its frame fills the
// screen too, but its model is drawn at the pointer, which is in the
// window's units like the rest of the UI. Stretched, it was drawn wide / 0.8
// times as far from the left edge as the pointer (a third at 16:9), so
// clicks landed left of it.
uintptr_t g_cursor_vtable = 0;
bool fills_parent(tome_hook_call* call) {
    if (!g_points || call->arg_count < 1 || !call->args[0]) return false;
    if (*reinterpret_cast<const uintptr_t*>(call->args[0]) == g_cursor_vtable) return false;
    const char* layout = reinterpret_cast<const char*>(call->args[0]) + g_frame_layout;
    if (fills_screen(layout, 0)) return true;
    int points = 0;
    for (int point = 0; point < 9; ++point) {
        const char* p;
        std::memcpy(&p, layout + g_points + 4 * point, sizeof p);
        if (!p) continue;
        if (!relative_to_frame(p)) return false;
        const char* relative;
        float x;
        std::memcpy(&relative, p + 4, sizeof relative);
        std::memcpy(&x, p + 0xC, sizeof x);
        if (x != 0.0f || !relative || !fills_screen(relative, 1)) return false;
        ++points;
    }
    return points > 0;
}

void TOME_CALL on_sprite_camera_enter(tome_mod*, tome_hook_call* call, void*) {
    const Sprite kind = is_world_frame(call)        ? Sprite::World
                        : fills_parent(call)          ? Sprite::Screen
                                                      : Sprite::Model;
    g_sprite_cameras.push_back({kind, reinterpret_cast<char*>(call->args[0])});
    if (kind == Sprite::World) ++g_in_world_camera;
}

void TOME_CALL on_sprite_camera_leave(tome_mod*, tome_hook_call*, void*) {
    if (g_sprite_cameras.empty()) return;
    const Sprite kind = g_sprite_cameras.back().kind;
    g_sprite_cameras.pop_back();
    if (kind == Sprite::World && g_in_world_camera > 0) --g_in_world_camera;
}

// BEFORE C44Matrix::Ortho(this, left, right, bottom, top, near, far): a
// full-screen model's flat camera looks from the middle of its view, which
// is as wide as the window: -wide/2 to wide/2. Its model is made for 0 to
// 0.8, so it would sit at the left, in proportion. It gets -wide/2 to
// 0.8 - wide/2 instead: the model stretched across the window, as the game
// draws it at 4:3 in any window, so the frames on it (a menu's buttons,
// the loading screen's) sit on its art as they always did.
void TOME_CALL on_ortho(tome_mod*, tome_hook_call* call, void*) {
    if (g_sprite_cameras.empty() || call->arg_count < 5) return;  // up to top (the fill's shift)
    // The fill behind a boxed screen's art: its camera, 0.8 across around
    // the same middle, stretches the model across the window.
    // A piece of sliced art: the camera's left edge (the art's 0, the
    // model stretched as the game draws it) moved to the piece's start, and
    // as wide as the piece.
    if (g_piece_pass) {
        const float left = tome_hook_arg_float(call, 1);
        tome_hook_set_arg_float(call, 1, left + g_piece_from);
        tome_hook_set_arg_float(call, 2, left + g_piece_to);
        return;
    }
    if (g_fill_pass) {
        const float middle = (tome_hook_arg_float(call, 1) + tome_hook_arg_float(call, 2)) / 2 + g_fill_dx;
        const float left = middle - widescreen::kBaseWidth / 2;  // the picture over the whole window
        tome_hook_set_arg_float(call, 1, left + g_fill_from * widescreen::kBaseWidth);  // its part in the viewport
        tome_hook_set_arg_float(call, 2, left + g_fill_to * widescreen::kBaseWidth);
        tome_hook_set_arg_float(call, 3, tome_hook_arg_float(call, 3) + g_fill_dy);
        tome_hook_set_arg_float(call, 4, tome_hook_arg_float(call, 4) + g_fill_dy);
        return;
    }
    // A boxed full-screen model: it's placed from the root's left edge, its
    // camera from its frame's, so the camera moves right by the box's margin.
    if (const float margin = follow_sprite(g_sprite_cameras.back().frame); margin > 0.0f) {
        tome_hook_set_arg_float(call, 1, tome_hook_arg_float(call, 1) + margin);
        tome_hook_set_arg_float(call, 2, tome_hook_arg_float(call, 2) + margin);
        return;
    }
    if (g_sprite_cameras.back().kind != Sprite::Screen) return;
    const float wide = ui_width();
    const float left = tome_hook_arg_float(call, 1), right = tome_hook_arg_float(call, 2);
    if (wide <= widescreen::kBaseWidth || !about(left, -wide / 2) || !about(right, wide / 2)) return;
    // Warped art: stretched across the window.
    if (warped(g_sprite_cameras.back().frame)) {
        tome_hook_set_arg_float(call, 2, widescreen::kBaseWidth - wide / 2);
        return;
    }
    // The menus' panels: the glue manager's sprite layers, each a model made
    // for 0.8 x 0.6 and drawn as it is (the camera leaves it at the left),
    // in proportion: the top-left layer (the left panels, the left side of
    // the border) on the window's left, the top-right one (the right panels
    // and border, behind the screen's buttons, which the game anchors to the
    // right) on its right. Another such model on a menu goes to the middle.
    // Not in a game: what's full-screen there is stretched across the window.
    if (!g_boxed && !g_in_game) {
        char* glue = static_cast<char*>(g_glue_get ? g_glue_get() : nullptr);
        char* frame = g_sprite_cameras.back().frame;
        if (glue && frame == field<char*>(glue, g_glue_top_left)) return;
        const float shift = glue && frame == field<char*>(glue, g_glue_top_right) ? wide - widescreen::kBaseWidth
                                                                                     : (wide - widescreen::kBaseWidth) / 2;
        tome_hook_set_arg_float(call, 1, left - shift);
        tome_hook_set_arg_float(call, 2, right - shift);
        return;
    }
    tome_hook_set_arg_float(call, 2, widescreen::kBaseWidth - wide / 2);
    // On a menu screen it goes into the box from the next frame: drawn flat
    // in a frame 0.8 wide, it's in proportion there (box_screen).
    if (box_margin() > 0.0f) box_sprite(g_sprite_cameras.back().frame);
}

// AFTER C44Matrix::Perspective(this, fov, aspect, near, far).
void TOME_CALL on_perspective(tome_mod*, tome_hook_call* call, void*) {
    if (g_in_world_camera == 0 || call->arg_count < 1 || !tome_settings::widescreen()) return;
    // A wide UI gives the world frame, and so its camera, the window's shape.
    if (ui_width() > widescreen::kBaseWidth) return;
    float* m = reinterpret_cast<float*>(call->args[0]);
    if (!m) return;
    int width = 0, height = 0;
    main_thread::client_size(width, height);
    widescreen::Scale s = widescreen::projection_scale(width, height);
    m[0] *= s.x;
    m[5] *= s.y;
}

// --- install -------------------------------------------------------------------------

hooks::Target* find_target(const char* name, std::string& report) {
    std::string error;
    hooks::Target* target = hooks::find_or_install(name, error);
    if (!target) report = std::string(name) + ": " + error;
    return target;
}

void add_handler(hooks::Target* target, int phase, tome_hook_fn fn) {
    hooks::Handler handler;
    handler.phase = phase;
    handler.fn = fn;
    hooks::add(target, handler);
}

// What the 2D part needs mapped, besides the patch's own symbols (bound in
// BuildMappings: widescreen, ui and game; the hooked ones found by name).
const char* const kUiSymbols[] = {
    "CLayoutFrame.vtable.UpdateScreenRect", "CGlueMgr::SetScreen", "CGlueMgr::Get", "CScreenFrame::GetCurrent",
    "C44Matrix::Ortho", "DDCWidth", "DDCToNDCRect", "NDCToDDCRect", "DDCToNDC", "NDCToDDC", "DDCToNDCWidth",
    "NDCToDDCWidth",
    "CreateSimpleFrame", "CLayoutFrame::SetPoint", "CLayoutFrame::SetWidth", "CLayoutFrame::SetHeight",
    "CLayoutFrame::ClearAllPoints", "CFrame.layout", "CLayoutFrame.points", "CLayoutFrame.relative_point_vtable",
    "CCursorFrame.vtable", "CGlueMgr.screen",
    "CGlueMgr.sprite_top_left", "CGlueMgr.sprite_top_right", "CSpriteFrame::Render", "CModelFrame::Render",
    "CSpriteFrame.vtable.Render", "CFrame.alpha", "CFrame.vtable.ApplyAlpha", "CLayoutFrame.vtable.GetRect",
    "GxXformSetViewport", "GxDevice.instance", "CGxDevice.viewport",
    "CLayoutFrame.dependents",
    "CLayoutFrame.width", "CLayoutFrame.scale", "CGameUI.console", "CGameUI.info_panel", "CGameUI.time_of_day",
    "CInfoPanel.inventory_cover", "CGameUI.console_upper", "CGameUI.portrait_layer", "CGameUI.minimap_layer",
    "CGameUI.resource_bar", "CSimpleFrame.shown", "CSimpleFrame.show_flag", "CSimpleFrame.vtable.Hide",
    "CSimpleFrame.vtable.Show",
};

// The six conversions' x operands (each reads DDCWidth once or twice),
// pointed at g_conversion_width, or its double for a double DDCWidth.
// Checks them all before changing any; false with the reason in `error`. A
// mod's hook on one still works: the operands are "??" in their mapped
// bytes, and a trampoline made later copies them as they are now.
bool point_conversions(uintptr_t base, const BuildMappings& build, std::string& error) {
    const uintptr_t constant = base + build.widescreen.ddc_width;
    // 0.8f as a float or widened to a double, compared as bits (no x87
    // excess precision).
    constexpr uint32_t kFloatBits = 0x3F4CCCCD;
    constexpr uint64_t kDoubleBits = 0x3FE99999A0000000ull;  // (double)0.8f, as 1.26a stores it
    const void* ours = nullptr;
    if (readable(constant, sizeof(uint64_t)) && field<uint64_t>(reinterpret_cast<const void*>(constant), 0) == kDoubleBits) {
        ours = &g_conversion_width_double;
    } else if (readable(constant, sizeof(uint32_t)) &&
               field<uint32_t>(reinterpret_cast<const void*>(constant), 0) == kFloatBits) {
        ours = &g_conversion_width;
    } else {
        error = "DDCWidth isn't 0.8 (a float or a double)";
        return false;
    }
    constexpr size_t kMaxBody = 0x60;  // the longest, 1.27b's Rect forms, are 0x47 bytes
    const char* const conversions[] = {"DDCToNDCRect", "NDCToDDCRect",  "DDCToNDC",
                                       "NDCToDDC",     "DDCToNDCWidth", "NDCToDDCWidth"};
    std::vector<uintptr_t> sites;
    for (const char* name : conversions) {
        const mappings::Symbol* sym = build.data->find(name);
        const uintptr_t code = base + sym->value;
        std::vector<size_t> at;
        if (!readable(code, kMaxBody) ||
            !widescreen::constant_operands(reinterpret_cast<const uint8_t*>(code), kMaxBody,
                                           static_cast<uint32_t>(constant), at) ||
            at.empty() || at.size() > 2) {
            error = std::string(name) + ": not the code it maps (" + std::to_string(at.size()) +
                    " operands read DDCWidth, expected 1 or 2)";
            return false;
        }
        for (size_t offset : at) sites.push_back(code + offset);
    }
    for (uintptr_t site : sites) {
        if (!patch_pointer(site, constant, ours, error)) return false;
    }
    return true;
}

// The window's new size (main thread). The menus' root and the current
// screen take its width now: they keep the one they were laid out for until
// told, and a menu laid out for the old width under conversions for the new
// one has its edge frames short of the window's edge and its full-screen
// models taken for something else (on_ortho checks their camera against the
// width): leaving fullscreen on Battle.net drew the right panel at its 4:3
// place, its edge a bar down the middle.
void on_size() {
    const float before = g_conversion_width;
    refresh_width();
    if (g_conversion_width != before) {
        update_root(g_glue_get());
        update_root(g_current_screen());
    }
    box_screen();
}

// The 2D part; false (with the reason in `missing`) if a symbol isn't mapped.
bool install_wide_ui(uintptr_t base, const BuildMappings& build, std::string& missing) {
    for (const char* name : kUiSymbols) {
        if (!build.data->find(name)) {
            missing = name;
            return false;
        }
    }
    const auto& w = build.widescreen;
    g_ui.set_point = reinterpret_cast<SetPointFn>(base + build.ui.layout_set_point);
    g_ui.set_width = reinterpret_cast<SetSizeFn>(base + build.ui.layout_set_width);
    g_ui.set_height = reinterpret_cast<SetSizeFn>(base + w.layout_set_height);
    g_ui.clear_points = reinterpret_cast<VoidMethod>(base + w.layout_clear_points);
    g_ui.create_simple_frame = reinterpret_cast<CreateSimpleFrameFn>(base + w.create_simple_frame);
    g_ui.frame_layout = build.ui.layout_offset;
    g_ui.points = w.layout_points_offset;
    g_ui.relative_point = base + w.relative_point_vtable;
    g_ui.dependents = w.layout_dependents_offset;
    g_ui.width = w.layout_width_offset;
    g_ui.scale = w.layout_scale_offset;
    g_ui.console = w.console_offset;
    g_ui.info_panel = w.info_panel_offset;
    g_ui.clock = w.clock_offset;
    g_ui.inventory_cover = w.inventory_cover_offset;
    g_ui.root_update_slot = w.layout_update_slot;
    g_ui.console_upper = w.console_upper_offset;
    g_ui.portrait_layer = w.portrait_layer_offset;
    g_ui.minimap_layer = w.minimap_layer_offset;
    g_ui.resource_bar = w.resource_bar_offset;
    g_ui.shown = w.simple_shown_offset;
    g_ui.show_flag = w.simple_show_flag_offset;
    g_ui.hide_slot = w.simple_hide_slot;
    g_ui.show_slot = w.simple_show_slot;

    hooks::Target* set_screen = find_target("CGlueMgr::SetScreen", missing);
    if (!set_screen) return false;
    hooks::Target* ortho = find_target("C44Matrix::Ortho", missing);
    if (!ortho) return false;
    hooks::Target* sprite_render = find_target("CSpriteFrame::Render", missing);
    if (!sprite_render) return false;
    hooks::Target* model_render = find_target("CModelFrame::Render", missing);
    if (!model_render) return false;
    if (!point_conversions(base, build, missing)) return false;
    add_handler(sprite_render, TOME_HOOK_BEFORE, on_sprite_render);
    add_handler(model_render, TOME_HOOK_BEFORE, on_sprite_render);
    g_screen_loading = build.game.screen_loading;
    g_glue_screen_offset = build.game.glue_screen_offset;
    g_glue_top_left = w.glue_top_left_offset;
    g_glue_top_right = w.glue_top_right_offset;
    g_screen_score = build.game.screen_score_screen;
    g_fill.setup_camera = reinterpret_cast<VoidMethod>(base + w.sprite_setup_camera);
    g_fill.render = w.sprite_render_slot;
    g_fill.alpha = w.frame_alpha_offset;
    g_fill.apply_alpha = w.frame_apply_alpha_slot;
    g_fill.get_rect = w.layout_get_rect_slot;
    g_fill.set_viewport = reinterpret_cast<SetViewportFn>(base + w.set_viewport);
    g_fill.device = reinterpret_cast<char**>(base + w.gx_device);
    g_fill.viewport = w.device_viewport_offset;
    add_handler(ortho, TOME_HOOK_BEFORE, on_ortho);
    g_frame_layout = g_ui.frame_layout;
    g_cursor_vtable = base + w.cursor_frame_vtable;
    g_highlight_vtable = w.highlight_frame_vtable ? base + w.highlight_frame_vtable : 0;
    g_frame_by_name = reinterpret_cast<GetFrameByNameFn>(base + build.ui.get_frame_by_name);
    g_points = g_ui.points;
    add_handler(set_screen, TOME_HOOK_BEFORE, on_set_screen_before);
    add_handler(set_screen, TOME_HOOK_AFTER, on_set_screen_after);
    g_current_screen = reinterpret_cast<GetCurrentFn>(base + w.screen_get_current);
    g_glue_get = reinterpret_cast<GlueGetFn>(base + build.game.glue_get);
    g_wide_setting = tome_settings::widescreen();
    g_wide_ui = true;
    refresh_width();
    main_thread::set_size_hook(on_size);
    return true;
}

bool install_widescreen(uintptr_t base, const BuildMappings& build, const Variant&, std::string& report) {
    // Every target first, so a failure leaves no handler behind.
    hooks::Target* camera = find_target("WorldView::SetupCamera", report);
    if (!camera) return false;
    hooks::Target* sprite_camera = find_target("CSpriteFrame::SetupCamera", report);
    if (!sprite_camera) return false;
    hooks::Target* perspective = find_target("C44Matrix::Perspective", report);
    if (!perspective) return false;
    g_game_ui_get = game_ui_get(base, build);
    g_world_frame_offset = build.widescreen.world_frame_offset;
    add_handler(camera, TOME_HOOK_BEFORE, on_world_camera_enter);
    add_handler(camera, TOME_HOOK_AFTER, on_world_camera_leave);
    add_handler(sprite_camera, TOME_HOOK_BEFORE, on_sprite_camera_enter);
    add_handler(sprite_camera, TOME_HOOK_AFTER, on_sprite_camera_leave);
    add_handler(perspective, TOME_HOOK_AFTER, on_perspective);
    std::string missing;
    if (install_wide_ui(base, build, missing)) {
        report = "keeps the 3D view's and the UI's proportions in any window shape";
    } else {
        report = "keeps the 3D view's proportions in any window shape (the UI stays stretched: " + missing + ")";
    }
    return true;
}

const char* const kSymbols[] = {"WorldView::SetupCamera", "CSpriteFrame::SetupCamera", "CGameUI.world_frame",
                                "C44Matrix::Perspective", nullptr};

} // namespace

extern const Patch kWidescreen = {
    "widescreen",
    "keeps the picture's proportions in a window that isn't 4:3, showing more at the sides (Tome > Settings)",
    {},  // the same on every build; the UI part where its symbols are mapped
    kSymbols,
    nullptr,  // nothing to protect: a mod may adjust the projection or the UI after it
    nullptr,  // always installed; the setting is checked every frame
    install_widescreen,
};

} // namespace tome::patches
