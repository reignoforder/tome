// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Drives the real loader (tome_runtime: the code in tome.mix but DllMain) in
// this process, without the game: a temporary TOME_HOME with fixture mods, a
// fake game image laid out at the real 1.27b RVAs (testing/fake_game.h, which
// mods' own tests use too, through tome_test.dll), and the log kept in memory
// for checks. Each test runs in a process of its own (host_main.cpp), so the
// loader's globals start fresh every time. Nothing waits on a clock: there is
// no game window, so main_thread::post runs everything at once, on this thread.

#include "game_build.h"
#include "paths.h"
#include "testing/fake_game.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace host {

std::wstring win_path(const std::string& path);

// The fake 1.27b game image (loader/src/testing/fake_game.h): no game files.
using FakeGame = tome::testing::FakeGame;

// A Tome home folder in %TEMP%, removed when the test passes.
class Home {
public:
    Home();
    ~Home();
    const tome::Paths& paths() const { return home_.paths(); }
    const tome::testing::Home& home() const { return home_; }
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
    tome::testing::Home home_;
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
