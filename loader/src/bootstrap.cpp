// SPDX-License-Identifier: GPL-3.0-or-later
#include "bootstrap.h"

#include "core/command_line.h"
#include "core/text.h"
#include "crash_handler.h"
#include "crash_loop.h"
#include "debug_input.h"
#include "events.h"
#include "frames.h"
#include "game_build.h"
#include "handshake.h"
#include "keybinds.h"
#include "log.h"
#include "main_thread.h"
#include "mod_manager.h"
#include "mod_menu.h"
#include "mods_ready.h"
#include "natives.h"
#include "objects.h"
#include "overlay.h"
#include "patches.h"
#include "paths.h"
#include "tome_settings.h"
#include "update_check.h"
#include "watcher.h"

#include <atomic>
#include <string>
#include <vector>

namespace tome {
namespace {

HMODULE g_self = nullptr;
bool g_game_loaded_at_attach = false;
// Hooks installed from DllMain, before the game runs any further.
const BuildMappings* g_early_build = nullptr;
bool g_early_verified = false;  // g_early_build's signatures match the loaded game.dll
std::vector<std::string> g_verify_errors;
bool g_early_hooks_ok = false;
std::string g_early_hooks_report;
bool g_paths_ok = false;
std::vector<std::string> g_layout_notes;  // from migrate_layout, logged once the log is open
bool g_menu_hooks_ok = false;
std::string g_menu_hooks_report;
std::string g_menu_reload_report;
bool g_events_ok = false;
std::string g_events_report;

bool g_patches_ok = false;
std::string g_patches_report;
bool g_objects_ok = false;
std::string g_objects_report;
bool g_sound_wait_ok = false;
std::string g_sound_wait_report;
bool g_esc_hooks_ok = false;
bool g_handshake_ok = false;
std::string g_handshake_report;
std::string g_esc_hooks_report;
std::string g_assets_report;  // empty if the assets weren't mounted from DllMain
DWORD g_attach_thread = 0;
Paths g_paths;
GameBuild g_build;
std::string g_safe_mode;  // why safe mode is on (from DllMain); empty if it isn't

// Safe mode (docs/research.md §9): the -tomesafe flag, Shift held while the
// game starts, a tome/safe file, or a crash loop (crash_loop.h). Returns
// what asked for it, or "".
std::string safe_mode_reason(const Paths& paths) {
    std::string reason;
    auto add = [&](const char* what) { reason += (reason.empty() ? "" : ", ") + std::string(what); };
    if (command_line::has_flag(GetCommandLineW(), L"-tomesafe")) add("-tomesafe");
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) add("Shift held at startup");
    if (GetFileAttributesW((paths.tome_dir + L"\\safe").c_str()) != INVALID_FILE_ATTRIBUTES) add("tome/safe");
    if (!text::iequals(paths.host_exe, L"worldedit.exe")) {
        std::string loop = crash_loop::check(paths);
        if (!loop.empty()) add(loop.c_str());
    }
    return reason;
}

HMODULE wait_for_module(const wchar_t* name, DWORD timeout_ms) {
    const DWORD start = GetTickCount();
    for (;;) {
        if (HMODULE module = GetModuleHandleW(name)) {
            return module;
        }
        if (GetTickCount() - start >= timeout_ms) {
            return nullptr;
        }
        Sleep(10);
    }
}

struct FindWindowCtx {
    DWORD pid;
    HWND found;
};

BOOL CALLBACK find_main_window(HWND hwnd, LPARAM lparam) {
    auto ctx = reinterpret_cast<FindWindowCtx*>(lparam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ctx->pid && IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER)) {
        ctx->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

HWND find_game_window(DWORD timeout_ms) {
    FindWindowCtx ctx{GetCurrentProcessId(), nullptr};
    const DWORD start = GetTickCount();
    while (!ctx.found && GetTickCount() - start < timeout_ms) {
        EnumWindows(find_main_window, reinterpret_cast<LPARAM>(&ctx));
        if (!ctx.found) Sleep(100);
    }
    return ctx.found;
}

// Temporary, visible proof that Tome is running, until the mod menu exists:
// append "[Tome x.y.z]" to the game window's title.
void announce_in_window_title(HWND window) {
    wchar_t title[256] = {};
    GetWindowTextW(window, title, 256);
    std::wstring new_title = std::wstring(title) + L" [Tome " TOME_VERSION_W L"]";
    SetWindowTextW(window, new_title.c_str());
    TOME_INFO("Window title set to \"%s\"", narrow(new_title).c_str());
}

// Data execution prevention: war3.exe is linked NX-compatible, so Windows
// turns it on unless the system's policy or a compatibility shim turns it
// off. It makes code written into data (a map exploit's) fault instead of
// running. Tome only reports it: forcing it on could break a setup that
// needs it off, and it doesn't stop code that's already running.
void log_dep() {
    DWORD flags = 0;
    BOOL permanent = FALSE;
    if (!GetProcessDEPPolicy(GetCurrentProcess(), &flags, &permanent)) {
        TOME_INFO("DEP: unknown (error %lu)", GetLastError());
        return;
    }
    if (flags & PROCESS_DEP_ENABLE) TOME_INFO("DEP: on%s", permanent ? " (permanent)" : "");
    else TOME_WARN("DEP: off (the system's DEP policy or a compatibility setting turned it off for war3.exe)");
}

// TOME_DEBUG_EXIT=1 (developer builds): once <home>/tome/debug-exit appears,
// the game exits the way it does from its menu (mods disabled and unloaded,
// then ExitProcess). Tests use it where they'd otherwise kill the game: a
// coverage build's counters are only written when each module detaches.
std::atomic<bool> g_exit_started{false};

DWORD WINAPI exit_watch_thread(LPVOID) {
    const std::wstring file = g_paths.tome_dir + L"\\debug-exit";
    while (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) Sleep(200);
    DeleteFileW(file.c_str());
    TOME_INFO("debug-exit: exiting");
    // Whichever thread starts the exit first does it: the other one never
    // ends it midway (ExitProcess on one thread ends the others where they are).
    main_thread::post([] {
        if (g_exit_started.exchange(true)) return;
        mod_manager().shutdown();
        ExitProcess(0);
    });
    // Seen once after a movie: the posted call never ran. Exit from here
    // then, without the mods' shutdown; the counters are still written.
    Sleep(5000);
    if (g_exit_started.exchange(true)) {
        TOME_WARN("debug-exit: the mods' shutdown is still running after 5 s");
        return 0;
    }
    TOME_WARN("debug-exit: the main thread didn't run the exit within 5 s; exiting from the watcher thread");
    ExitProcess(0);
}

void start_exit_watch() {
    char env[8] = {};
    if (!TOME_DEV || !GetEnvironmentVariableA("TOME_DEBUG_EXIT", env, sizeof(env)) || env[0] != '1') return;
    HANDLE thread = CreateThread(nullptr, 0, exit_watch_thread, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
}

DWORD bootstrap() {
    // Until log::open succeeds, TOME_* lines go to the debugger only.
    if (!g_paths_ok) {
        TOME_ERROR("Failed to resolve paths; Tome is disabled");
        return 1;
    }
    if (!log::open(g_paths.tome_dir + L"\\tome.log")) {
        TOME_WARN("Failed to open tome.log; logging to the debugger only");
    }
    debug_input::prepare();

    TOME_INFO("Tome %s starting%s", TOME_VERSION, TOME_DEV ? " (developer build: TOME_DEBUG_* variables are read)" : "");
    TOME_INFO("Host process: %s (pid %lu)", narrow(g_paths.host_exe).c_str(), GetCurrentProcessId());
    TOME_INFO("Game directory: %s", narrow(g_paths.game_dir).c_str());
    TOME_INFO("Mods directory: %s", narrow(g_paths.mods_dir).c_str());
    for (const std::string& note : g_layout_notes) TOME_INFO("Layout: %s", note.c_str());
    TOME_INFO("game.dll already loaded when tome.mix was attached: %s",
              g_game_loaded_at_attach ? "yes" : "no");
    log_dep();
    crash_loop::log_status();

    if (text::iequals(g_paths.host_exe, L"worldedit.exe")) {
        TOME_INFO("Running inside the World Editor; editor support isn't implemented yet. Tome is idle.");
        return 0;
    }

    // As early as the log allows: its rollback of an update that didn't start
    // has to run before anything that version could crash in (the mods' first
    // load, below, and the game's own startup going on meanwhile).
    update_check::init(g_paths);

    for (const std::string& error : mapping_errors()) {
        TOME_ERROR("Mapping database: %s", error.c_str());
    }

    HMODULE game = wait_for_module(L"game.dll", 60000);
    if (!game) {
        TOME_ERROR("game.dll did not load within 60 s; Tome is idle.");
        return 1;
    }

    if (!identify_game_build(game, g_build)) {
        TOME_ERROR("Could not read game.dll's version information; Tome is idle.");
        return 1;
    }
    TOME_INFO("game.dll: %s", narrow(g_build.path).c_str());
    TOME_INFO("  base 0x%08lx, file version %s, PE timestamp 0x%08lx",
              static_cast<unsigned long>(g_build.base), g_build.file_version.c_str(),
              static_cast<unsigned long>(g_build.pe_timestamp));
    TOME_INFO("  sha256 %s", g_build.sha256.c_str());

    if (!g_build.known) {
        if (g_early_build && g_early_verified) {
            TOME_WARN("The PE header looked like %s and its signatures matched, so the early hooks are "
                      "installed, but the SHA-256 doesn't match; nothing else will run.",
                      g_early_build->data->name.c_str());
        } else if (g_early_build) {
            TOME_WARN("The PE header looked like %s, but its signatures don't match, so nothing was patched.",
                      g_early_build->data->name.c_str());
            for (const std::string& error : g_verify_errors) TOME_WARN("  %s", error.c_str());
        }
        TOME_ERROR("This game build is not supported (no mappings for it). Tome is idle; "
                   "the game will run unmodified.");
        return 1;
    }
    TOME_INFO("Recognized Warcraft III %s, %s (%zu mapped symbols)", g_build.known->data->name.c_str(),
              edition_name(g_build.edition), g_build.known->data->symbols.size());

    // Checked before anything is patched: in DllMain if game.dll was already
    // loaded, otherwise here, before the late hooks.
    const bool verified = g_early_build ? g_early_verified : verify_loaded_build(game, *g_build.known, g_verify_errors);
    if (!verified) {
        for (const std::string& error : g_verify_errors) TOME_ERROR("Signature mismatch: %s", error.c_str());
        TOME_ERROR("game.dll doesn't match the %s mappings (%zu symbol(s) differ; modified by another tool?). "
                   "Tome is idle; the game will run unmodified.",
                   g_build.known->data->name.c_str(), g_verify_errors.size());
        return 1;
    }
    TOME_INFO("Signatures match (functions and call sites)");

    if (g_early_build) {
        if (g_early_hooks_ok) {
            TOME_INFO("JASS natives: %s", g_early_hooks_report.c_str());
        } else {
            TOME_ERROR("JASS natives unavailable: %s", g_early_hooks_report.c_str());
        }
        if (g_menu_hooks_ok) {
            TOME_INFO("Mod menu: %s; %s (attached on thread %lu)", g_menu_hooks_report.c_str(),
                      g_menu_reload_report.c_str(), g_attach_thread);
            mod_menu::log_chains_model();
        } else {
            TOME_ERROR("Mod menu unavailable: %s", g_menu_hooks_report.c_str());
        }
        if (g_esc_hooks_ok) {
            TOME_INFO("Mod menu in games: %s", g_esc_hooks_report.c_str());
        } else if (g_menu_hooks_ok) {
            TOME_ERROR("Mod menu in games unavailable: %s", g_esc_hooks_report.c_str());
        }
        if (g_events_ok) {
            TOME_INFO("Events: %s", g_events_report.c_str());
        } else {
            TOME_ERROR("Events unavailable: %s", g_events_report.c_str());
        }
        if (g_handshake_ok) {
            TOME_INFO("Multiplayer handshake: %s", g_handshake_report.c_str());
        } else if (g_events_ok) {
            TOME_ERROR("Multiplayer handshake unavailable: %s", g_handshake_report.c_str());
        }
        if (g_patches_ok) TOME_INFO("Patches: %s", g_patches_report.c_str());
        if (g_objects_ok) {
            TOME_INFO("Game objects: %s", g_objects_report.c_str());
        } else if (!g_objects_report.empty()) {
            TOME_ERROR("Game objects unavailable: %s", g_objects_report.c_str());
        }
        if (g_sound_wait_ok) {
            TOME_INFO("Sound start-up: %s", g_sound_wait_report.c_str());
        } else {
            TOME_WARN("Sound start-up doesn't wait for mods: %s", g_sound_wait_report.c_str());
        }
        if (!g_assets_report.empty()) TOME_INFO("Assets at startup: %s", g_assets_report.c_str());
        crash_handler::start(g_build);
    } else {
        // game.dll wasn't loaded yet when Tome was attached, so this runs
        // concurrently with the game; a map loading right now could miss Tome's natives.
        std::string report;
        bool ok = natives::install_hooks(g_build.base, *g_build.known, report);
        TOME_WARN("JASS natives hooked late (game.dll wasn't loaded at attach): %s", report.c_str());
        if (!ok) TOME_ERROR("JASS natives unavailable");
        TOME_WARN("Events, frames and the mod menu need Tome loaded after game.dll; they stay unavailable");
        TOME_WARN("Built-in patches (memhack-guard) need Tome loaded after game.dll; they stay off");
    }

    // The first load runs here: nothing can call into a mod yet (its natives
    // aren't bound until InitNatives, which waits for this). Every later
    // change runs on the main thread (§5.6).
    ModManager& mods = mod_manager();
    mods.init(g_paths, g_build);
    if (!g_safe_mode.empty()) {
        TOME_WARN("Safe mode (%s): no mod starts this session; state.json is unchanged. Enable mods one at a "
                  "time from the mod menu or with /tome enable <id>.",
                  g_safe_mode.c_str());
    }
    mods.refresh();
    mods_ready::set();
    overlay::log_probe();

    tome_settings::init(g_paths);
    HWND window = find_game_window(60000);
    if (window && main_thread::install(window)) {
        TOME_INFO("Main-thread dispatcher installed on the game window (main thread %lu)",
                  GetWindowThreadProcessId(window, nullptr));
        announce_in_window_title(window);
        keybinds::init(g_paths);
        debug_input::start(window);
        start_exit_watch();
        // Mods enabled above may have asked for a main menu that already exists.
        main_thread::post(frames::catch_up);
        main_thread::post([] { patches::background_mute_start(g_paths.tome_dir); });
    } else {
        TOME_WARN("No game window to attach to; mod changes will apply off the main thread");
    }
    start_watcher(g_paths, [] {
        if (mod_manager().poll_changed()) {
            main_thread::post([] {
                mod_manager().refresh();
                mod_menu::on_mods_changed();
            });
        } else if (mod_manager().poll_settings_changed()) {
            // Only a settings file edited by hand: no full refresh for that.
            main_thread::post([] {
                mod_manager().reload_settings();
                mod_menu::on_mods_changed();
            });
        }
    });

    TOME_INFO("Bootstrap complete");
    return 0;
}

DWORD WINAPI bootstrap_thread(LPVOID) {
    DWORD result = bootstrap();
    // Whatever happened, never leave InitNatives or the sound start-up waiting.
    mods_ready::set();
    return result;
}

} // namespace

void start_bootstrap(HMODULE self) {
    g_self = self;
    // GetModuleHandle doesn't take the loader lock's slow path; safe in DllMain.
    g_attach_thread = GetCurrentThreadId();
    g_paths_ok = init_paths(self, g_paths);
    if (g_paths_ok) g_layout_notes = migrate_layout(g_paths);
    if (g_paths_ok) overlay::init(g_paths);
    if (g_paths_ok) {
        g_safe_mode = safe_mode_reason(g_paths);
        mod_manager().set_safe_mode(g_safe_mode);
    }
    HMODULE game = GetModuleHandleW(L"game.dll");
    g_game_loaded_at_attach = game != nullptr;

    // Patch before returning, so the game can't reach InitNatives first.
    // Identifies the build from the in-memory header; the SHA-256 check
    // follows on the bootstrap thread. File I/O here is limited to Tome's own
    // folders (no LoadLibrary under the loader lock).
    if (game) {
        g_early_build = find_build_by_header(game);
        if (g_early_build) g_early_verified = verify_loaded_build(game, *g_early_build, g_verify_errors);
        if (g_early_verified) {
            g_early_hooks_ok = natives::install_hooks(reinterpret_cast<uintptr_t>(game), *g_early_build,
                                                      g_early_hooks_report);
            // Built-in fixes (always on, independent of events and of Tome's
            // folders): applied to whichever ones fit this build. See patches.h.
            g_patches_ok = patches::install(reinterpret_cast<uintptr_t>(game), *g_early_build, g_patches_report);
            // After the patches: they set up the hook engine (and may wrap natives, which jass_call then calls).
            g_objects_ok = objects::install(reinterpret_cast<uintptr_t>(game), *g_early_build, g_objects_report);
            g_sound_wait_ok = mods_ready::install_sound_wait(g_sound_wait_report);
            if (g_paths_ok) {
                frames::init(reinterpret_cast<uintptr_t>(game), *g_early_build, g_attach_thread);
                crash_handler::install(g_paths, g_attach_thread);
                crash_handler::set_report_sink(crash_loop::note_crash);
                g_menu_hooks_ok = mod_menu::install_hooks(reinterpret_cast<uintptr_t>(game), *g_early_build,
                                                          g_menu_hooks_report);
                // Only a start whose main menu Tome can see counts (crash_loop.h).
                if (g_menu_hooks_ok && !text::iequals(g_paths.host_exe, L"worldedit.exe")) crash_loop::count_start();
                if (g_menu_hooks_ok) mod_menu::reload_frame_definitions(g_menu_reload_report);
                g_events_ok = events::install(reinterpret_cast<uintptr_t>(game), *g_early_build, g_events_report);
                if (g_events_ok) {
                    g_handshake_ok = handshake::install(reinterpret_cast<uintptr_t>(game), *g_early_build,
                                                        g_handshake_report);
                }
                if (g_menu_hooks_ok) g_esc_hooks_ok = mod_menu::install_game_hooks(g_esc_hooks_report);
                // Before the game loads the main menu's models and textures,
                // so mods can override those too.
                GameBuild early;
                early.known = g_early_build;
                early.file_version = g_early_build->data->file_version;
                early.edition = read_edition(reinterpret_cast<uintptr_t>(game), g_early_build);
                mod_manager().mount_startup_assets(g_paths, early, g_assets_report);
            }
        }
    }

    // The thread only starts running after DllMain returns and the loader lock is released.
    HANDLE thread = CreateThread(nullptr, 0, bootstrap_thread, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        TOME_ERROR("Failed to create the bootstrap thread");
    }
}

} // namespace tome
