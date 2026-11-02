// SPDX-License-Identifier: GPL-3.0-or-later
// The loader against the real game.dll, loaded into this process but never
// run: every check here is deterministic (no window, no clock, no rendering).
// Needs the local 1.27b install (TOME_GAME_DIR); each test says "skipped"
// without it. game.dll is loaded from the install as it is (only read); its
// Mss32.dll doesn't load .mix files until the game starts its sound, so a
// tome.mix deployed there stays out (checked).
#include "harness.h"
#include "test.h"

#include "events.h"
#include "frames.h"
#include "game_build.h"
#include "handshake.h"
#include "hooks.h"
#include "mappings.h"
#include "mod_manager.h"
#include "mod_menu.h"
#include "mods_ready.h"
#include "natives.h"
#include "patches.h"

#include <windows.h>

#include <cstdio>

using host::Home;
using host::Session;
using tome::mod_manager;

namespace {

// game.dll loaded from the install, identified and verified; null (and the
// test skipped or failed) otherwise.
HMODULE load_game(tome::GameBuild& build) {
    std::wstring dir = host::win_path(TOME_TEST_GAME_DIR);
    if (GetFileAttributesW((dir + L"\\game.dll").c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::printf("  skipped: no game.dll in %s\n", TOME_TEST_GAME_DIR);
        return nullptr;
    }
    SetDllDirectoryW(dir.c_str());
    HMODULE game = LoadLibraryW((dir + L"\\game.dll").c_str());
    if (!game) {
        test::fail(__FILE__, __LINE__, "LoadLibrary(game.dll) failed: error " + std::to_string(GetLastError()));
        return nullptr;
    }
    if (GetModuleHandleW(L"tome.mix")) {
        test::fail(__FILE__, __LINE__, "a tome.mix got loaded along with game.dll");
        return nullptr;
    }
    if (!tome::identify_game_build(game, build) || !build.known) {
        std::printf("  skipped: game.dll isn't a build Tome maps (sha256 %s)\n", build.sha256.c_str());
        return nullptr;
    }
    return game;
}

} // namespace

TEST(game_dll_matches_its_mapping) {
    tome::GameBuild build;
    HMODULE game = load_game(build);
    if (!game) return;
    CHECK_EQ(build.known->data->name, std::string("1.27b"));
    CHECK_EQ(build.file_version, std::string("1.27.1.7085"));
    // Loaded where Windows put it, usually not 0x6F000000: the RVAs have to hold.
    std::vector<std::string> errors;
    CHECK(tome::verify_loaded_build(game, *build.known, errors));
    for (const std::string& e : errors) test::fail(__FILE__, __LINE__, e);
    CHECK(tome::find_build_by_header(game) == build.known);
}

// Everything start_bootstrap (bootstrap.cpp) patches from DllMain, on the
// real code, in its order: every report must say it applied. The one step
// left out, mod_menu::reload_frame_definitions, runs game code (it loads
// Tome's TOC through the game) and needs the game running.
TEST(game_startup_patches_apply) {
    tome::GameBuild build;
    HMODULE game = load_game(build);
    if (!game) return;
    Home home;
    host::open_log(home);
    const uintptr_t base = reinterpret_cast<uintptr_t>(game);
    const tome::BuildMappings& known = *build.known;
    std::string report;

    CHECK(tome::natives::install_hooks(base, known, report));
    CHECK_EQ(report, std::string("redirected 3 of 3 InitNatives call sites"));

    CHECK(tome::patches::install(base, known, report));
    CHECK(report.rfind(" applied, 0 skipped: ", std::string::npos) != std::string::npos);
    for (const char* patch : {"memhack-guard applied", "path-sanitizer applied", "blp-guard applied",
                              "d3d-reset-lights applied", "gl-context-restore applied", "background-mute applied",
                              "widescreen applied", "ambient-sound applied", "movie-display-reset applied",
                              "movie-frame-length applied"}) {
        if (report.find(patch) == std::string::npos) test::fail(__FILE__, __LINE__, std::string(patch) + ": " + report);
    }
    CHECK(report.find("failed") == std::string::npos);
    if (report.find("gamma ramp left") != std::string::npos || report.find("gamma restore left") != std::string::npos) {
        test::fail(__FILE__, __LINE__, "movie gamma: " + report);
    }

    CHECK(tome::mods_ready::install_sound_wait(report));

    tome::frames::init(base, known, GetCurrentThreadId());
    if (!tome::mod_menu::install_hooks(base, known, report)) test::fail(__FILE__, __LINE__, "mod menu: " + report);

    if (!tome::events::install(base, known, report)) test::fail(__FILE__, __LINE__, "events: " + report);
    CHECK_EQ(report, std::string("hooked screens, map load, game start and end, frames, game ticks, shutdown, chat, "
                                 "pause, units, spells, damage, experience, players, resources, keys, menus, "
                                 "dialogs, levels and saved games"));
    if (!tome::handshake::install(base, known, report)) test::fail(__FILE__, __LINE__, "handshake: " + report);
    if (!tome::mod_menu::install_game_hooks(report)) test::fail(__FILE__, __LINE__, "Esc menu: " + report);
}

// The hook engine on every mapped function's real prologue: each one is
// patched (bytes checked, instructions moved, short branches widened). A
// prologue the x86 decoder gets wrong would crash the game the first time a
// mod hooks it.
TEST(game_every_mapped_function_hooks) {
    tome::GameBuild build;
    HMODULE game = load_game(build);
    if (!game) return;
    Home home;
    host::open_log(home);
    const tome::mappings::Build& data = *build.known->data;
    tome::hooks::init(reinterpret_cast<uintptr_t>(game), &data);
    int hooked = 0;
    for (const tome::mappings::Symbol& sym : data.symbols) {
        if (sym.kind != tome::mappings::Kind::Function) continue;
        std::string error;
        if (tome::hooks::find_or_install(sym.name, error)) {
            ++hooked;
        } else {
            test::fail(__FILE__, __LINE__, sym.name + ": " + error);
        }
    }
    int functions = 0;
    for (const tome::mappings::Symbol& sym : data.symbols) functions += sym.kind == tome::mappings::Kind::Function;
    std::printf("  %d of %d mapped functions hooked\n", hooked, functions);
    CHECK_EQ(hooked, functions);
}

// Two mods' handlers around the game's own RCString::GetText (real code, run
// through Tome's hook): the same order the e2e test checks in the game.
TEST(game_hooks_run_around_real_code) {
    tome::GameBuild build;
    HMODULE game = load_game(build);
    if (!game) return;
    Home home;
    CHECK(home.install_mod("hook-a"));
    CHECK(home.install_mod("hook-b"));
    Session session;
    CHECK(session.start(home, build));
    mod_manager().set_enabled("hook-a", true);
    mod_manager().set_enabled("hook-b", true);
    CHECK_LOG("[hook-b] probe returned \"TomeHookProbe from hook-a\"");
    CHECK_LOG("(5 bytes moved, 1 arguments)");

    mod_manager().set_enabled("hook-a", false);
    mod_manager().set_enabled("hook-b", false);
    mod_manager().set_enabled("hook-b", true);
    CHECK_LOG("[hook-b] probe returned \"TomeHookProbe\"");
}

// A mod's crash in a REPLACE handler of real game code: the original runs.
TEST(game_crash_in_hook_runs_the_real_original) {
    tome::GameBuild build;
    HMODULE game = load_game(build);
    if (!game) return;
    Home home;
    CHECK(home.install_mod("crash"));
    CHECK(home.write(L"tome\\mods\\crash\\mode.txt", "hook"));
    Session session;
    CHECK(session.start(home, build));
    mod_manager().set_enabled("crash", true);
    CHECK_LOG("crash crashed in replace hook RCString::GetText: access violation");
    CHECK_LOG("[crash] probe returned \"TomeCrashProbe\"");
}
