// SPDX-License-Identifier: GPL-3.0-or-later
// WebAssembly mods (docs/wasm-mods.md) through the real loader and WAMR:
// fixture modules made by tests/wasm_builder.h, written into the test's
// tome/mods.
#include "harness.h"
#include "test.h"
#include "wasm_builder.h"

#include "core/wasm_abi.h"
#include "core/wasm_imports.h"
#include "frames.h"
#include "mod_manager.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <windows.h>

using host::Home;
using host::Session;
using tome::mod_manager;
using namespace wasm_test;

namespace {

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

bool running(const char* id) {
    auto all = mod_manager().status();
    const tome::ModStatus* s = find_status(all, id);
    return s && s->running;
}

bool install_wasm(Home& home, const std::string& id, const std::string& module, const std::string& schema = {}) {
    const std::wstring dir = L"tome\\mods\\" + std::wstring(id.begin(), id.end());
    CreateDirectoryW((home.paths().home_dir + L"\\" + dir).c_str(), nullptr);
    const std::string config = schema.empty() ? "" : R"(, "config": "config.schema.json")";
    return home.write(dir + L"\\tome.mod.json",
                      R"({"schema": 1, "id": ")" + id +
                          R"(", "version": "1.0.0", "game": "1.27b", "tome": "*", "sync": "client", "wasm": "mod.wasm")" +
                          config + "}") &&
           home.write(dir + L"\\mod.wasm", module) &&
           (schema.empty() || home.write(dir + L"\\config.schema.json", schema));
}

// The parts most fixtures share: log (import 0) and register_command
// (import 1), a page of memory with "hello from wasm" at 16 and "wasmcmd"
// at 64, and tome_buffer/tome_callback that log what a callback gets.
struct Fixture {
    Builder b;
    uint32_t log, register_command;
    Fixture() {
        log = b.import("tome", "log", {I32, I32, I32}, {});
        register_command = b.import("tome", "register_command", {I32, I32, I32}, {I32});
        b.memory(1);
        b.data(16, "hello from wasm");
        b.data(64, "wasmcmd");
        b.data(80, "enabled");
    }
    Bytes log_at(int32_t ptr, int32_t len) const {
        Bytes out;
        for (const Bytes& part : {op::i32_const(1), op::i32_const(ptr), op::i32_const(len), op::call(log)})
            out.insert(out.end(), part.begin(), part.end());
        return out;
    }
    void callbacks(std::initializer_list<Bytes> body = {}) {
        b.export_func("tome_buffer", b.func({I32}, {I32}, {op::i32_const(1024)}));
        if (body.size() == 0) {
            b.export_func("tome_callback", b.func({I32, I32, I32}, {},
                                                  {op::i32_const(1), op::local_get(1), op::local_get(2), op::call(log)}));
        } else {
            b.export_func("tome_callback", b.func({I32, I32, I32}, {}, body));
        }
    }
    void load(std::initializer_list<Bytes> body) { b.export_func("tome_mod_load", b.func({}, {I32}, body)); }
    void enable(std::initializer_list<Bytes> body) { b.export_func("tome_mod_enable", b.func({}, {}, body)); }
    // tome_mod_enable registering /wasmcmd as callback 5.
    void enable_with_command() {
        enable({log_at(80, 7), op::i32_const(64), op::i32_const(7), op::i32_const(5), op::call(register_command),
                op::drop()});
    }
};

std::string hello_module() {
    Fixture f;
    f.load({f.log_at(16, 15), op::i32_const(0)});
    f.enable_with_command();
    f.callbacks();
    return f.b.bytes();
}

} // namespace

TEST(host_wasm_mod_waits_for_approval_and_wamr_waits_for_it) {
    Home home;
    CHECK(install_wasm(home, "wasm-hello", hello_module()));
    Session session;
    CHECK(session.start(home));
    CHECK_EQ(state_of("wasm-hello"), std::string("approval"));
    CHECK_LOG("wasm-hello: runs sandboxed code; turn it on to allow it");
    CHECK_NO_LOG("WAMR");  // never started for a mod that doesn't run
}

TEST(host_wasm_mod_runs) {
    Home home;
    CHECK(install_wasm(home, "wasm-hello", hello_module()));
    Session session;
    session.events = true;
    CHECK(session.start(home));
    mod_manager().set_enabled("wasm-hello", true);
    CHECK_LOG("WAMR 2.4.5 started (fast interpreter)");
    CHECK_LOG("[wasm-hello] hello from wasm");
    CHECK_LOG("Loaded wasm-hello 1.0.0 (WebAssembly)");
    CHECK_LOG("[wasm-hello] enabled");
    CHECK(running("wasm-hello"));
    // The command, whose arguments the module logs.
    CHECK_EQ(session.game.chat("/wasmcmd some args"), 1);
    CHECK_LOG("[wasm-hello] some args");
    // Off and on again: the command goes with it and comes back.
    mod_manager().set_enabled("wasm-hello", false);
    CHECK_EQ(session.game.chat("/wasmcmd gone"), 0);
    mod_manager().set_enabled("wasm-hello", true);
    CHECK_EQ(session.game.chat("/wasmcmd back"), 1);
    CHECK_LOG("[wasm-hello] back");
}

// Enabled again after a disable: the module hears that its old callback ids
// are gone (tome_callbacks_revoked) before tome_mod_enable registers anew.
TEST(host_wasm_reenable_reports_revoked_callbacks) {
    Fixture f;
    f.b.data(96, "revoked");
    f.load({op::i32_const(0)});
    f.enable_with_command();
    f.callbacks();
    f.b.export_func("tome_callbacks_revoked", f.b.func({}, {}, {f.log_at(96, 7)}));
    Home home;
    CHECK(install_wasm(home, "wasm-revoke", f.b.bytes()));
    Session session;
    session.events = true;  // chat commands
    CHECK(session.start(home));
    mod_manager().set_enabled("wasm-revoke", true);
    CHECK_LOG("[wasm-revoke] enabled");
    mod_manager().set_enabled("wasm-revoke", false);
    CHECK_NO_LOG("[wasm-revoke] revoked");  // not on the first enable, nor at the disable
    mod_manager().set_enabled("wasm-revoke", true);
    CHECK_LOG("[wasm-revoke] revoked");
    CHECK_EQ(session.game.chat("/wasmcmd again"), 1);
    CHECK_LOG("[wasm-revoke] again");
    CHECK(running("wasm-revoke"));
}

