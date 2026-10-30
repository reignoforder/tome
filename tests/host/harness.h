#pragma once

// Drives the real loader (tome_runtime: the code in tome.mix but DllMain) in
// this process, without the game: a temporary TOME_HOME with fixture mods, a
// fake game image laid out at the real 1.27b RVAs, and the log kept in memory
// for checks. Each test runs in a process of its own (host_main.cpp), so the
// loader's globals start fresh every time. Nothing waits on a clock: there is
// no game window, so main_thread::post runs everything at once, on this thread.

#include "game_build.h"
#include "paths.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace host {

// "/unix/path" (a CMake path, under Wine) as "Z:\unix\path"; Windows paths unchanged.
std::wstring win_path(const std::string& path);

// The 1.27b image the tests run against: committed, never-executed memory at
// the real RVAs, with only the parts the tests need filled in. No game files.
//   - RCString::GetText: its mapped bytes are the whole function, copied.
//   - InitNatives: a stub that counts calls; its three mapped call sites are
//     real `call InitNatives` instructions followed by `ret`, so calling a
//     call site is what loading a map does (natives::install_hooks patches them).
//   - BindNative: a jump to a recorder; the bound natives' thunks can then be
//     called the way the JASS VM calls them (cdecl, one 32-bit argument each).
class FakeGame {
public:
    bool create();
    tome::GameBuild build() const;
    uintptr_t base() const { return base_; }
    uintptr_t address(const char* symbol) const;

    // Runs InitNatives through call site `n` (0-2), as a map load does.
    void load_map(int n = 1) const;
    unsigned engine_init_count() const;
    // The thunk the engine last bound for `name`, or nullptr.
    void* native(const char* name) const;

    // A line sent from the in-game chat box, as the game passes it to
    // CGameUI::HandleSlashCommand (hooked by events::install): 1 if Tome or a
    // mod handled it, 0 if it went on to the game (the fake handles nothing).
    int chat(const char* text) const;

private:
    bool put_bytes(const char* symbol);
    bool put_slash_command();
    bool put_jump(const char* symbol, const void* target);

    uintptr_t base_ = 0;
    const tome::BuildMappings* mappings_ = nullptr;
};

// A Tome home folder in %TEMP%, removed when the test passes.
class Home {
public:
    Home();
    ~Home();
    const tome::Paths& paths() const { return paths_; }
    // Copies a built mod (<build>/mods/<folder>) or a fixture without code
    // (tests/e2e/mods/<folder>) into tome/mods.
    bool install_mod(const char* folder);
    bool write(const std::wstring& relative, const std::string& text);
    bool read(const std::wstring& relative, std::string& text) const;
    bool remove(const std::wstring& relative);
    bool exists(const std::wstring& relative) const;
    // Sets the file's last write time `seconds` ahead of now: a changed stamp,
    // as a rebuild gives (CopyFile keeps the source's time).
    bool touch(const std::wstring& relative, int seconds = 10);

private:
    tome::Paths paths_;
};

// Opens tome.log in the home and keeps every line in memory.
void open_log(const Home& home);
// Whether any line logged so far contains `text`.
bool logged(std::string_view text);
// Every line logged so far, for failure messages.
std::string log_text();

// The usual start: a fake game, the mod manager on `home`, natives hooked,
// mods discovered and loaded, mods_ready set; as bootstrap() does it.
struct Session {
    FakeGame game;
    std::string safe_mode;  // set before start: why safe mode is on (as from DllMain)
    bool events = false;    // set before start: install the event hooks too (chat commands)
    // Against the fake game image.
    bool start(const Home& home);
    // Against another image, such as the real game.dll (tests/game).
    bool start(const Home& home, const tome::GameBuild& build);
};

} // namespace host

// Checks with the log attached when they fail.
#define CHECK_LOG(text)                                                                     \
    do {                                                                                    \
        if (!host::logged(text))                                                            \
            test::fail(__FILE__, __LINE__, std::string("no log line with \"") + (text) +   \
                                               "\"; the log:\n" + host::log_text());      \
    } while (0)
#define CHECK_NO_LOG(text)                                                                  \
    do {                                                                                    \
        if (host::logged(text))                                                             \
            test::fail(__FILE__, __LINE__, std::string("unexpected log line with \"") +    \
                                               (text) + "\"; the log:\n" + host::log_text()); \
    } while (0)
