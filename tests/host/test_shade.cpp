// SPDX-License-Identifier: GPL-3.0-or-later
// The loader as under Shade (the fake shade.dll, tests/shade/fake_shade.cpp,
// loaded before it starts): its JASS natives are Shade stubs to
// tome_native_entry instead of code made at run time, and mods' DLLs are
// refused by a Shade before version 4 (only code recompiled with the game
// runs there; 3 can't say what code runs for a DLL) and loaded by a newer
// one, whose built code (ShadeModCodeHash) is part of what's allowed.
#include "harness.h"
#include "signing.h"
#include "test.h"

#include "core/mod_state.h"
#include "mod_manager.h"
#include "sha256.h"
#include "shade.h"
#include "shade/fake_shade.h"

#include <cstdint>

using host::allowed_with_key;
using host::crash_hash;
using host::Home;
using host::Session;
using host::sign_crash;
using tome::mod_manager;

namespace {

using Native1 = int32_t (*)(int32_t);

const tome::ModStatus* find_status(const std::vector<tome::ModStatus>& all, const char* id) {
    for (const tome::ModStatus& s : all) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

std::string state_of(const char* id) {
    auto all = mod_manager().status();
    const tome::ModStatus* s = find_status(all, id);
    return s ? s->state : "(missing)";
}

std::string approval_of(const char* id) {
    auto all = mod_manager().status();
    const tome::ModStatus* s = find_status(all, id);
    return s ? s->approval : "(missing)";
}

// The fake Shade as version 4, naming `built` as every DLL's code.
const fake_shade::Api* shade_4(const std::string& built) {
    const fake_shade::Api* shade = fake_shade::load();
    if (!shade) return nullptr;
    shade->set_version(4);
    shade->set_mod_code(built.c_str());
    return tome::shade::can_load_dlls() ? shade : nullptr;
}

const std::string kBuiltA(64, '1');
const std::string kBuiltB(64, '2');

// What's allowed for the crash fixture when its DLL runs as `built`.
std::string crash_hash_built(const Home& home, const std::string& built) {
    return tome::sha256_bytes(tome::mod_state::built_code_manifest(crash_hash(home), built));
}

} // namespace

TEST(host_shade_refuses_mod_dlls) {
    // Version 3 loads DLLs but can't say what runs for them: refused.
    const fake_shade::Api* shade = fake_shade::load();
    CHECK(shade != nullptr);
    if (!shade) return;
    shade->set_version(3);
    CHECK(tome::shade::active());
    CHECK(!tome::shade::can_load_dlls());
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
        refused |= e.find("its DLL can't run under this Shade") != std::string::npos;
    }
    CHECK(refused);
}

TEST(host_shade_loads_mod_dlls) {
    CHECK(shade_4(kBuiltA) != nullptr);
    CHECK_EQ(tome::shade::mod_code_hash(L"C:\\any.dll"), kBuiltA);
    Home home;
    CHECK(home.install_mod("hello"));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("hello", true);
    auto all = mod_manager().status();
    const tome::ModStatus* hello = find_status(all, "hello");
    CHECK(hello != nullptr);
    if (hello) {
        CHECK(hello->running);
        for (const std::string& e : hello->errors) CHECK(e.find("can't run under") == std::string::npos);
    }
}

TEST(host_shade_built_code_is_part_of_the_approval) {
    // Asked about like on Windows; turning it on allows the files and the
    // code Shade built from them.
    CHECK(shade_4(kBuiltA) != nullptr);
    Home home;
    CHECK(home.install_mod("crash"));
    Session session;
    CHECK(session.start(home));
    CHECK_EQ(state_of("crash"), std::string("approval"));
    CHECK_EQ(approval_of("crash"), std::string("new"));
    CHECK_NO_LOG("[crash] mode");
    mod_manager().set_enabled("crash", true);
    CHECK_EQ(state_of("crash"), std::string("active"));
    std::string state;
    CHECK(home.read(L"tome\\state.json", state));
    CHECK(state.find(crash_hash_built(home, kBuiltA)) != std::string::npos);
    CHECK(state.find(crash_hash(home)) == std::string::npos);
}

TEST(host_shade_allowed_built_code_runs) {
    CHECK(shade_4(kBuiltA) != nullptr);
    Home home;
    CHECK(home.install_mod("crash"));
    CHECK(home.write(L"tome\\state.json", allowed_with_key("", crash_hash_built(home, kBuiltA))));
    Session session;
    CHECK(session.start(home));
    CHECK_EQ(state_of("crash"), std::string("active"));
    CHECK_LOG("[crash] mode");
}

TEST(host_shade_other_built_code_asks_again) {
    // The same DLL, but another library where Shade looks: not what was allowed.
    CHECK(shade_4(kBuiltB) != nullptr);
    Home home;
    CHECK(home.install_mod("crash"));
    CHECK(home.write(L"tome\\state.json", allowed_with_key("", crash_hash_built(home, kBuiltA))));
    Session session;
    CHECK(session.start(home));
    CHECK_EQ(state_of("crash"), std::string("approval"));
    CHECK_EQ(approval_of("crash"), std::string("changed"));
    CHECK_NO_LOG("[crash] mode");
}

TEST(host_shade_signature_does_not_allow_built_code) {
    // On Windows an update signed by the allowed key runs
    // (host_signed_update_by_the_same_key_runs); the signature doesn't cover
    // the code Shade built, so here it's asked about.
    CHECK(shade_4(kBuiltA) != nullptr);
    Home home;
    CHECK(home.install_mod("crash"));
    const std::string key = sign_crash(home, 7);
    CHECK(home.write(L"tome\\state.json", allowed_with_key(key)));
    Session session;
    CHECK(session.start(home));
    CHECK_EQ(state_of("crash"), std::string("approval"));
    CHECK_NO_LOG("[crash] mode");
    CHECK_NO_LOG("signed by the same key as before");
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