TEST(host_wasm_trap_in_load_is_a_load_failure) {
    Home home;
    Fixture f;
    f.load({op::unreachable(), op::i32_const(0)});
    CHECK(install_wasm(home, "wasm-trap", f.b.bytes()));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("wasm-trap", true);
    CHECK_LOG("[wasm-trap] the module trapped in tome_mod_load: Exception: unreachable");
    CHECK_LOG("Failed to load wasm-trap: crashed in tome_mod_load: the mod reported a failure");
    CHECK_EQ(state_of("wasm-trap"), std::string("error"));
}

TEST(host_wasm_endless_loop_runs_out_of_budget) {
    Home home;
    Fixture f;
    f.load({op::i32_const(0)});
    f.enable({op::loop_forever()});
    CHECK(install_wasm(home, "wasm-loop", f.b.bytes()));
    CHECK(home.install_mod("hello"));
    Session session;
    session.events = true;
    CHECK(session.start(home));
    mod_manager().set_enabled("hello", true);
    mod_manager().set_enabled("wasm-loop", true);
    CHECK_LOG("[wasm-loop] the module trapped in tome_mod_enable: Exception: instruction limit exceeded");
    CHECK_LOG("wasm-loop crashed in tome_mod_enable");
    CHECK(!running("wasm-loop"));
    // Everything else goes on.
    CHECK_EQ(session.game.chat("/hello after"), 1);
}

TEST(host_wasm_trap_in_a_callback_disables) {
    Home home;
    Fixture f;
    f.load({op::i32_const(0)});
    f.enable_with_command();
    f.callbacks({op::i32_const(70000), op::i32_load(), op::drop()});  // past the page
    CHECK(install_wasm(home, "wasm-oob", f.b.bytes()));
    Session session;
    session.events = true;
    CHECK(session.start(home));
    mod_manager().set_enabled("wasm-oob", true);
    CHECK(running("wasm-oob"));
    session.game.chat("/wasmcmd x");
    CHECK_LOG("[wasm-oob] the module trapped in tome_callback: Exception: out of bounds memory access");
    CHECK(!running("wasm-oob"));
    CHECK_EQ(session.game.chat("/wasmcmd again"), 0);  // its command went with it
}

TEST(host_wasm_bad_arguments_are_refused) {
    Home home;
    Fixture f;
    // A string running past the end of memory, then one with invalid UTF-8.
    f.b.data(100, "\xC3");
    f.load({f.log_at(65530, 100), f.log_at(100, 1), f.log_at(16, 15), op::i32_const(0)});
    CHECK(install_wasm(home, "wasm-args", f.b.bytes()));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("wasm-args", true);
    CHECK_LOG("[wasm-args] log: a string argument is outside the module's memory");
    CHECK_LOG("[wasm-args] log: a string argument is not valid UTF-8");
    CHECK_LOG("[wasm-args] hello from wasm");  // and it carries on
    CHECK(running("wasm-args"));
}

TEST(host_wasm_callbacks_need_the_exports) {
    Home home;
    Fixture f;
    f.load({op::i32_const(0)});
    f.enable_with_command();  // but no tome_buffer/tome_callback
    CHECK(install_wasm(home, "wasm-nocb", f.b.bytes()));
    Session session;
    session.events = true;
    CHECK(session.start(home));
    mod_manager().set_enabled("wasm-nocb", true);
    CHECK_LOG("[wasm-nocb] register_command: the module doesn't export tome_buffer and tome_callback");
    CHECK_EQ(session.game.chat("/wasmcmd x"), 0);
}

