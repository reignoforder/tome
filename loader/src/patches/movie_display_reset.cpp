// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Movies under Wine (docs/known-bugs.md, "Under Wine";
 * docs/reverse-engineering.md, "Movies").
 *
 * Before a movie (the intro, the campaign cinematics: Movies\*.mpq through
 * DirectShow) and again after it, the player sets the display mode with
 * Movie::SetDisplayMode. In a window it puts the desktop's registry mode
 * back with ChangeDisplaySettingsA(NULL, 0) and, if that fails, tries again
 * every 50 ms until it doesn't. Wine can refuse it for good (an X server
 * that can't change modes, such as Xvfb: DISP_CHANGE_FAILED every time), and
 * the game then spins there forever on a black screen: before the movie's
 * window exists, so the movie never starts, and nothing ever comes back.
 *
 * Mechanism: the innermost REPLACE handler on Movie::SetDisplayMode
 * (Handler.last, so mods' BEFORE and REPLACE handlers still run first and
 * see the call) does
 * the windowed reset itself: nothing if the display is already in its
 * registry mode, otherwise the same retries, but it gives up after
 * kResetTries of them (about a second) and lets the movie play in the mode
 * the desktop is in.
 *
 * A fullscreen game sets the first of its 800x600 and 640x480 cinematic
 * modes the display takes, and Movie::Play makes the movie's window that
 * size at 0,0 (Movie.cinematic_mode). Proton only emulates the mode: the
 * monitor keeps its own size and the 4:3 picture is scaled into a box in the
 * middle, black all around (and a mode it scales leaves light lines along
 * the video's window, whatever its shape). The handler puts the desktop's
 * registry mode back instead (the same bounded reset; nothing to do when the
 * game runs at the desktop's size) and writes the mode the display is then
 * in there, as the game's own body does when none of its modes pass: the
 * window covers the screen and the video renderer scales the video to it
 * (smoothly). The windowed reset after the movie then has nothing to do, and
 * the game sets its own mode again after it as it does after its 800x600.
 *
 * A fullscreen movie also raises the display gamma: Movie::Play sets a ramp
 * of pow(x, 1 / 2.2) for it (Movie.gamma_call) and puts the saved one back
 * after. Wine takes that ramp and Proton applies it to the movie's OpenGL
 * output: black becomes dark grey, and the dark scenes (most of them) come
 * out washed out, their compression noise and chroma blotches turned up
 * several times. The patch points that call, and the one that puts the
 * saved ramp back when a key skips the movie (Movie.gamma_restore), at a
 * no-op that reports success: the movie shows as it was encoded, and the
 * ramp is never touched.
 *
 * Only under Wine: on Windows the reset doesn't fail like that, and the
 * game's own wait is left as it was. Elsewhere the patch is skipped
 * (`needed`).
 */
#include "patches.h"

#include "core/hooks.h"
#include "hooks.h"
#include "log.h"
#include "patch.h"
#include "tome/tome.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace tome::patches {
namespace {

uintptr_t g_cinematic_mode = 0;  // Movie.cinematic_mode: width, height, bits, frequency

// SetDeviceGammaRamp for the movie's call: changes nothing.
BOOL WINAPI keep_gamma_ramp(HDC, LPVOID) { return TRUE; }
void* g_keep_gamma_ramp = reinterpret_cast<void*>(&keep_gamma_ramp);

constexpr int kResetTries = 20;    // after the first call; the game's own pause between them
constexpr DWORD kResetPauseMs = 50;

// The display's mode (`which`: ENUM_CURRENT_SETTINGS or
// ENUM_REGISTRY_SETTINGS). False if it can't be read.
bool read_mode(DWORD which, DEVMODEA& mode) {
    mode = {};
    mode.dmSize = sizeof(mode);
    return EnumDisplaySettingsA(nullptr, which, &mode) != FALSE;
}

// Whether the display is already in its registry mode, so the reset has
// nothing to do (on Windows ChangeDisplaySettingsA(NULL, 0) is then a no-op
// too). In a window the game never changed the mode before a movie, and on a
// display that refuses the reset this saves the retries' second.
bool in_registry_mode() {
    DEVMODEA current, registry;
    if (!read_mode(ENUM_CURRENT_SETTINGS, current) || !read_mode(ENUM_REGISTRY_SETTINGS, registry)) return false;
    return current.dmPelsWidth == registry.dmPelsWidth && current.dmPelsHeight == registry.dmPelsHeight &&
           current.dmBitsPerPel == registry.dmBitsPerPel && current.dmDisplayFrequency == registry.dmDisplayFrequency;
}

bool reset_display_mode() {
    if (in_registry_mode()) return true;
    if (ChangeDisplaySettingsA(nullptr, 0) == DISP_CHANGE_SUCCESSFUL) return true;
    for (int i = 0; i < kResetTries; ++i) {
        Sleep(kResetPauseMs);
        if (ChangeDisplaySettingsA(nullptr, 0) == DISP_CHANGE_SUCCESSFUL) return true;
    }
    return false;
}

void write_cinematic_mode(const DEVMODEA& mode) {
    // What the game's body writes before trying its modes (the frequency stays).
    const uint32_t fields[3] = {mode.dmPelsWidth, mode.dmPelsHeight, mode.dmBitsPerPel};
    std::memcpy(reinterpret_cast<void*>(g_cinematic_mode), fields, sizeof fields);
}

// The cinematic case: the desktop's registry mode back (a fullscreen game
// may run in a mode of its own), and the movie's window that size. If the
// reset fails, the mode the display is in. False if the mode can't be read.
bool use_desktop_mode() {
    if (!reset_display_mode()) {
        TOME_WARN("movie-display-reset: the desktop's display mode couldn't be put back for the movie; playing it "
                  "in the current mode");
    }
    DEVMODEA current;
    if (!read_mode(ENUM_CURRENT_SETTINGS, current)) return false;
    write_cinematic_mode(current);
    return true;
}

// REPLACE Movie::SetDisplayMode(cinematic, mode), innermost: in place of the
// game's body only. Runs on the main thread (the movie player's).
void TOME_CALL on_set_display_mode(tome_mod*, tome_hook_call* call, void*) {
    if (call->arg_count < 1) {
        hooks::call_next(call);
        return;
    }
    // cl: the caller sets only the low byte (sete cl).
    if ((call->args[0] & 0xFF) != 0) {
        if (!use_desktop_mode()) hooks::call_next(call);
        return;
    }
    if (!reset_display_mode()) {
        TOME_WARN("movie-display-reset: the display mode couldn't be reset (ChangeDisplaySettings failed %d times); "
                  "going on in the current mode",
                  kResetTries + 1);
    }
}

// Points one of the movie player's call [SetDeviceGammaRamp] at
// keep_gamma_ramp: ff 15 <slot> becomes ff 15 <&g_keep_gamma_ramp>, the same
// 6 bytes. The slot address in the instruction is relocated with game.dll.
// "" when done, else why not.
std::string skip_gamma_call(uintptr_t base, uint32_t rva) {
    if (!rva) return "not mapped";
    const uintptr_t site = base + rva;
    if (!readable(site, 6)) return "not readable";
    uint32_t slot = 0;
    std::memcpy(&slot, reinterpret_cast<const void*>(site + 2), sizeof slot);
    const uint32_t expected_slot = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
        GetProcAddress(GetModuleHandleA("gdi32.dll"), "SetDeviceGammaRamp")));
    // The slot must hold SetDeviceGammaRamp itself (no other tool hooked it in between).
    if (!readable(slot, 4) || *reinterpret_cast<const uint32_t*>(slot) != expected_slot) {
        return "not a call to SetDeviceGammaRamp";
    }
    uint8_t expected[6] = {0xff, 0x15};
    std::memcpy(expected + 2, &slot, 4);
    uint8_t replacement[6] = {0xff, 0x15};
    void* ptr = &g_keep_gamma_ramp;
    std::memcpy(replacement + 2, &ptr, 4);
    std::string error;
    if (!patch_bytes(site, expected, replacement, sizeof replacement, error)) return error;
    return "";
}

// The movie's gamma ramp and its restore. Best-effort: without them the
// movies are only too bright. "" when both are done, else what's left.
std::string install_gamma_skip(uintptr_t base, const BuildMappings& build) {
    // The ramp first: its restore is only skipped once nothing changes the
    // ramp (a kept restore just puts back the ramp it saved).
    std::string error = skip_gamma_call(base, build.movie.gamma_call);
    if (!error.empty()) return "gamma ramp left (" + error + ")";
    error = skip_gamma_call(base, build.movie.gamma_restore);
    if (!error.empty()) return "gamma restore left (" + error + ")";
    return "";
}

bool install_movie_display_reset(uintptr_t base, const BuildMappings& build, const Variant&, std::string& report) {
    g_cinematic_mode = base + build.movie.cinematic_mode;
    std::string error;
    hooks::Target* set_mode = hooks::find_or_install("Movie::SetDisplayMode", error);
    if (!set_mode) {
        report = "Movie::SetDisplayMode: " + error;
        return false;
    }
    hooks::Handler replace;
    replace.phase = TOME_HOOK_REPLACE;
    replace.last = true;  // innermost: mods' handlers run first, only the game's body is replaced
    replace.fn = on_set_display_mode;
    hooks::add(set_mode, replace);
    const std::string gamma = install_gamma_skip(base, build);
    report = "plays movies full screen in the desktop's mode, at their own gamma, without waiting forever for a "
             "display mode reset";
    if (!gamma.empty()) report += "; " + gamma;
    return true;
}

const char* const kSymbols[] = {"Movie::SetDisplayMode", "Movie.cinematic_mode", nullptr};

} // namespace

extern const Patch kMovieDisplayReset = {
    "movie-display-reset",
    "plays movies (intro, campaign cinematics) full screen in the desktop's mode, at their own gamma, and keeps them "
    "from hanging on a black screen when Wine refuses a display mode reset",
    {},  // the same on every build
    kSymbols,
    nullptr,  // nothing to protect: a mod can't reopen a hole through it
    only_under_wine,
    install_movie_display_reset,
};

} // namespace tome::patches
