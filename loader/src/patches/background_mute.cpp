// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Audio in the background (docs/known-bugs.md, "Sound in the background").
 *
 * The game keeps playing its sound effects and music after Alt+Tab, or
 * whenever another window has the foreground, on Windows and under Wine.
 *
 * Mechanism: the main-thread dispatcher's subclass of the game window passes
 * every WM_ACTIVATEAPP to this patch (main_thread::set_activate_hook), which
 * mutes the process's default audio session (Core Audio's
 * ISimpleAudioVolume::SetMute) when the game leaves the foreground and
 * unmutes it when it comes back. One switch covers everything the game plays,
 * whatever Miles uses for it (its 2D mixer, a 3D provider, the music stream):
 * on Windows and under Wine they all play through the process's default
 * session.
 * A mute the user set in the system mixer is left alone
 * (core/background_mute.*). Only the game's own session: other programs and
 * the system volume aren't touched. No game.dll symbols: it works on any
 * build.
 *
 * Windows' volume mixer keeps an application's mute for its next run, so a
 * game that crashes or quits while away would start muted. While Tome's mute
 * is on, tome/background-mute.flag exists; at the next start
 * (background_mute_start) a session that's muted with the flag still there
 * is unmuted. A normal quit doesn't leave it: destroying the active window
 * deactivates the game (which mutes it), and WM_DESTROY then unmutes it
 * (on_destroy).
 */
#include "patches.h"

#include "core/background_mute.h"
#include "log.h"
#include "main_thread.h"
#include "paths.h"

#include <windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include <string>

namespace tome::patches {
namespace {

BackgroundMute g_mute;                     // main thread only
ISimpleAudioVolume* g_volume = nullptr;    // kept for the session
bool g_volume_failed = false;
std::wstring g_flag;                       // tome/background-mute.flag; empty until started
bool g_installed = false;

// The process's default audio session on the default output device, made on
// first use (on the main thread, where the game's own audio objects live).
ISimpleAudioVolume* session_volume() {
    if (g_volume || g_volume_failed) return g_volume;
    g_volume_failed = true;  // one attempt: a failure is logged once
    // S_FALSE or RPC_E_CHANGED_MODE: the game initialized COM already, which
    // is fine. Not balanced by CoUninitialize: the interface stays in use.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IMMDeviceEnumerator* devices = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&devices));
    if (FAILED(hr)) {
        TOME_WARN("background-mute: no audio device enumerator (0x%08lx)", static_cast<unsigned long>(hr));
        return nullptr;
    }
    IMMDevice* device = nullptr;
    hr = devices->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    devices->Release();
    if (FAILED(hr)) {
        TOME_WARN("background-mute: no default audio output (0x%08lx)", static_cast<unsigned long>(hr));
        return nullptr;
    }
    IAudioSessionManager* sessions = nullptr;
    hr = device->Activate(__uuidof(IAudioSessionManager), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&sessions));
    device->Release();
    if (FAILED(hr)) {
        TOME_WARN("background-mute: no audio session manager (0x%08lx)", static_cast<unsigned long>(hr));
        return nullptr;
    }
    // GUID_NULL, not cross-process: the session every stream of this process
    // joins unless it names another one.
    ISimpleAudioVolume* volume = nullptr;
    hr = sessions->GetSimpleAudioVolume(nullptr, FALSE, &volume);
    sessions->Release();
    if (FAILED(hr)) {
        TOME_WARN("background-mute: no audio session volume (0x%08lx)", static_cast<unsigned long>(hr));
        return nullptr;
    }
    g_volume = volume;
    g_volume_failed = false;
    return g_volume;
}

void apply(ISimpleAudioVolume* volume, BackgroundMute::Action action, bool was_ours) {
    switch (action) {
    case BackgroundMute::Action::mute:
        // The flag first: a crash between the two then still gets undone.
        if (!g_flag.empty()) write_file_atomic(g_flag, "");
        if (FAILED(volume->SetMute(TRUE, nullptr))) TOME_WARN("background-mute: couldn't mute the game");
        break;
    case BackgroundMute::Action::unmute:
        if (FAILED(volume->SetMute(FALSE, nullptr))) {
            TOME_WARN("background-mute: couldn't unmute the game");
        } else if (!g_flag.empty()) {
            DeleteFileW(g_flag.c_str());
        }
        break;
    case BackgroundMute::Action::none:
        // Ours no longer (unmuted from the mixer while away, or our mute
        // failed): the flag would make the next start undo a mute the user
        // sets later.
        if (was_ours && !g_mute.muted_by_us() && !g_flag.empty()) DeleteFileW(g_flag.c_str());
        break;
    }
}

void on_activate(bool active) {
    ISimpleAudioVolume* volume = session_volume();
    if (!volume) return;
    BOOL muted = FALSE;
    if (FAILED(volume->GetMute(&muted))) return;
    bool was_ours = g_mute.muted_by_us();
    BackgroundMute::Action action = g_mute.activate(active, muted != FALSE);
    TOME_DEBUG("background-mute: %s the foreground, %s%s", active ? "back in" : "out of",
               muted ? "muted" : "not muted",
               action == BackgroundMute::Action::mute     ? ": muting"
               : action == BackgroundMute::Action::unmute ? ": unmuting"
                                                          : "");
    apply(volume, action, was_ours);
}

// The game quits: undo Tome's mute (and its flag), which Windows would keep
// for the next run.
void on_destroy() {
    ISimpleAudioVolume* volume = g_volume;  // nothing to undo without one
    if (!volume) return;
    BOOL muted = FALSE;
    if (FAILED(volume->GetMute(&muted))) return;
    bool was_ours = g_mute.muted_by_us();
    apply(volume, g_mute.close(muted != FALSE), was_ours);
}

bool install_background_mute(uintptr_t, const BuildMappings&, const Variant&, std::string& report) {
    // Takes effect once the dispatcher subclasses the game window (bootstrap).
    main_thread::set_activate_hook(on_activate);
    main_thread::set_destroy_hook(on_destroy);
    g_installed = true;
    report = "mutes the game while it's in the background";
    return true;
}

} // namespace

void background_mute_start(const std::wstring& tome_dir) {
    if (!g_installed) return;
    g_flag = tome_dir + L"\\background-mute.flag";
    // Made here, from a posted message, rather than first inside a sent
    // WM_ACTIVATEAPP.
    session_volume();
    if (g_mute.muted_by_us()) {
        // Muted before the flag's path was known: mark it now, so a crash
        // while away still gets undone. A flag from the last run is now ours.
        write_file_atomic(g_flag, "");
        return;
    }
    if (GetFileAttributesW(g_flag.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    // Left by a run that ended while away: undo its mute.
    ISimpleAudioVolume* volume = session_volume();
    BOOL muted = FALSE;
    if (volume && SUCCEEDED(volume->GetMute(&muted)) && muted) {
        if (FAILED(volume->SetMute(FALSE, nullptr))) {
            TOME_WARN("background-mute: couldn't undo the mute of the last run");
            return;
        }
        TOME_INFO("background-mute: unmuted the game, which the last run left muted");
    }
    DeleteFileW(g_flag.c_str());
}

extern const Patch kBackgroundMute = {
    "background-mute",
    "mutes the game while it's in the background (after Alt+Tab it kept playing)",
    {},  // the same on every build
    nullptr,  // no game.dll symbols
    nullptr,  // nothing to protect
    nullptr,  // needed everywhere
    install_background_mute,
};

} // namespace tome::patches