TEST(host_wasm_modules_tome_refuses) {
    struct Case {
        const char* id;
        std::string module;
        const char* error;
    };
    std::vector<Case> cases;
    {
        Builder b;
        b.import("tome", "nope", {}, {});
        b.export_func("tome_mod_load", b.func({}, {I32}, {op::i32_const(0)}));
        cases.push_back({"wasm-import", b.bytes(), "imports tome.nope, which Tome doesn't provide"});
    }
    {
        Builder b;
        b.import("env", "log", {I32, I32, I32}, {});
        b.export_func("tome_mod_load", b.func({}, {I32}, {op::i32_const(0)}));
        cases.push_back({"wasm-env", b.bytes(), "imports env.log, which Tome doesn't provide"});
    }
    {
        Builder b;
        b.import("tome", "log", {I32}, {});  // the wrong type
        b.export_func("tome_mod_load", b.func({}, {I32}, {op::i32_const(0)}));
        cases.push_back({"wasm-sig", b.bytes(), "imports tome.log, which Tome doesn't provide (or not with that type)"});
    }
    {
        Builder b;
        b.import_global("tome", "g");
        b.export_func("tome_mod_load", b.func({}, {I32}, {op::i32_const(0)}));
        cases.push_back({"wasm-global", b.bytes(), "imports tome.g, which isn't a function"});
    }
    {
        Builder b;
        b.export_func("tome_mod_load", b.func({}, {}, {}));
        cases.push_back({"wasm-type", b.bytes(), "exports tome_mod_load with the wrong type"});
    }
    {
        Builder b;
        b.export_func("tome_mod_enable", b.func({}, {}, {}));
        cases.push_back({"wasm-noload", b.bytes(), "doesn't export tome_mod_load"});
    }
    {
        // A start function calling an import: WAMR would run it while
        // instantiating, before the import could find its mod.
        Fixture f;
        f.load({op::i32_const(0)});
        f.b.start(f.b.func({}, {}, {f.log_at(16, 15)}));
        cases.push_back({"wasm-start", f.b.bytes(), "has a start function, which would run before Tome can watch it"});
    }
    {
        Builder b;
        b.export_func("tome_mod_load", b.func({}, {I32}, {op::i32_const(0)}));
        b.export_func("__wasm_call_ctors", b.func({}, {}, {op::loop_forever()}));
        cases.push_back(
            {"wasm-ctors", b.bytes(), "exports __wasm_call_ctors, which would run before Tome can watch it"});
    }
    {
        // A page past the limit: max_memory_pages caps growth, not what a
        // module starts with.
        Builder b;
        b.memory(1025);
        b.export_func("tome_mod_load", b.func({}, {I32}, {op::i32_const(0)}));
        cases.push_back({"wasm-big-memory", b.bytes(), "starts with more than 64 MiB of memory"});
    }
    cases.push_back({"wasm-junk", std::string("\0asm but not really", 19), "not a valid WebAssembly module"});
    // 15 bytes declaring an element section of ~50M segments: WAMR sizes
    // its array from the count before reading them (gigabytes, found by
    // fuzz_wamr; fuzz/regressions/wamr). Tome's heap for WAMR refuses it.
    cases.push_back({"wasm-huge-count", std::string("\0asm\1\0\0\0\x09\x05\xff\xff\xff\x17\x0a", 15),
                     "not a valid WebAssembly module: WASM module load failed: allocate memory failed"});

    Home home;
    for (const Case& c : cases) CHECK(install_wasm(home, c.id, c.module));
    Session session;
    CHECK(session.start(home));
    for (const Case& c : cases) {
        mod_manager().set_enabled(c.id, true);
        CHECK_LOG((std::string("Failed to load ") + c.id + ": " + c.error).c_str());
        CHECK_EQ(state_of(c.id), std::string("error"));
    }
}

TEST(host_wasm_settings) {
    Fixture f;
    const uint32_t setting_int = f.b.import("tome", "setting_int", {I32, I32, I32}, {I32});
    const uint32_t set_setting_int = f.b.import("tome", "set_setting_int", {I32, I32, I32}, {I32});
    const uint32_t setting_string = f.b.import("tome", "setting_string", {I32, I32, I32, I32}, {I32});
    const uint32_t on_changed = f.b.import("tome", "on_setting_changed", {I32}, {I32});
    f.b.data(300, "count");
    f.b.data(310, "name");
    f.b.data(320, "count is 3");
    f.load({op::i32_const(0)});
    f.enable({
        op::i32_const(9), op::call(on_changed), op::drop(),
        // if setting_int("count", -1) == 3: log "count is 3"
        op::i32_const(300), op::i32_const(5), op::i32_const(-1), op::call(setting_int), op::i32_const(3), op::i32_eq(),
        op::if_(), f.log_at(320, 10), op::end(),
        // log setting_string("name") (3 bytes, into 200)
        op::i32_const(310), op::i32_const(4), op::i32_const(200), op::i32_const(16), op::call(setting_string),
        op::drop(), f.log_at(200, 3),
        op::i32_const(300), op::i32_const(5), op::i32_const(7), op::call(set_setting_int), op::drop(),
    });
    f.callbacks();  // logs the changed setting's key
    const std::string schema = R"({"type": "object", "properties": {
        "count": {"type": "integer", "minimum": 0, "maximum": 10, "default": 3},
        "name": {"type": "string", "default": "abc"}}})";
    Home home;
    CHECK(install_wasm(home, "wasm-settings", f.b.bytes(), schema));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("wasm-settings", true);
    CHECK_LOG("[wasm-settings] count is 3");
    CHECK_LOG("[wasm-settings] abc");
    CHECK_LOG("[wasm-settings] count");  // on_setting_changed, after set_setting_int
    CHECK(running("wasm-settings"));
    std::string saved;
    CHECK(home.read(L"tome\\mods\\config\\wasm-settings.json", saved));
    CHECK(saved.find("7") != std::string::npos);
}

namespace {

DWORD WINAPI enable_hello(LPVOID) {
    mod_manager().set_enabled("wasm-hello", true);
    return 0;
}

} // namespace

TEST(host_wasm_first_load_on_another_thread) {
    // As in the game: the first load and enable on the bootstrap thread,
    // which then ends; later calls on the main thread.
    Home home;
    CHECK(install_wasm(home, "wasm-hello", hello_module()));
    Session session;
    session.events = true;
    CHECK(session.start(home));
    HANDLE thread = CreateThread(nullptr, 0, enable_hello, nullptr, 0, nullptr);
    CHECK(thread != nullptr);
    if (!thread) return;
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    CHECK_LOG("[wasm-hello] enabled");
    CHECK_EQ(session.game.chat("/wasmcmd on the main thread"), 1);
    CHECK_LOG("[wasm-hello] on the main thread");
    CHECK(running("wasm-hello"));
}

// --- the C example (sdk/examples/hello-wasm), built by tome_add_wasm_mod ----

#ifdef TOME_TEST_WASM_EXAMPLE
TEST(host_wasm_example_runs) {
    Home home;
    CHECK(home.install_mod("hello-wasm"));
    Session session;
    session.events = true;
    CHECK(session.start(home));
    mod_manager().set_enabled("hello-wasm", true);
    CHECK_LOG("[hello-wasm] loaded, Warcraft III 1.27b");
    CHECK_LOG("[hello-wasm] enabled");
    CHECK(running("hello-wasm"));

    // A command with a string setting. (What it publishes needs the game's
    // main thread, which only the game has: the e2e test checks that.)
    CHECK_EQ(session.game.chat("/hellowasm host"), 1);
    CHECK_LOG("[hello-wasm] /hellowasm command: Hello, host.");
    mod_manager().set_enabled("hello-wasm", false);
    CHECK_LOG("[hello-wasm] disabled");
}

