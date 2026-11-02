// SPDX-License-Identifier: GPL-3.0-or-later
// The mod manager's less travelled paths: mods' settings files that are
// missing or broken, discovery problems, DLLs that don't load, reloads,
// dependents turned off with what they need, signatures that don't count,
// and Tome's own files when they're broken.
#include "harness.h"
#include "test.h"
#include "wasm_builder.h"

#include "core/json.h"
#include "core/signature.h"
#include "mod_manager.h"

#include <windows.h>

#include <cstring>
#include <string>

using host::Home;
using host::Session;
using tome::mod_manager;

namespace {

std::wstring wide(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

// A mod folder with tome.mod.json (`extra`: more of its keys, each with a
// leading comma) and the given files.
bool write_mod(Home& home, const std::string& id, const std::string& extra,
               std::initializer_list<std::pair<const char*, std::string>> files = {}) {
    const std::wstring dir = L"tome\\mods\\" + wide(id);
    CreateDirectoryW((home.paths().home_dir + L"\\" + dir).c_str(), nullptr);
    bool ok = home.write(dir + L"\\tome.mod.json", R"({"schema": 1, "id": ")" + id +
                                                       R"(", "version": "1.0.0", "game": "1.27b", "tome": "*", )"
                                                       R"("sync": "client")" +
                                                       extra + "}");
    for (const auto& [name, text] : files) ok = home.write(dir + L"\\" + wide(name), text) && ok;
    return ok;
}

const tome::ModStatus* find_status(const std::vector<tome::ModStatus>& all, const char* id) {
    for (const tome::ModStatus& s : all) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

bool has_message(const char* id, const char* text) {
    auto all = mod_manager().status();
    const tome::ModStatus* s = find_status(all, id);
    if (!s) return false;
    if (s->reason.find(text) != std::string::npos || s->settings_error.find(text) != std::string::npos) return true;
    for (const std::string& e : s->errors) {
        if (e.find(text) != std::string::npos) return true;
    }
    for (const std::string& w : s->warnings) {
        if (w.find(text) != std::string::npos) return true;
    }
    return false;
}

#define CHECK_MESSAGE(id, text) \
    do { \
        if (!has_message(id, text)) \
            test::fail(__FILE__, __LINE__, std::string(id) + " has no message with \"" + (text) + "\"\n" + \
                                               host::log_text()); \
    } while (0)

bool running(const char* id) {
    auto all = mod_manager().status();
    const tome::ModStatus* s = find_status(all, id);
    return s && s->running;
}

const char* kSchema = R"({"type": "object", "properties": {
    "loud": {"type": "boolean", "default": false},
    "times": {"type": "integer", "minimum": 1, "maximum": 5, "default": 2}}})";
const char* kConfig = R"(, "config": "config.schema.json")";

} // namespace

TEST(host_settings_schema_problems) {
    Home home;
    CHECK(write_mod(home, "no-schema", kConfig));
    CHECK(write_mod(home, "bad-json-schema", kConfig, {{"config.schema.json", "{nope"}}));
    CHECK(write_mod(home, "bad-schema", kConfig, {{"config.schema.json", R"({"type": "object", "properties": 5})"}}));
    Session session;
    CHECK(session.start(home));
    CHECK_MESSAGE("no-schema", "can't read config.schema.json");
    CHECK_MESSAGE("bad-json-schema", "config.schema.json: ");
    CHECK_MESSAGE("bad-schema", "config.schema.json: ");
    std::string error;
    CHECK(mod_manager().set_setting("no-schema", "loud", tome::json::Value::boolean(true), error) != TOME_OK);
}

