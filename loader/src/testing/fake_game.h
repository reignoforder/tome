// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The loader without the game: a fake game.dll image and a Tome home folder
// of its own, for Tome's host tests (tests/host), tome_test.dll (mods' own
// tests, sdk/include/tome/tome_test.h) and tome-check. Each process runs
// one session: the loader keeps its state in globals.
//
// The fake image is committed, never-executed memory at the real 1.27b RVAs,
// with only these parts filled in:
//   - every mapped function's checked bytes, so Tome's hooks install;
//   - RCString::GetText: its mapped bytes are the whole function, copied, so
//     a string argument is a fake RCString (string_arg);
//   - InitNatives: a stub that counts calls; its three mapped call sites are
//     real `call InitNatives` instructions followed by `ret`, so calling a
//     call site is what loading a map does (natives::install_hooks patches them);
//   - BindNative: a jump to a recorder; the bound natives' thunks can then be
//     called the way the JASS VM calls them (cdecl, one 32-bit argument each);
//   - CGameUI::HandleSlashCommand: a whole function that handles nothing;
//   - CreateJassString and the trigger natives Tome runs code values with:
//     fakes that keep the strings and count each code value's runs;
//   - CGameWar3::GetPlayer: no player in any slot (the instance load_map
//     sets has none), so the selection and player names read as none;
//   - Storm.dll, if asked for (StartOptions::fake_storm): mods' assets
//     mount, but no file is found in them.
// Nothing waits on a clock: there is no game window, so main_thread::post
// runs everything at once, on the calling thread.

#include "game_build.h"
#include "paths.h"

#include <cstdint>
#include <string>

namespace tome::testing {

// "/unix/path" (a CMake path, under Wine) as "Z:\unix\path"; Windows paths
// get backslashes and are otherwise unchanged.
std::wstring win_path(const std::string& path);
bool copy_tree(const std::wstring& from, const std::wstring& to);
void remove_tree(const std::wstring& dir);
bool path_exists(const std::wstring& path);

class FakeGame {
public:
    bool create();
    GameBuild build() const;
    uintptr_t base() const { return base_; }
    uintptr_t address(const char* symbol) const;
    // Which game it runs as (Game.expansion): The Frozen Throne after create().
    void set_edition(Edition edition) const;

    // Runs InitNatives through call site `n` (0-2), as a map load does, and
    // from then on a game is running (CGameWar3's instance is set), so
    // natives may return strings and run code values.
    void load_map(int n = 1);
    unsigned engine_init_count() const;
    // The thunk the engine last bound for `name`, or nullptr.
    void* native(const char* name) const;

    // A line sent from the in-game chat box, as the game passes it to
    // CGameUI::HandleSlashCommand (hooked by events::install): 1 if Tome or a
    // mod handled it, 0 if it went on to the game (the fake handles nothing).
    int chat(const char* text) const;

    // A JASS string argument holding `text` (copied): an RCString the copied
    // GetText reads. Valid for the rest of the process.
    static uint32_t string_arg(const char* text);
    // The text of a string a native returned (CreateJassString's id), or
    // nullptr if the fake never made that id.
    static const char* string_result(uint32_t id);
    // How often Tome ran code value `code` (TriggerEvaluate or TriggerExecute),
    // and what it returns as a condition (default 1).
    static unsigned code_runs(uint32_t code);
    static void set_code_result(uint32_t code, bool result);

private:
    bool put_bytes(const char* symbol);
    bool put_slash_command();
    bool put_jump_at(uint32_t rva, const void* target);
    bool put_jump(const char* symbol, const void* target);

    uintptr_t base_ = 0;
    const BuildMappings* mappings_ = nullptr;
};

// A Tome home folder in %TEMP% (tome/, tome/mods/), <prefix>-<process id>-<time>;
// removed by remove(), but for the mods' DLLs (Tome never unloads them).
class Home {
public:
    explicit Home(const wchar_t* prefix = L"tome-host");
    const Paths& paths() const { return paths_; }
    void remove() const { remove_tree(paths_.home_dir); }
    // Removes what remove() left of earlier homes with this prefix, once their
    // process has ended. A home that still has its tome.log (one kept after a
    // failure, for a look) stays.
    static void remove_leftovers(const wchar_t* prefix);

private:
    Paths paths_;
};

// Copies a mod folder or a .tome (trailing backslashes ignored) into the
// home's tome/mods/, under its own name (set in *name if given). False if
// it isn't there or couldn't be copied.
bool install(const Home& home, std::wstring from, std::wstring* name = nullptr);

// The home's tome/tome.log.
std::wstring log_path(const Home& home);
// Opens tome.log in the home, keeps every line in memory too, and logs the
// home's path. start() does this first.
void open_log(const Home& home);

// The usual start, as bootstrap() does it: the log in the home (every line
// kept in memory too), natives hooked, the event hooks if `events`, the mod
// manager on `home`, mods discovered and loaded, mods_ready set. The calling
// thread is the main thread.
struct StartOptions {
    std::string safe_mode;  // why safe mode is on (as from DllMain); "" for a normal start
    bool events = false;    // install the event hooks too (chat commands, events)
    bool fake_storm = false;  // mods' assets mount (into nothing: storm::use_fake_for_test)
};
bool start(const Home& home, const GameBuild& build, const StartOptions& options);

} // namespace tome::testing