TEST(host_wasm_example_natives) {
    Home home;
    CHECK(home.install_mod("hello-wasm"));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("hello-wasm", true);
    session.game.load_map();

    using Add = int32_t (*)(int32_t, int32_t);
    using Half = uint32_t (*)(const float*);
    using Same = uint32_t (*)(uint32_t);
    auto add = reinterpret_cast<Add>(session.game.native("HelloWasmAdd"));
    auto half = reinterpret_cast<Half>(session.game.native("HelloWasmHalf"));
    auto same = reinterpret_cast<Same>(session.game.native("HelloWasmSame"));
    CHECK(add && half && same);
    if (!add || !half || !same) return;
    CHECK_EQ(add(40, 2), 42);
    CHECK_EQ(add(-5, 3), -2);
    const float x = 5.0f;
    const uint32_t bits = half(&x);  // reals come in by pointer, go out as bits
    float result = 0;
    std::memcpy(&result, &bits, 4);
    CHECK(result == 2.5f);
    CHECK_EQ(same(0x100007), 0x100007u);  // its own unit argument back
    CHECK(running("hello-wasm"));

    // Disabled: the natives answer 0 and the map keeps running.
    mod_manager().set_enabled("hello-wasm", false);
    CHECK_EQ(add(40, 2), 0);
}
#endif

// --- the Rust example (sdk/rust/wasm/examples/hello-wasm-rs, tome-wasm) ----

#ifdef TOME_TEST_WASM_RUST_EXAMPLE
TEST(host_wasm_rust_example_runs) {
    Home home;
    CHECK(home.install_mod("hello-wasm-rs"));
    Session session;
    session.events = true;
    CHECK(session.start(home));
    mod_manager().set_enabled("hello-wasm-rs", true);
    CHECK_LOG("[hello-wasm-rs] loaded, Warcraft III 1.27b");
    CHECK_LOG("[hello-wasm-rs] enabled");
    CHECK(running("hello-wasm-rs"));

    // A command (a closure) with a string and an integer setting.
    CHECK_EQ(session.game.chat("/hellowasmrs host"), 1);
    CHECK_LOG("[hello-wasm-rs] /hellowasmrs command: Hello, host.");
    mod_manager().set_enabled("hello-wasm-rs", false);
    CHECK_LOG("[hello-wasm-rs] disabled");
    CHECK(!running("hello-wasm-rs"));

    // Enabled again: registered again (the crate dropped its closures).
    mod_manager().set_enabled("hello-wasm-rs", true);
    CHECK(running("hello-wasm-rs"));
    CHECK_EQ(session.game.chat("/hellowasmrs again"), 1);
    CHECK_LOG("[hello-wasm-rs] /hellowasmrs command: Hello, again.");
}

TEST(host_wasm_rust_example_natives) {
    Home home;
    CHECK(home.install_mod("hello-wasm-rs"));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("hello-wasm-rs", true);
    session.game.load_map();

    using Add = int32_t (*)(int32_t, int32_t);
    using Half = uint32_t (*)(const float*);
    using Same = uint32_t (*)(uint32_t);
    auto add = reinterpret_cast<Add>(session.game.native("HelloWasmRsAdd"));
    auto half = reinterpret_cast<Half>(session.game.native("HelloWasmRsHalf"));
    auto same = reinterpret_cast<Same>(session.game.native("HelloWasmRsSame"));
    CHECK(add && half && same);
    if (!add || !half || !same) return;
    CHECK_EQ(add(40, 2), 42);
    CHECK_EQ(add(INT32_MAX, 1), INT32_MIN);  // wrapping_add: no panic
    const float x = 5.0f;
    const uint32_t bits = half(&x);
    float result = 0;
    std::memcpy(&result, &bits, 4);
    CHECK(result == 2.5f);
    CHECK_EQ(same(0x100007), 0x100007u);
    CHECK(running("hello-wasm-rs"));

    mod_manager().set_enabled("hello-wasm-rs", false);
    CHECK_EQ(add(40, 2), 0);
}
#endif

TEST(host_wasm_native_checks) {
    // A native returning a handle it wasn't given, and one that traps.
    Fixture f;
    const uint32_t reg = f.b.import("tome", "register_native", {I32, I32, I32}, {I32});
    f.b.data(400, "WasmMadeUp");
    f.b.data(420, "WasmTrap");
    f.b.data(440, "WasmCode");
    f.load({op::i32_const(0)});
    f.enable({op::i32_const(400), op::i32_const(10), op::i32_const(1), op::call(reg), op::drop(),
              op::i32_const(420), op::i32_const(8), op::i32_const(2), op::call(reg), op::drop(),
              op::i32_const(440), op::i32_const(8), op::i32_const(3), op::call(reg), op::drop()});
    f.callbacks();
    // tome_native(id, args, count): id 1 returns 0x100099; id 2 traps.
    f.b.export_func("tome_native",
                    f.b.func({I32, I32, I32}, {I32},
                             {op::local_get(0), op::i32_const(2), op::i32_eq(), op::if_(), op::unreachable(), op::end(),
                              op::i32_const(0x100099)}));
    Home home;
    const std::string id = "wasm-natives";
    CreateDirectoryW((home.paths().home_dir + L"\\tome\\mods\\wasm-natives").c_str(), nullptr);
    CHECK(home.write(L"tome\\mods\\wasm-natives\\tome.mod.json",
                     R"({"schema": 1, "id": "wasm-natives", "version": "1.0.0", "game": "1.27b", "tome": "*",
                         "sync": "client", "wasm": "mod.wasm", "natives": [
                         "native WasmMadeUp takes unit u returns unit",
                         "native WasmTrap takes integer x returns integer",
                         "native WasmCode takes nothing returns code"]})"));
    CHECK(home.write(L"tome\\mods\\wasm-natives\\mod.wasm", f.b.bytes()));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("wasm-natives", true);
    CHECK_LOG("[wasm-natives] register_native(WasmCode): natives returning code aren't available");
    session.game.load_map();
    using Fn = uint32_t (*)(uint32_t);
    auto made_up = reinterpret_cast<Fn>(session.game.native("WasmMadeUp"));
    auto trap = reinterpret_cast<Fn>(session.game.native("WasmTrap"));
    CHECK(made_up && trap);
    if (!made_up || !trap) return;
    CHECK_EQ(made_up(0x100007), 0u);  // not its argument: null
    CHECK_LOG("[wasm-natives] native WasmMadeUp returned handle 1048729, which isn't one of its own unit arguments");
    CHECK(running("wasm-natives"));
    CHECK_EQ(trap(1), 0u);  // a trap: 0, and the mod stops
    CHECK_LOG("[wasm-natives] the module trapped in tome_native: Exception: unreachable");
    CHECK_LOG("wasm-natives crashed in native WasmTrap");
    CHECK(!running("wasm-natives"));
}