TEST(host_settings_values_read_reset_and_refused) {
    Home home;
    CHECK(write_mod(home, "cfg", kConfig, {{"config.schema.json", kSchema}}));
    CreateDirectoryW(home.paths().mod_config_dir.c_str(), nullptr);
    CHECK(home.write(L"tome\\mods\\config\\cfg.json", "{broken"));
    Session session;
    CHECK(session.start(home));
    CHECK_LOG("cfg: config\\cfg.json: ");
    CHECK_LOG("; using the defaults");
    CHECK(mod_manager().setting("cfg", "times").as_number() == 2);

    std::string error;
    CHECK_EQ(mod_manager().set_setting("cfg", "nothing", tome::json::Value::number(1), error),
             TOME_ERR_INVALID_ARGUMENT);
    CHECK(error.find("no setting \"nothing\" in cfg's settings schema") != std::string::npos);
    CHECK_EQ(mod_manager().set_setting("cfg", "times", tome::json::Value::number(4), error), TOME_OK);
    CHECK_EQ(mod_manager().set_setting("cfg", "loud", tome::json::Value::boolean(true), error), TOME_OK);
    CHECK(mod_manager().setting("cfg", "times").as_number() == 4);

    // Back to the defaults; a second reset changes nothing.
    mod_manager().reset_settings("cfg");
    CHECK_LOG("cfg: settings reset to their defaults");
    CHECK(mod_manager().setting("cfg", "times").as_number() == 2);
    CHECK(!mod_manager().setting("cfg", "loud").as_bool());
    mod_manager().reset_settings("cfg");
    mod_manager().reset_settings("no-such-mod");

    // Edited by hand into something broken: the current values stay.
    CHECK(home.write(L"tome\\mods\\config\\cfg.json", "{still broken"));
    CHECK(home.touch(L"tome\\mods\\config\\cfg.json"));
    CHECK(mod_manager().poll_settings_changed());
    mod_manager().reload_settings();
    CHECK_LOG("; keeping the current settings");
    // ...or with a value out of range, which is warned about.
    CHECK(home.write(L"tome\\mods\\config\\cfg.json", R"({"times": 99})"));
    CHECK(home.touch(L"tome\\mods\\config\\cfg.json", 20));
    mod_manager().reload_settings();
    CHECK_LOG("cfg: config\\cfg.json: ");
}

TEST(host_discovery_problems) {
    Home home;
    CHECK(write_mod(home, "no-wasm", R"(, "wasm": "mod.wasm")"));
    CHECK(write_mod(home, "no-assets", R"(, "assets": "art")"));
    CHECK(write_mod(home, "other-game", R"(, "entry": {"1.31": "new.dll"})"));
    CreateDirectoryW((home.paths().mods_dir + L"\\empty").c_str(), nullptr);
    Session session;
    CHECK(session.start(home));
    CHECK_MESSAGE("no-wasm", "\"wasm\" module \"mod.wasm\" not found");
    CHECK_MESSAGE("no-assets", "\"assets\" folder \"art\" not found");
    CHECK_MESSAGE("other-game", "no \"entry\" DLL for this game version; loading as assets-only");
}

TEST(host_dll_that_doesnt_load) {
    Home home;
    // A 32-bit DLL's headers and nothing else: Tome's checks pass, LoadLibrary fails.
    std::string dll(1024, '\0');
    dll[0] = 'M';
    dll[1] = 'Z';
    dll[0x3C] = static_cast<char>(0x80);
    std::memcpy(&dll[0x80], "PE\0\0", 4);
    dll[0x84] = 0x4c;  // IMAGE_FILE_MACHINE_I386
    dll[0x85] = 0x01;
    dll[0x96] = 0x02;
    dll[0x97] = 0x20;  // IMAGE_FILE_DLL
    CHECK(write_mod(home, "broken-dll", R"(, "entry": "mod.dll")", {{"mod.dll", dll}}));
    // A real DLL that isn't a mod: no tome_mod_load.
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    std::string version_dll;
    CHECK(tome::read_file(std::wstring(system) + L"\\version.dll", version_dll));
    CHECK(write_mod(home, "not-a-mod", R"(, "entry": "mod.dll")", {{"mod.dll", version_dll}}));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("broken-dll", true);
    mod_manager().set_enabled("not-a-mod", true);
    CHECK(!running("broken-dll"));
    CHECK(!running("not-a-mod"));
    CHECK_LOG("LoadLibrary failed: ");
    CHECK_MESSAGE("not-a-mod", "tome_mod_load");
}

