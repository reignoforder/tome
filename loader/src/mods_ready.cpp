// SPDX-License-Identifier: GPL-3.0-or-later
#include "mods_ready.h"

#include "hooks.h"
#include "log.h"
#include "tome/tome.h"

#include <windows.h>

#include <climits>

namespace tome::mods_ready {
namespace {

HANDLE g_ready = nullptr;
constexpr DWORD kTimeoutMs = 10000;

// BEFORE game.dll's AIL_open_digital_driver: the chain this call runs is
// fixed already, so a mod that wants the device patches Miles (or hooks
// something it calls) in tome_mod_load rather than hooking this import.
void TOME_CALL before_open_sound(tome_mod*, tome_hook_call*, void*) {
    wait("opening the sound device");
}

} // namespace

void init() {
    if (!g_ready) g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

void set() {
    if (g_ready) SetEvent(g_ready);
}

void wait(const char* what) {
    if (g_ready && WaitForSingleObject(g_ready, kTimeoutMs) == WAIT_TIMEOUT) {
        TOME_WARN("Mods still loading after %lu ms; %s anyway", kTimeoutMs, what);
        SetEvent(g_ready);  // don't stall anything later too
    }
}

bool install_sound_wait(std::string& report) {
    std::string error;
    hooks::Target* open = hooks::find_or_install("Import.AIL_open_digital_driver", error);
    if (!open) {
        report = "Import.AIL_open_digital_driver: " + error;
        return false;
    }
    hooks::Handler before;
    before.phase = TOME_HOOK_BEFORE;
    before.priority = INT32_MAX;  // before any mod's handler
    before.fn = before_open_sound;
    hooks::add(open, before);
    report = "the game opens its sound device after the first mod load";
    return true;
}

} // namespace tome::mods_ready