TEST(host_wasm_native_with_another_signature_returns_0) {
    // tome.mod.json gives a native another signature while the module stays
    // loaded: once a map binds the new one, the registration made for the
    // old one isn't called (its integer would reach the map as a string).
    Fixture f;
    const uint32_t reg = f.b.import("tome", "register_native", {I32, I32, I32}, {I32});
    f.b.data(400, "WasmSig");
    f.load({op::i32_const(0)});
    f.enable({op::i32_const(400), op::i32_const(7), op::i32_const(1), op::call(reg), op::drop()});
    f.callbacks();
    f.b.export_func("tome_native", f.b.func({I32, I32, I32}, {I32}, {op::i32_const(7)}));
    auto metadata = [](const char* result) {
        return std::string(R"({"schema": 1, "id": "wasm-sig", "version": "1.0.0", "game": "1.27b", "tome": "*",
                               "sync": "client", "wasm": "mod.wasm", "natives": [
                               "native WasmSig takes nothing returns )") +
               result + R"("]})";
    };
    Home home;
    CreateDirectoryW((home.paths().home_dir + L"\\tome\\mods\\wasm-sig").c_str(), nullptr);
    CHECK(home.write(L"tome\\mods\\wasm-sig\\tome.mod.json", metadata("integer")));
    CHECK(home.write(L"tome\\mods\\wasm-sig\\mod.wasm", f.b.bytes()));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("wasm-sig", true);
    session.game.load_map();
    using Fn = uint32_t (*)();
    auto native = reinterpret_cast<Fn>(session.game.native("WasmSig"));
    CHECK(native);
    if (!native) return;
    CHECK_EQ(native(), 7u);
    CHECK(home.write(L"tome\\mods\\wasm-sig\\tome.mod.json", metadata("string")));
    mod_manager().refresh();
    CHECK_EQ(native(), 7u);  // the map that runs still has the old one
    session.game.load_map();
    CHECK_EQ(native(), 0u);
    CHECK_LOG("Native WasmSig: the map has it as ()S, but its mod registered it as ()I");
    CHECK(running("wasm-sig"));
}

TEST(host_wasm_config_files) {
    Fixture f;
    const uint32_t write = f.b.import("tome", "config_write", {I32, I32, I32, I32}, {I32});
    const uint32_t read = f.b.import("tome", "config_read", {I32, I32, I32, I32}, {I32});
    f.b.data(300, "state.json");
    f.b.data(320, "{\"n\":1}");
    f.b.data(340, "../escape");
    f.b.data(360, "missing");
    f.b.data(380, "read back");
    f.b.data(400, "none");
    f.load({
        // config_write("state.json", "{\"n\":1}"), then read it into 500 and log it.
        op::i32_const(300), op::i32_const(10), op::i32_const(320), op::i32_const(7), op::call(write), op::drop(),
        op::i32_const(300), op::i32_const(10), op::i32_const(500), op::i32_const(64), op::call(read),
        op::i32_const(7), op::i32_eq(), op::if_(), f.log_at(380, 9), f.log_at(500, 7), op::end(),
        // A name with a folder in it: refused.
        op::i32_const(340), op::i32_const(9), op::i32_const(320), op::i32_const(7), op::call(write), op::drop(),
        // A file that isn't there: UINT32_MAX.
        op::i32_const(360), op::i32_const(7), op::i32_const(500), op::i32_const(64), op::call(read),
        op::i32_const(-1), op::i32_eq(), op::if_(), f.log_at(400, 4), op::end(),
        op::i32_const(0),
    });
    Home home;
    CHECK(install_wasm(home, "wasm-files", f.b.bytes()));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("wasm-files", true);
    CHECK_LOG("[wasm-files] read back");
    CHECK_LOG("[wasm-files] {\"n\":1}");
    CHECK_LOG("[wasm-files] config_write(../escape): use a plain file name");
    CHECK_LOG("[wasm-files] none");
    std::string saved;
    CHECK(home.read(L"tome\\mods\\config\\wasm-files\\state.json", saved));
    CHECK_EQ(saved, std::string("{\"n\":1}"));
    CHECK(!home.exists(L"tome\\mods\\config\\escape"));
    CHECK(running("wasm-files"));
}

