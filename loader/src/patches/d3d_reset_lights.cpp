/*
 * Direct3D lights across a device reset (docs/known-bugs.md, "Scene darker
 * after Alt+Tab (Direct3D, Wine)"; docs/reverse-engineering.md).
 *
 * In Direct3D fullscreen, Alt+Tab loses the device and the game resets it
 * when it comes back. Under Wine (wined3d), IDirect3DDevice9::Reset frees
 * every light but leaves the lights that were enabled holding their slots in
 * the stateblock's table of active lights (8 in fixed function):
 * wined3d_stateblock_reset clears the stateblock state, but the light table
 * lives outside it. When RestoreState enables the game's lights again,
 * wined3d finds the slots taken and refuses them ("Too many concurrently
 * active lights"). With 5 lights on before, 3 of them come back; every reset
 * loses more, and the scene gets darker each time (on the menu, the water and
 * the ice lose the blue light first).
 *
 * Mechanism: a BEFORE handler on CGxDeviceD3d::ReleaseForReset, which the
 * game calls with 0 right before Reset, disables lights 0-7 through
 * IDirect3DDevice9::LightEnable. Disabling frees a light's slot in wined3d,
 * so Reset has nothing to leak; RestoreState enables them again afterwards
 * as usual. The game's own light cache is cleared by ReleaseForReset itself,
 * so it doesn't go stale. The teardown calls it with 1 (release_textures)
 * right before releasing the device, so there's nothing to keep then.
 *
 * Only under Wine: the leak is wined3d's, and on Windows LightEnable on a
 * light that was never set would make the runtime create a default one.
 * Elsewhere the patch is skipped (`needed`).
 */
#include "patches.h"

#include "game_build.h"
#include "hooks.h"
#include "tome/tome.h"

#include <cstdint>
#include <string>

namespace tome::patches {
namespace {

// IDirect3DDevice9::LightEnable (vtable slot 53).
using LightEnableFn = long(__stdcall*)(void* device, uint32_t index, int enable);
constexpr size_t kLightEnableSlot = 53;
constexpr uint32_t kGameLights = 8;  // RestoreState sets lights 0-7

uint32_t g_device_offset = 0;

// BEFORE CGxDeviceD3d::ReleaseForReset(this, release_textures).
void TOME_CALL on_release_for_reset(tome_mod*, tome_hook_call* call, void*) {
    if (call->arg_count < 2) return;
    if (call->args[1] != 0) return;  // the teardown: the device goes away next
    const char* self = reinterpret_cast<const char*>(call->args[0]);
    if (!self) return;
    void* device = *reinterpret_cast<void* const*>(self + g_device_offset);
    if (!device) return;
    auto light_enable = reinterpret_cast<LightEnableFn>((*reinterpret_cast<void***>(device))[kLightEnableSlot]);
    for (uint32_t i = 0; i < kGameLights; ++i) light_enable(device, i, 0);
}

bool d3d_reset_lights_needed(std::string& report) {
    if (running_on_wine()) return true;
    report = "not needed outside Wine";
    return false;
}

bool install_d3d_reset_lights(uintptr_t, const BuildMappings& build, std::string& report) {
    g_device_offset = build.d3d.device_offset;
    std::string error;
    hooks::Target* release = hooks::find_or_install("CGxDeviceD3d::ReleaseForReset", error);
    if (!release) {
        report = "CGxDeviceD3d::ReleaseForReset: " + error;
        return false;
    }
    hooks::Handler before;
    before.phase = TOME_HOOK_BEFORE;
    before.fn = on_release_for_reset;
    hooks::add(release, before);
    report = "frees the Direct3D lights before a device reset";
    return true;
}

const char* const kSymbols[] = {"CGxDeviceD3d::ReleaseForReset", "CGxDeviceD3d.d3d_device", nullptr};

} // namespace

extern const Patch kD3dResetLights = {
    "d3d-reset-lights",
    "keeps the scene's lights after Alt+Tab in Direct3D fullscreen under Wine (a wined3d reset bug leaks them)",
    nullptr,  // any build that maps the symbols below
    kSymbols,
    nullptr,  // nothing to protect: a mod can't reopen a hole through it
    d3d_reset_lights_needed,
    install_d3d_reset_lights,
};

} // namespace tome::patches
