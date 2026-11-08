// SPDX-License-Identifier: GPL-3.0-or-later
// The loader as under Shade (the fake shade.dll, tests/shade/fake_shade.cpp,
// loaded before it starts): its JASS natives are Shade stubs to
// tome_native_entry instead of code made at run time, and mods' DLLs are
// refused (only code recompiled with the game runs there).
#include "harness.h"
#include "test.h"

#include "mod_manager.h"
#include "shade.h"
#include "shade/fake_shade.h"

#include <cstdint>

using host::Home;
using host::Session;
using tome::mod_manager;

namespace {

using Native1 = int32_t (*)(int32_t);

const tome::ModStatus* find_status(const std::vector<tome::ModStatus>& all, const char* id) {
    for (const tome::ModStatus& s : all) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

} // namespace

TEST(host_shade_refuses_mod_dlls) {
    CHECK(fake_shade::load() != nullptr);
    CHECK(tome::shade::active());
    Home home;
    CHECK(home.install_mod("crash"));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("crash", true);
    auto all = mod_manager().status();
    const tome::ModStatus* crash = find_status(all, "crash");
    CHECK(crash != nullptr);
    if (!crash) return;
    CHECK(!crash->running);
    bool refused = false;
    for (const std::string& e : crash->errors) {
        refused |= e.find("its DLL can't run under Shade") != std::string::npos;
    }
    CHECK(refused);
}

TEST(host_shade_natives) {
    CHECK(fake_shade::load() != nullptr);
    CHECK(tome::shade::active());
    Home home;
#ifdef TOME_TEST_WASM_EXAMPLE
    CHECK(home.install_mod("hello-wasm"));
#endif
    Session session;
    CHECK(session.start(home));
#ifdef TOME_TEST_WASM_EXAMPLE
    mod_manager().set_enabled("hello-wasm", true);
#endif
    session.game.load_map();
    CHECK_LOG("Engine initialized natives; bound");

    // Tome's own: jumped to, with the engine's stack frame.
    auto log_integer = reinterpret_cast<Native1>(session.game.native("TomeLogInteger"));
    CHECK(log_integer != nullptr);
    if (log_integer) log_integer(-7);
    CHECK_LOG("[map] TomeLogInteger(-7)");
    using Native0 = int32_t (*)();
    auto is_loaded = reinterpret_cast<Native0>(session.game.native("TomeIsLoaded"));
    CHECK(is_loaded != nullptr);
    if (is_loaded) CHECK_EQ(is_loaded(), 1);

#ifdef TOME_TEST_WASM_EXAMPLE
    // A mod's: called guarded through tome_native_route.
    using Add = int32_t (*)(int32_t, int32_t);
    auto add = reinterpret_cast<Add>(session.game.native("HelloWasmAdd"));
    CHECK(add != nullptr);
    if (!add) return;
    CHECK_EQ(add(40, 2), 42);
    mod_manager().set_enabled("hello-wasm", false);
    CHECK_EQ(add(40, 2), 0);  // none attached: 0
    mod_manager().set_enabled("hello-wasm", true);
    CHECK_EQ(add(1, 2), 3);
#endif
}