TEST(host_reload_restarts_running_mods) {
    Home home;
    CHECK(home.install_mod("hello"));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("hello", true);
    CHECK(running("hello"));
    CHECK(mod_manager().reload("other").empty());
    std::vector<std::string> reloaded = mod_manager().reload("hello");
    CHECK_EQ(reloaded.size(), size_t(1));
    CHECK(running("hello"));
    CHECK_EQ(mod_manager().reload("").size(), size_t(1));
}

TEST(host_dependents_turned_off_with_their_dependency) {
    Home home;
    CHECK(write_mod(home, "base", ""));
    CHECK(write_mod(home, "addon", R"(, "depends": {"base": "^1.0"})"));
    CHECK(write_mod(home, "extra", R"(, "depends": {"addon": "*"})"));
    Session session;
    CHECK(session.start(home));
    CHECK(running("addon"));
    std::vector<std::string> off = mod_manager().set_enabled("base", false);
    CHECK_LOG("addon: disabled, since it depends on base");
    CHECK(!running("addon"));
    CHECK(!running("extra"));
}

TEST(host_signatures_that_dont_count) {
    Home home;
    // Only code is signed: each has a module that does nothing.
    wasm_test::Builder b;
    b.export_func("tome_mod_load", b.func({}, {wasm_test::I32}, {wasm_test::op::i32_const(0)}));
    const char* wasm = R"(, "wasm": "mod.wasm")";
    CHECK(write_mod(home, "huge-sig", wasm, {{"mod.wasm", b.bytes()}, {"tome.sig", std::string(tome::signature::kMaxFileSize + 1, 'x')}}));
    CHECK(write_mod(home, "bad-sig", wasm, {{"mod.wasm", b.bytes()}, {"tome.sig", "not a signature"}}));
    Session session;
    CHECK(session.start(home));
    CHECK_MESSAGE("huge-sig", "bytes; treated as not signed");
    CHECK_MESSAGE("bad-sig", "tome.sig is invalid (");
}

TEST(host_broken_tome_files) {
    Home home;
    CHECK(home.install_mod("hello"));
    CHECK(home.write(L"tome\\state.json", "{not json"));
    CreateDirectoryW((home.paths().tome_dir + L"\\cache").c_str(), nullptr);
    CHECK(home.write(L"tome\\cache\\file-hashes.json", "[]"));
    Session session;
    CHECK(session.start(home));
    CHECK_LOG("tome/state.json is invalid (");
    mod_manager().set_enabled("hello", true);
    CHECK_LOG("tome/cache/file-hashes.json isn't valid; every mod's files are hashed again");
}

TEST(host_wasm_unload_runs_on_reload_and_exit) {
    using namespace wasm_test;
    Builder b;
    const uint32_t log = b.import("tome", "log", {I32, I32, I32}, {});
    b.memory(1);
    b.data(16, "unloading");
    b.export_func("tome_mod_load", b.func({}, {I32}, {op::i32_const(0)}));
    b.export_func("tome_mod_unload", b.func({}, {}, {op::i32_const(1), op::i32_const(16), op::i32_const(9),
                                                     op::call(log)}));
    Home home;
    CHECK(write_mod(home, "wasm-unload", R"(, "wasm": "mod.wasm")", {{"mod.wasm", b.bytes()}}));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("wasm-unload", true);
    CHECK(running("wasm-unload"));
    // Changed on disk: unloaded, then loaded again.
    CHECK(home.touch(L"tome\\mods\\wasm-unload\\mod.wasm"));
    CHECK(mod_manager().poll_changed());
    mod_manager().refresh();
    CHECK_LOG("[wasm-unload] unloading");
    mod_manager().shutdown();
    CHECK_LOG("Exiting: disabled and unloaded 1 mod");
}
