/*
 * OpenGL context after Alt+Tab (docs/known-bugs.md, "Under Wine";
 * docs/reverse-engineering.md).
 *
 * With the OpenGL renderer in fullscreen under Wine, leaving the game and
 * coming back leaves most textures gone: models, frames and text draw white
 * until the game restarts. On the way back the game releases its textures
 * and makes a new context itself, and Wine also replaces the driver context
 * behind an HGLRC when the window is restored (win32u_wgl_context_reset).
 * Textures uploaded in between are lost while the game still holds their
 * names, and the new context hands the same small names out again, so the
 * names stay "valid" (glIsTexture) and a check of them can't see the loss:
 * measured on the main menu, 29 of 118 textures came back and the rest
 * stayed white.
 *
 * Mechanism: an AFTER handler on CGxDeviceOpenGl::CanDraw (every frame, on
 * the main thread where the game's context is current). CanDraw is false
 * while the window is minimized; on the first frame it's true again, the
 * game's own DestroyContext (which deletes and forgets every texture) and
 * CreateContext (GL state setup) run, as when its video mode changes, so
 * everything uploads again into a context that stays. Ported from the
 * fps-unlock mod's first fix. When to rebuild is core/context_restore.*:
 * only if the device was fullscreen (CGxDevice.fullscreen) while hidden, and
 * a CreateContext that fails is tried again on the next frame, since the
 * game would otherwise draw with no context until it restarts.
 *
 * Only under Wine: the context replacement is Wine's, and on Windows the
 * rebuild would only cost a texture upload after each restore. Elsewhere
 * the patch is skipped (`needed`).
 */
#include "patches.h"

#include "core/context_restore.h"
#include "game_build.h"
#include "hooks.h"
#include "log.h"
#include "tome/tome.h"

#include <windows.h>

#include <cstdint>
#include <string>

#pragma GCC diagnostic ignored "-Wattributes"

namespace tome::patches {
namespace {

using DestroyContextFn = void(__thiscall*)(void* device);
using CreateContextFn = int(__thiscall*)(void* device);

DestroyContextFn g_destroy_context = nullptr;
CreateContextFn g_create_context = nullptr;
uint32_t g_fullscreen_offset = 0;

ContextRestore g_restore;  // main thread only

// AFTER CGxDeviceOpenGl::CanDraw(this) -> bool: false while minimized.
void TOME_CALL on_can_draw(tome_mod*, tome_hook_call* call, void*) {
    if (call->arg_count < 1) return;
    const char* device = reinterpret_cast<const char*>(call->args[0]);
    if (!device) return;
    const bool can_draw = (call->result & 0xFF) != 0;
    const bool fullscreen = *reinterpret_cast<const int32_t*>(device + g_fullscreen_offset) != 0;
    if (!g_restore.frame(can_draw, fullscreen)) return;
    // DestroyContext does nothing to a context that's already gone (a failed
    // CreateContext destroys its own), so a retry runs the same two calls.
    void* self = const_cast<char*>(device);
    g_destroy_context(self);
    const bool ok = g_create_context(self) != 0;
    const uint32_t failed_before = g_restore.failures();
    g_restore.rebuilt(ok);
    if (ok && failed_before == 0) {
        TOME_INFO("gl-context-restore: the window is back; made the OpenGL context again");
    } else if (ok) {
        TOME_INFO("gl-context-restore: made the OpenGL context again after %u failed tries", failed_before);
    } else if (failed_before == 0) {
        TOME_ERROR("gl-context-restore: the window is back, but the OpenGL context couldn't be made again; "
                   "trying again every frame");
    }
}

bool gl_context_restore_needed(std::string& report) {
    if (running_on_wine()) return true;
    report = "not needed outside Wine";
    return false;
}

bool install_gl_context_restore(uintptr_t base, const BuildMappings& build, std::string& report) {
    g_fullscreen_offset = build.gl.fullscreen_offset;
    g_destroy_context = reinterpret_cast<DestroyContextFn>(base + build.gl.destroy_context);
    g_create_context = reinterpret_cast<CreateContextFn>(base + build.gl.create_context);
    std::string error;
    hooks::Target* can_draw = hooks::find_or_install("CGxDeviceOpenGl::CanDraw", error);
    if (!can_draw) {
        report = "CGxDeviceOpenGl::CanDraw: " + error;
        return false;
    }
    hooks::Handler after;
    after.phase = TOME_HOOK_AFTER;
    after.fn = on_can_draw;
    hooks::add(can_draw, after);
    report = "makes the OpenGL context again when the window comes back (fullscreen Alt+Tab)";
    return true;
}

const char* const kSymbols[] = {"CGxDeviceOpenGl::CanDraw", "CGxDeviceOpenGl::DestroyContext",
                                "CGxDeviceOpenGl::CreateContext", "CGxDevice.fullscreen", nullptr};

} // namespace

extern const Patch kGlContextRestore = {
    "gl-context-restore",
    "rebuilds the OpenGL context after Alt+Tab in fullscreen under Wine, so textures come back",
    nullptr,  // any build that maps the symbols below
    kSymbols,
    nullptr,  // nothing to protect: a mod can't reopen a hole through it
    gl_context_restore_needed,
    install_gl_context_restore,
};

} // namespace tome::patches
