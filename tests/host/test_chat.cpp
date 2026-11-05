// SPDX-License-Identifier: GPL-3.0-or-later
// Chat commands through Tome's hook on CGameUI::HandleSlashCommand (the
// fake game image carries every mapped function, so events::install hooks
// them all): /tome, a mod's own command, and what goes on to the game.
#include "harness.h"
#include "test.h"

#include "log.h"
#include "mod_manager.h"
#include "paths.h"

using host::Home;
using host::Session;
using tome::mod_manager;

namespace {

bool running(const char* id) {
    for (const tome::ModStatus& s : mod_manager().status()) {
        if (s.id == id) return s.running;
    }
    return false;
}

} // namespace

TEST(host_chat_tome_commands) {
    Home home;
    CHECK(home.install_mod("hello"));
    Session session;
    session.events = true;
    CHECK(session.start(home));
    mod_manager().set_enabled("hello", true);
    CHECK(running("hello"));

    CHECK_EQ(session.game.chat("/tome disable hello"), 1);
    CHECK_LOG("Chat command: /tome disable hello");
    CHECK(!running("hello"));
    CHECK_EQ(session.game.chat("/tome enable hello"), 1);
    CHECK(running("hello"));
    CHECK_EQ(session.game.chat("/tome nonsense"), 1);  // unknown subcommands are still Tome's
    CHECK_LOG("Chat command: /tome nonsense");

    // /tome logs reads tome.log as log.cpp writes it.
    CHECK_EQ(session.game.chat("/tome logs 3"), 1);
    CHECK_LOG("/tome logs: showed 3 lines");
    TOME_WARN("host test: a warning for /tome logs");
    CHECK_EQ(session.game.chat("/tome logs warn 1"), 1);
    CHECK_LOG("/tome logs: showed 1 lines");
}

TEST(host_chat_mod_command) {
    Home home;
    CHECK(home.install_mod("hello"));
    Session session;
    session.events = true;
    CHECK(session.start(home));
    mod_manager().set_enabled("hello", true);

    CHECK_EQ(session.game.chat("/hello e2e"), 1);
    CHECK_LOG("Chat command: /hello e2e (hello)");
    CHECK_LOG("[hello] /hello command: Hello, e2e.");

    // A disabled mod's command isn't Tome's to handle: the game gets the line.
    mod_manager().set_enabled("hello", false);
    CHECK_EQ(session.game.chat("/hello ignored"), 0);
    CHECK_NO_LOG("Hello, ignored");
    CHECK_EQ(session.game.chat("just chatting"), 0);
}

// The C++ and Rust examples' commands (their tome.hpp and Rust wrappers).
TEST(host_chat_cpp_and_rust_commands) {
    Home home;
    CHECK(home.install_mod("hello-cpp"));
    std::string dll;
    const bool rust = tome::read_file(host::win_path(TOME_TEST_BUILD_DIR "/mods/hello-rs/hello_rs.dll"), dll);
    if (rust) CHECK(home.install_mod("hello-rs"));
    Session session;
    session.events = true;
    CHECK(session.start(home));
    mod_manager().set_enabled("hello-cpp", true);
    CHECK_EQ(session.game.chat("/hellocpp e2e"), 1);
    CHECK_LOG("[hello-cpp] /hellocpp command: Hello from C++, e2e.");
    if (!rust) return;
    mod_manager().set_enabled("hello-rs", true);
    CHECK_EQ(session.game.chat("/hellors e2e"), 1);
    CHECK_NO_LOG("crashed in chat command /hellors");
    CHECK_LOG("[hello-rs] /hellors command: Hello from Rust, e2e.");
}