TEST(host_wasm_game_file_names_stay_in_the_game) {
    // With "Allow Local Files" on, Storm reads loose files first: names that
    // leave the game folder are refused before Storm sees them.
    Fixture f;
    const uint32_t read = f.b.import("tome", "read_game_file", {I32, I32, I32, I32}, {I32});
    const uint32_t toc = f.b.import("tome", "load_toc", {I32, I32}, {I32});
    f.b.data(300, "..\\secret.txt");
    f.b.data(320, "C:\\Users\\x.txt");
    f.b.data(340, "UI\\x.toc");
    // Tome's own folder, in the game folder by default: state.json, the log,
    // every mod's config files. Win32 reads "TOME." as "tome".
    f.b.data(360, ".\\TOME.\\state.json");
    f.load({op::i32_const(300), op::i32_const(13), op::i32_const(500), op::i32_const(64), op::call(read), op::drop(),
            op::i32_const(320), op::i32_const(14), op::i32_const(500), op::i32_const(64), op::call(read), op::drop(),
            op::i32_const(300), op::i32_const(13), op::call(toc), op::drop(),
            op::i32_const(340), op::i32_const(8), op::i32_const(500), op::i32_const(64), op::call(read), op::drop(),
            op::i32_const(360), op::i32_const(18), op::i32_const(500), op::i32_const(64), op::call(read), op::drop(),
            op::i32_const(0)});
    Home home;
    CHECK(install_wasm(home, "wasm-paths", f.b.bytes()));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("wasm-paths", true);
    CHECK_LOG("[wasm-paths] read_game_file(..\\secret.txt): refused, the name leaves the game's files");
    CHECK_LOG("[wasm-paths] read_game_file(C:\\Users\\x.txt): refused");
    CHECK_LOG("[wasm-paths] load_toc(..\\secret.txt): refused");
    CHECK_NO_LOG("read_game_file(UI\\x.toc): refused");  // a plain name in the archives is fine
    CHECK_LOG("[wasm-paths] read_game_file(.\\TOME.\\state.json): refused, that's Tome's own folder");
    CHECK(running("wasm-paths"));
}

TEST(host_wasm_bad_topics_are_explained) {
    Fixture f;
    const uint32_t publish = f.b.import("tome", "publish", {I32, I32, I32, I32}, {I32});
    f.b.data(300, "my-mod/news");
    f.load({op::i32_const(300), op::i32_const(11), op::i32_const(300), op::i32_const(4), op::call(publish), op::drop(),
            op::i32_const(0)});
    Home home;
    CHECK(install_wasm(home, "wasm-topics", f.b.bytes()));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("wasm-topics", true);
    CHECK_LOG("[wasm-topics] publish(my-mod/news): a topic is 1-64 of a-z 0-9 . - _");
}

