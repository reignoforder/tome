// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Widescreen 3D view (docs/known-bugs.md, "Stretched picture on widescreen";
 * docs/reverse-engineering.md, "Camera projection").
 *
 * 1.27b builds every camera's projection from its viewport in the 0.8 x 0.6
 * UI space, so the world camera always gets aspect 4/3 whatever the window.
 * In a 16:9 window the 4:3 picture is stretched sideways: units and
 * buildings a third too wide, and no more of the map than at 4:3.
 *
 * Mechanism: an AFTER handler on C44Matrix::Perspective (the one projection
 * builder) scales the matrix's x term (m[0]) by (4/3) / (width/height) of
 * the game window's client area, so the height of the view stays and more
 * shows at the sides, as in 1.29. A window narrower than 4:3 gets its y term
 * (m[5]) scaled instead. The math is in core/widescreen.*. Picking the
 * ground goes through the same matrix (checked in game: a move order at 90%
 * of the width lands under the cursor), so it follows.
 *
 * Only the world camera: Perspective also builds the projections of 3D
 * models inside UI frames (portraits, the clock) and of the main menu's 3D
 * background, which sit in stretched 2D art and must stay stretched with it.
 * The world camera is the one set up by WorldView::SetupCamera, so BEFORE and
 * AFTER handlers there mark the calls in between. Unit picking builds its own
 * projection, through the world frame's CSpriteFrame::SetupCamera (the
 * generic one of UI models): left at 4:3, a unit's click area sat further
 * from the middle of the screen than the unit, and off screen near the
 * sides. BEFORE and AFTER handlers there mark the world frame's calls too
 * (CGameUI.world_frame). Everything here runs on the main thread (rendering
 * and input).
 *
 * The player can turn it off (Tome > Settings > Widescreen, tome_settings.h);
 * it applies from the next frame. The 2D UI (console, top bar, command card)
 * stays stretched either way: that needs the frame layout itself changed
 * (docs/research.md §11.4).
 */
#include "patches.h"

#include "core/widescreen.h"
#include "game_build.h"
#include "hooks.h"
#include "main_thread.h"
#include "tome/tome.h"
#include "tome_settings.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tome::patches {
namespace {

int g_in_world_camera = 0;  // world camera setups running (main thread)
// Per CSpriteFrame::SetupCamera running, innermost last: whether it's the
// world frame's, as its BEFORE handler found (main thread).
std::vector<bool> g_sprite_cameras;

GameUiGetFn g_game_ui_get = nullptr;
uint32_t g_world_frame_offset = 0;  // CGameUI.world_frame

// BEFORE and AFTER WorldView::SetupCamera(this, delta, rect).
void TOME_CALL on_world_camera_enter(tome_mod*, tome_hook_call*, void*) {
    ++g_in_world_camera;
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
void TOME_CALL on_sprite_camera_enter(tome_mod*, tome_hook_call* call, void*) {
    const bool world = is_world_frame(call);
    g_sprite_cameras.push_back(world);
    if (world) ++g_in_world_camera;
}

void TOME_CALL on_sprite_camera_leave(tome_mod*, tome_hook_call*, void*) {
    if (g_sprite_cameras.empty()) return;
    const bool world = g_sprite_cameras.back();
    g_sprite_cameras.pop_back();
    if (world && g_in_world_camera > 0) --g_in_world_camera;
}

// AFTER C44Matrix::Perspective(this, fov, aspect, near, far).
void TOME_CALL on_perspective(tome_mod*, tome_hook_call* call, void*) {
    if (g_in_world_camera == 0 || call->arg_count < 1 || !tome_settings::widescreen()) return;
    float* m = reinterpret_cast<float*>(call->args[0]);
    if (!m) return;
    int width = 0, height = 0;
    main_thread::client_size(width, height);
    widescreen::Scale s = widescreen::projection_scale(width, height);
    m[0] *= s.x;
    m[5] *= s.y;
}

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

bool install_widescreen(uintptr_t base, const BuildMappings& build, std::string& report) {
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
    report = "keeps the world view's proportions in any window shape";
    return true;
}

const char* const kSymbols[] = {"WorldView::SetupCamera", "CSpriteFrame::SetupCamera", "CGameUI.world_frame",
                                "C44Matrix::Perspective", nullptr};

} // namespace

extern const Patch kWidescreen = {
    "widescreen",
    "shows more of the map instead of stretching the 3D view in a window that isn't 4:3 (Tome > Settings)",
    nullptr,  // any build that maps the symbols below
    kSymbols,
    nullptr,  // nothing to protect: a mod may adjust the projection after it
    nullptr,  // always installed; the setting is checked every frame
    install_widescreen,
};

} // namespace tome::patches