// Imports no other fixture calls, each with arguments that work and ones
// that don't. Without the game's UI (the fake game has none) the frame
// imports refuse every handle. Each call whose result is the expected one
// logs "okNN" (checked below); the rest show in Tome's log.
TEST(host_wasm_imports_checked) {
    Fixture f;
    const uint32_t mod_id = f.b.import("tome", "mod_id", {I32, I32}, {I32});
    const uint32_t mod_version = f.b.import("tome", "mod_version", {I32, I32}, {I32});
    const uint32_t setting_bool = f.b.import("tome", "setting_bool", {I32, I32, I32}, {I32});
    const uint32_t setting_number = f.b.import("tome", "setting_number", {I32, I32, F64}, {F64});
    const uint32_t set_bool = f.b.import("tome", "set_setting_bool", {I32, I32, I32}, {I32});
    const uint32_t set_number = f.b.import("tome", "set_setting_number", {I32, I32, F64}, {I32});
    const uint32_t set_string = f.b.import("tome", "set_setting_string", {I32, I32, I32, I32}, {I32});
    const uint32_t ui_root = f.b.import("tome", "ui_root", {I32}, {I32});
    const uint32_t frame_create = f.b.import("tome", "frame_create", {I32, I32, I32, I32, I32}, {I32});
    const uint32_t set_point = f.b.import("tome", "frame_set_point", {I32, I32, I32, I32, F32, F32}, {I32});
    const uint32_t set_text = f.b.import("tome", "frame_set_text", {I32, I32, I32}, {I32});
    const uint32_t set_visible = f.b.import("tome", "frame_set_visible", {I32, I32}, {I32});
    const uint32_t set_animation = f.b.import("tome", "frame_set_animation", {I32, I32, I32}, {I32});
    const uint32_t on_click = f.b.import("tome", "frame_on_click", {I32, I32}, {I32});
    const uint32_t load_fdf = f.b.import("tome", "load_fdf", {I32, I32}, {I32});
    const uint32_t load_toc = f.b.import("tome", "load_toc", {I32, I32}, {I32});
    const uint32_t keybind_name = f.b.import("tome", "keybind_name", {I32, I32, I32, I32}, {I32});
    const uint32_t popup = f.b.import("tome", "popup", {I32, I32, I32}, {I32});
    const uint32_t popup_close = f.b.import("tome", "popup_close", {I32}, {I32});
    const uint32_t restart_game = f.b.import("tome", "restart_game", {}, {I32});
    const uint32_t selection = f.b.import("tome", "selection", {I32, I32, I32, I32}, {I32});
    const uint32_t shadows = f.b.import("tome", "map_script_shadows", {I32, I32, I32}, {I32});
    const uint32_t publish = f.b.import("tome", "publish", {I32, I32, I32, I32}, {I32});
    const uint32_t register_native = f.b.import("tome", "register_native", {I32, I32, I32}, {I32});
    const uint32_t code_execute = f.b.import("tome", "jass_code_execute", {I32}, {I32});
    f.b.data(200, "flag");
    f.b.data(210, "speed");
    f.b.data(220, "name");
    f.b.data(230, "Zed");
    f.b.data(240, "Frame");
    f.b.data(250, "x.fdf");
    f.b.data(260, "x.toc");
    f.b.data(270, "nokey");
    f.b.data(280, "Title");
    f.b.data(290, "Text");
    f.b.data(300, "Yes");
    f.b.data(400, "t.big");
    f.b.data(420, "HelloX");
    // The popup: options {300, 3} at 320, the spec at 340.
    const std::string option{'\x2c', '\x01', 0, 0, 3, 0, 0, 0};
    f.b.data(320, option);
    std::string spec;
    for (uint32_t v : {280u, 5u, 290u, 4u, 320u, 1u, 0u, 0u, 0u}) {
        for (int i = 0; i < 4; ++i) spec.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
    f.b.data(340, spec);
    constexpr int32_t kOut = 2000, kCount = 2600, kLeader = 2604, kId = 2608, kOutside = 70000;

    Bytes body;
    int checks = 0;
    auto add = [&](std::initializer_list<Bytes> parts) {
        for (const Bytes& p : parts) body.insert(body.end(), p.begin(), p.end());
    };
    // `call` leaves an i32; logs "okNN" if it's `expected`.
    auto expect = [&](std::initializer_list<Bytes> call, int32_t expected) {
        char marker[8];
        std::snprintf(marker, sizeof(marker), "ok%02d", ++checks);
        const int32_t at = 1100 + 8 * checks;
        f.b.data(static_cast<uint32_t>(at), marker);
        add(call);
        add({op::i32_const(expected), op::i32_eq(), op::if_(), f.log_at(at, 4), op::end()});
    };
    using namespace op;
    expect({i32_const(kOut), i32_const(64), call(mod_id)}, 8);  // "wasm-api"
    expect({i32_const(kOut), i32_const(64), call(mod_version)}, 5);
    expect({i32_const(200), i32_const(4), i32_const(7), call(setting_bool)}, 0);
    expect({i32_const(200), i32_const(4), i32_const(1), call(set_bool)}, TOME_OK);
    expect({i32_const(200), i32_const(4), i32_const(7), call(setting_bool)}, 1);
    expect({i32_const(kOutside), i32_const(4), i32_const(7), call(setting_bool)}, 7);  // key outside memory
    expect({i32_const(kOutside), i32_const(4), i32_const(1), call(set_bool)}, TOME_ERR_INVALID_ARGUMENT);
    expect({i32_const(210), i32_const(5), f64_const(3.5), call(set_number)}, TOME_OK);
    expect({i32_const(kOutside), i32_const(5), f64_const(3.5), call(set_number)}, TOME_ERR_INVALID_ARGUMENT);
    add({i32_const(210), i32_const(5), f64_const(1.0), call(setting_number), drop()});
    add({i32_const(kOutside), i32_const(5), f64_const(1.0), call(setting_number), drop()});
    expect({i32_const(220), i32_const(4), i32_const(230), i32_const(3), call(set_string)}, TOME_OK);
    expect({i32_const(220), i32_const(4), i32_const(kOutside), i32_const(3), call(set_string)},
           TOME_ERR_INVALID_ARGUMENT);
    expect({i32_const(TOME_UI_MAIN_MENU), call(ui_root)}, 0);  // no main menu without the game
    expect({i32_const(240), i32_const(5), i32_const(99), i32_const(0), i32_const(0), call(frame_create)}, 0);
    expect({i32_const(kOutside), i32_const(5), i32_const(99), i32_const(0), i32_const(0), call(frame_create)}, 0);
    expect({i32_const(99), i32_const(0), i32_const(99), i32_const(0), f32_const(0), f32_const(0), call(set_point)},
           TOME_ERR_INVALID_ARGUMENT);
    expect({i32_const(99), i32_const(290), i32_const(4), call(set_text)}, TOME_ERR_INVALID_ARGUMENT);
    expect({i32_const(99), i32_const(1), call(set_visible)}, TOME_ERR_INVALID_ARGUMENT);
    expect({i32_const(99), i32_const(kOut), i32_const(1), call(set_animation)}, TOME_ERR_INVALID_ARGUMENT);
    expect({i32_const(99), i32_const(9), call(on_click)}, TOME_ERR_INVALID_ARGUMENT);
    add({i32_const(250), i32_const(5), call(load_fdf), drop()});
    expect({i32_const(kOutside), i32_const(5), call(load_fdf)}, TOME_ERR_INVALID_ARGUMENT);
    add({i32_const(260), i32_const(5), call(load_toc), drop()});
    expect({i32_const(270), i32_const(5), i32_const(kOut), i32_const(64), call(keybind_name)}, 0);
    expect({i32_const(kOutside), i32_const(5), i32_const(kOut), i32_const(64), call(keybind_name)}, -1);
    add({i32_const(340), i32_const(11), i32_const(kId), call(popup), drop()});
    add({i32_const(kId), i32_load(), call(popup_close), drop()});
    expect({i32_const(kOutside), i32_const(11), i32_const(kId), call(popup)}, TOME_ERR_INVALID_ARGUMENT);
    add({call(restart_game), drop()});
    expect({i32_const(kOut), i32_const(512), i32_const(kCount), i32_const(kLeader), call(selection)}, 0);
    expect({i32_const(kOut), i32_const(512), i32_const(kOutside), i32_const(kLeader), call(selection)}, -1);
    expect({i32_const(kOut), i32_const(512), i32_const(kCount), call(shadows)}, 0);
    expect({i32_const(kOut), i32_const(512), i32_const(kOutside), call(shadows)}, -1);
    expect({i32_const(400), i32_const(5), i32_const(0), i32_const(kOutside), call(publish)}, -1);
    expect({i32_const(420), i32_const(6), i32_const(1), call(register_native)}, TOME_ERR_INVALID_ARGUMENT);
    add({i32_const(0), call(code_execute), drop()});

    f.load({i32_const(0)});
    f.b.export_func("tome_mod_enable", f.b.func({}, {}, {body}));
    f.callbacks();
    Home home;
    CHECK(install_wasm(home, "wasm-api", f.b.bytes(), R"({"type": "object", "properties": {
        "flag": {"type": "boolean", "default": false},
        "speed": {"type": "number", "default": 1.5},
        "name": {"type": "string", "default": "Ann"}}})"));
    Session session;
    CHECK(session.start(home));
    tome::frames::set_main_thread(GetCurrentThreadId());
    mod_manager().set_enabled("wasm-api", true);
    CHECK(running("wasm-api"));
    for (int i = 1; i <= checks; ++i) {
        char marker[16];
        std::snprintf(marker, sizeof(marker), "[wasm-api] ok%02d", i);
        CHECK_LOG(marker);
    }
    CHECK_LOG("[wasm-api] frame_create: frame handle 99 isn't a frame that exists now");
    CHECK_LOG("[wasm-api] publish: the message is outside the module's memory or larger than");
    CHECK_LOG("[wasm-api] register_native(HelloX): the module doesn't export tome_native and tome_buffer");
    std::string config;
    CHECK(home.read(L"tome\\mods\\config\\wasm-api.json", config));
    CHECK(config.find("\"Zed\"") != std::string::npos);
    CHECK(config.find("3.5") != std::string::npos);
}

// More of what Tome refuses a module: callbacks without the exports they
// need, config files too large or outside its memory, natives that aren't
// its own, a string result that isn't one, and a module file too large.
TEST(host_wasm_more_refusals) {
    Fixture f;  // no tome_buffer/tome_callback
    const uint32_t on_setting = f.b.import("tome", "on_setting_changed", {I32}, {I32});
    const uint32_t on_event = f.b.import("tome", "on_event", {I32, I32}, {I32});
    const uint32_t on_ui = f.b.import("tome", "on_ui", {I32, I32}, {I32});
    const uint32_t write = f.b.import("tome", "config_write", {I32, I32, I32, I32}, {I32});
    const uint32_t read = f.b.import("tome", "config_read", {I32, I32, I32, I32}, {I32});
    f.b.data(300, "big.bin");
    f.b.data(320, "out.bin");
    f.enable({op::i32_const(1), op::call(on_setting), op::drop(),
              op::i32_const(TOME_EVENT_TICK), op::i32_const(2), op::call(on_event), op::drop(),
              op::i32_const(TOME_UI_MAIN_MENU), op::i32_const(3), op::call(on_ui), op::drop(),
              op::i32_const(320), op::i32_const(7), op::i32_const(70000), op::i32_const(8), op::call(write), op::drop(),
              op::i32_const(300), op::i32_const(7), op::i32_const(500), op::i32_const(64), op::call(read), op::drop()});
    f.load({op::i32_const(0)});
    Home home;
    CHECK(install_wasm(home, "wasm-refused", f.b.bytes()));
    CreateDirectoryW(home.paths().mod_config_dir.c_str(), nullptr);
    CreateDirectoryW((home.paths().mod_config_dir + L"\\wasm-refused").c_str(), nullptr);
    CHECK(home.write(L"tome\\mods\\config\\wasm-refused\\big.bin", std::string(tome::wasm::kMaxFileSize + 1, 'x')));
    // A module file over the limit: refused by its size, before it's read.
    {
        Builder b;
        b.export_func("tome_mod_load", b.func({}, {I32}, {op::i32_const(0)}));
        std::string huge = b.bytes();
        huge.resize(tome::wasm::kMaxModuleSize + 1);
        CHECK(install_wasm(home, "wasm-huge", huge));
    }
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("wasm-refused", true);
    for (const char* import : {"on_setting_changed", "on_event", "on_ui"}) {
        CHECK_LOG(std::string("[wasm-refused] ") + import + ": the module doesn't export tome_buffer and tome_callback");
    }
    CHECK_LOG("[wasm-refused] config_write: the data is outside the module's memory or larger than 1 MiB");
    CHECK_LOG("[wasm-refused] config_read: the file is larger than 1 MiB");
    mod_manager().set_enabled("wasm-huge", true);
    CHECK_LOG("the module is larger than 16 MiB");
}

TEST(host_wasm_natives_not_its_own_and_bad_results) {
    Fixture f;
    const uint32_t reg = f.b.import("tome", "register_native", {I32, I32, I32}, {I32});
    f.b.data(400, "WasmUndeclared");
    f.b.data(420, "HelloLog");
    f.b.data(440, "WasmVoid");
    f.b.data(460, "WasmBadString");
    f.load({op::i32_const(0)});
    f.enable({op::i32_const(400), op::i32_const(14), op::i32_const(1), op::call(reg), op::drop(),
              op::i32_const(420), op::i32_const(8), op::i32_const(2), op::call(reg), op::drop(),
              op::i32_const(440), op::i32_const(8), op::i32_const(3), op::call(reg), op::drop(),
              op::i32_const(460), op::i32_const(13), op::i32_const(4), op::call(reg), op::drop()});
    f.callbacks();
    // tome_native: a string result outside the module's memory for id 4.
    f.b.export_func("tome_native", f.b.func({I32, I32, I32}, {I32}, {op::i32_const(70000)}));
    Home home;
    CHECK(home.install_mod("hello"));
    CreateDirectoryW((home.paths().home_dir + L"\\tome\\mods\\wasm-others").c_str(), nullptr);
    CHECK(home.write(L"tome\\mods\\wasm-others\\tome.mod.json",
                     R"({"schema": 1, "id": "wasm-others", "version": "1.0.0", "game": "1.27b", "tome": "*",
                         "sync": "client", "wasm": "mod.wasm", "natives": [
                         "native WasmVoid takes nothing returns nothing",
                         "native WasmBadString takes nothing returns string"]})"));
    CHECK(home.write(L"tome\\mods\\wasm-others\\mod.wasm", f.b.bytes()));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("hello", true);
    mod_manager().set_enabled("wasm-others", true);
    CHECK_LOG("[wasm-others] register_native(WasmUndeclared): not declared in tome.mod.json \"natives\"");
    CHECK_LOG("[wasm-others] register_native(HelloLog): declared by another mod");
    session.game.load_map();
    using Fn = uint32_t (*)();
    auto void_native = reinterpret_cast<Fn>(session.game.native("WasmVoid"));
    auto bad_string = reinterpret_cast<Fn>(session.game.native("WasmBadString"));
    CHECK(void_native && bad_string);
    if (!void_native || !bad_string) return;
    void_native();
    CHECK_EQ(bad_string(), 0u);
    CHECK_LOG("[wasm-others] native WasmBadString: the string it returned isn't a NUL-terminated UTF-8 string");
    CHECK(running("wasm-others"));
}
