// Discovery, metadata problems, state.json, hot reload, packages, safe mode
// and shutdown, with the hello examples (C, C++, Rust) and the e2e fixtures.
#include "harness.h"
#include "test.h"

#include "core/json.h"
#include "core/zip.h"
#include "mod_manager.h"
#include "paths.h"

#include <cstdint>
#include <cstring>

using host::Home;
using host::Session;
using tome::mod_manager;

namespace {

const tome::ModStatus* find_status(const std::vector<tome::ModStatus>& all, const char* id) {
    for (const tome::ModStatus& s : all) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

const tome::ModStatus* find_folder(const std::vector<tome::ModStatus>& all, const char* folder) {
    for (const tome::ModStatus& s : all) {
        if (s.folder == folder) return &s;
    }
    return nullptr;
}

bool any_error_contains(const tome::ModStatus* s, const char* text) {
    if (!s) return false;
    if (s->reason.find(text) != std::string::npos) return true;
    for (const std::string& e : s->errors) {
        if (e.find(text) != std::string::npos) return true;
    }
    return false;
}

int count_logged(const char* text) {
    std::string log = host::log_text();
    int n = 0;
    for (size_t at = log.find(text); at != std::string::npos; at = log.find(text, at + 1)) ++n;
    return n;
}

// What a user editing tome/state.json does (the e2e test's set_enabled).
bool edit_state(Home& home, const char* id, bool enabled) {
    std::string text;
    if (!home.read(L"tome\\state.json", text)) return false;
    tome::json::Value root;
    if (!tome::json::parse(text, root).ok || !root.find("mods") || !root.find("mods")->find(id)) return false;
    tome::json::Value mods = *root.find("mods");
    tome::json::Value mod = *mods.find(id);
    mod.set("enabled", tome::json::Value::boolean(enabled));
    mods.set(id, mod);
    root.set("mods", mods);
    return home.write(L"tome\\state.json", tome::json::serialize(root));
}

// --- a zip writer (stored entries), for changed packages ---

void put16(std::string& out, uint16_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>(v >> 8));
}

void put32(std::string& out, uint32_t v) {
    put16(out, static_cast<uint16_t>(v & 0xFFFF));
    put16(out, static_cast<uint16_t>(v >> 16));
}

std::string write_zip(const std::vector<std::pair<std::string, std::string>>& files) {
    std::string out, central;
    for (const auto& [name, data] : files) {
        uint32_t crc = tome::zip::crc32(data);
        uint32_t offset = static_cast<uint32_t>(out.size());
        put32(out, 0x04034b50);
        put16(out, 20);
        put16(out, 0);
        put16(out, 0);  // stored
        put16(out, 0);
        put16(out, 0x21);  // 1980-01-01
        put32(out, crc);
        put32(out, static_cast<uint32_t>(data.size()));
        put32(out, static_cast<uint32_t>(data.size()));
        put16(out, static_cast<uint16_t>(name.size()));
        put16(out, 0);
        out += name;
        out += data;

        put32(central, 0x02014b50);
        put16(central, 20);
        put16(central, 20);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0x21);
        put32(central, crc);
        put32(central, static_cast<uint32_t>(data.size()));
        put32(central, static_cast<uint32_t>(data.size()));
        put16(central, static_cast<uint16_t>(name.size()));
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put32(central, 0);
        put32(central, offset);
        central += name;
    }
    uint32_t central_offset = static_cast<uint32_t>(out.size());
    out += central;
    put32(out, 0x06054b50);
    put16(out, 0);
    put16(out, 0);
    put16(out, static_cast<uint16_t>(files.size()));
    put16(out, static_cast<uint16_t>(files.size()));
    put32(out, static_cast<uint32_t>(central.size()));
    put32(out, central_offset);
    put16(out, 0);
    return out;
}

// The built hello.tome with tome.mod.json's version changed.
bool hello_package(const char* version, std::string& out) {
    std::string archive;
    if (!tome::read_file(host::win_path(TOME_TEST_BUILD_DIR "/packages/hello.tome"), archive)) return false;
    std::vector<tome::zip::Entry> entries;
    std::string error;
    if (!tome::zip::read_directory(archive, entries, error)) return false;
    std::vector<std::pair<std::string, std::string>> files;
    for (const tome::zip::Entry& e : entries) {
        if (e.directory()) continue;
        std::string data;
        if (!tome::zip::extract(archive, e, data, error)) return false;
        if (e.name == "tome.mod.json") {
            size_t at = data.find("\"version\": \"1.0.0\"");
            if (at == std::string::npos) return false;
            data.replace(at, std::strlen("\"version\": \"1.0.0\""), std::string("\"version\": \"") + version + "\"");
        }
        files.emplace_back(e.name, data);
    }
    out = write_zip(files);
    return true;
}

using Native1 = int32_t (*)(int32_t);
using NativeReal = uint32_t (*)(const float*);

float bits_to_float(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

} // namespace

TEST(host_metadata_problems_reported) {
    Home home;
    CHECK(home.install_mod("broken-json"));
    CHECK(home.install_mod("needs-ghost"));
    CHECK(home.install_mod("needs-unmapped"));
    Session session;
    CHECK(session.start(home));
    auto all = mod_manager().status();
    const tome::ModStatus* broken = find_folder(all, "broken-json");
    CHECK(broken && broken->state == "error");
    CHECK(any_error_contains(broken, "tome.mod.json: line 2"));
    const tome::ModStatus* ghost = find_status(all, "needs-ghost");
    CHECK(any_error_contains(ghost, "needs \"ghost\" ^1, which isn't installed"));
    const tome::ModStatus* unmapped = find_status(all, "needs-unmapped");
    CHECK(any_error_contains(unmapped, "needs \"CUnit.armor\", which isn't mapped for 1.27b"));
    CHECK_LOG("Mods: 3 installed, 0 active");
}

TEST(host_64_bit_dll_rejected) {
    Home home;
    CHECK(home.install_mod("wrong-arch"));
    // Just the headers a 64-bit DLL starts with: Tome reads those before LoadLibrary.
    std::string dll(1024, '\0');
    dll[0] = 'M';
    dll[1] = 'Z';
    dll[0x3C] = static_cast<char>(0x80);
    std::memcpy(&dll[0x80], "PE\0\0", 4);
    dll[0x84] = static_cast<char>(0x64);  // IMAGE_FILE_MACHINE_AMD64 (0x8664)
    dll[0x85] = static_cast<char>(0x86);
    dll[0x96] = 0x22;  // executable image, large address aware
    dll[0x97] = 0x20;  // IMAGE_FILE_DLL
    CHECK(home.write(L"tome\\mods\\wrong-arch\\wrong-arch.dll", dll));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("wrong-arch", true);
    CHECK_LOG("wrong-arch: the DLL is 64-bit");
    CHECK(!find_status(mod_manager().status(), "wrong-arch")->running);
}

TEST(host_resolve_a_mapped_name) {
    Home home;
    CHECK(home.install_mod("hello"));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("hello", true);
    CHECK_LOG("[hello] CTextFrame::SetText is a thiscall function at 0x");
    CHECK_LOG("hello: code allowed (turned on), sha256 ");
}

TEST(host_state_json_toggles_live) {
    Home home;
    CHECK(home.install_mod("hello"));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("hello", true);
    CHECK_LOG("[hello] enabled (1 time");

    CHECK(edit_state(home, "hello", false));
    CHECK(mod_manager().poll_changed());  // what the folder watcher asks
    mod_manager().refresh();
    CHECK_LOG("[hello] disabled");
    CHECK(!find_status(mod_manager().status(), "hello")->running);

    CHECK(edit_state(home, "hello", true));
    CHECK(mod_manager().poll_changed());
    mod_manager().refresh();
    CHECK_LOG("[hello] enabled (2 times");
}

// Tome's own write of a state.json someone else wrote (the first refresh's)
// isn't an edit: no second refresh. Someone else's edit after it still is.
TEST(host_own_state_write_is_no_change) {
    Home home;
    CHECK(home.install_mod("hello"));
    CHECK(home.write(L"tome\\state.json", "{\"mods\": {}}"));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("hello", true);  // Tome writes state.json
    std::string state;
    CHECK(home.read(L"tome\\state.json", state));
    CHECK(state.find("\"hello\"") != std::string::npos);
    CHECK(!mod_manager().poll_changed());
    CHECK(!mod_manager().poll_changed());

    CHECK(edit_state(home, "hello", false));
    CHECK(mod_manager().poll_changed());
    CHECK(!mod_manager().poll_changed());
}

TEST(host_hot_reload_loads_the_new_dll) {
    Home home;
    CHECK(home.install_mod("hello"));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("hello", true);
    CHECK_EQ(count_logged("[hello] loaded (Tome"), 1);

    CHECK(home.touch(L"tome\\mods\\hello\\hello.dll"));
    CHECK(mod_manager().poll_changed());
    mod_manager().refresh();
    CHECK_LOG("Unloaded hello");
    CHECK_EQ(count_logged("[hello] loaded (Tome"), 2);
    CHECK_EQ(count_logged("[hello] enabled (1 time"), 2);  // the new DLL starts fresh
}

TEST(host_removed_mod_leaves_the_list) {
    Home home;
    CHECK(home.install_mod("hello"));
    CHECK(home.install_mod("needs-ghost"));
    Session session;
    CHECK(session.start(home));
    CHECK_EQ(mod_manager().status().size(), size_t(2));
    CHECK(home.remove(L"tome\\mods\\needs-ghost"));
    CHECK(mod_manager().poll_changed());
    mod_manager().refresh();
    CHECK_EQ(mod_manager().status().size(), size_t(1));
    CHECK(find_status(mod_manager().status(), "needs-ghost") == nullptr);
}

TEST(host_packages) {
    Home home;
    CHECK(home.install_mod("hello"));
    std::string package;
    CHECK(tome::read_file(host::win_path(TOME_TEST_BUILD_DIR "/packages/hello.tome"), package));
    CHECK(home.write(L"tome\\mods\\hello-pkg.tome", package));
    Session session;
    CHECK(session.start(home));
    CHECK_LOG("Unpacked hello-pkg.tome");
    auto all = mod_manager().status();
    CHECK(any_error_contains(find_folder(all, "hello-pkg.tome"), "the folder hello has the same id and is used instead"));

    // Without the folder, the package's copy runs.
    CHECK(home.remove(L"tome\\mods\\hello"));
    mod_manager().refresh();
    mod_manager().set_enabled("hello", true);
    const tome::ModStatus* s = find_status(mod_manager().status(), "hello");
    CHECK(s && s->running && s->package);

    // Updated in place: a new version in the same file reloads from a new copy.
    std::string updated;
    CHECK(hello_package("1.0.1", updated));
    CHECK(home.write(L"tome\\mods\\hello-pkg.tome", updated));
    CHECK(home.touch(L"tome\\mods\\hello-pkg.tome"));
    mod_manager().refresh();
    CHECK_LOG("Loaded hello 1.0.1");
    CHECK_LOG("Removed the unused unpacked package hello-pkg.");

    // Removed: the mod stops and its unpacked copy goes too.
    CHECK(home.remove(L"tome\\mods\\hello-pkg.tome"));
    mod_manager().refresh();
    CHECK(find_status(mod_manager().status(), "hello") == nullptr);
    CHECK_EQ(count_logged("Unloaded hello"), 2);
}

TEST(host_shutdown_stops_every_mod) {
    Home home;
    CHECK(home.install_mod("hello"));
    CHECK(home.install_mod("crash"));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("hello", true);
    mod_manager().set_enabled("crash", true);
    mod_manager().shutdown();
    CHECK_LOG("[crash] disabled");
    CHECK_LOG("[crash] unloading");
    CHECK_LOG("Exiting: disabled and unloaded 2 mods");
}

// Safe mode asked for at startup (-tomesafe, Shift, tome/safe): nothing
// starts, and the mod menu can still start one mod for the session.
TEST(host_safe_mode_from_startup) {
    Home home;
    CHECK(home.install_mod("hello"));
    CHECK(home.install_mod("crash"));
    Session session;
    session.safe_mode = "-tomesafe";
    CHECK(session.start(home));
    auto all = mod_manager().status();
    CHECK_EQ(find_status(all, "hello")->reason, std::string("safe mode"));
    CHECK(!find_status(all, "hello")->running);
    CHECK_NO_LOG("[hello] loaded");

    mod_manager().set_enabled("hello", true);
    all = mod_manager().status();
    CHECK(find_status(all, "hello")->running);
    CHECK(!find_status(all, "crash")->running);
}

// Safe mode turned on in the mod menu: every running mod stops.
TEST(host_safe_mode_turned_on_live) {
    Home home;
    CHECK(home.install_mod("hello"));
    CHECK(home.install_mod("crash"));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("hello", true);
    mod_manager().set_enabled("crash", true);
    mod_manager().set_safe_mode_now(true);
    CHECK_LOG("Safe mode turned on in the mod menu");
    auto all = mod_manager().status();
    CHECK(!find_status(all, "hello")->running);
    CHECK(!find_status(all, "crash")->running);
    CHECK_LOG("[crash] disabled");

    mod_manager().set_safe_mode_now(false);
    all = mod_manager().status();
    CHECK(find_status(all, "hello")->running);
    CHECK(find_status(all, "crash")->running);
}

TEST(host_cpp_mod_natives) {
    Home home;
    CHECK(home.install_mod("hello-cpp"));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("hello-cpp", true);
    CHECK(find_status(mod_manager().status(), "hello-cpp")->running);
    session.game.load_map();
    auto log = reinterpret_cast<Native1>(session.game.native("HelloCppLog"));
    auto twice = reinterpret_cast<NativeReal>(session.game.native("HelloCppDouble"));
    CHECK(log && twice);
    if (!log || !twice) return;
    log(7);
    CHECK_LOG("HelloCppLog(7) called from JASS");
    float x = 1.25f;
    CHECK_EQ(bits_to_float(twice(&x)), 2.5f);  // real arguments are float*, results float bits in eax
}

TEST(host_rust_mod_natives) {
    std::string dll;
    if (!tome::read_file(host::win_path(TOME_TEST_BUILD_DIR "/mods/hello-rs/hello_rs.dll"), dll)) {
        std::printf("  skipped: the Rust example isn't built\n");
        return;
    }
    Home home;
    CHECK(home.install_mod("hello-rs"));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("hello-rs", true);
    CHECK(find_status(mod_manager().status(), "hello-rs")->running);
    session.game.load_map();
    auto log = reinterpret_cast<Native1>(session.game.native("HelloRsLog"));
    auto twice = reinterpret_cast<NativeReal>(session.game.native("HelloRsDouble"));
    CHECK(log && twice);
    if (!log || !twice) return;
    log(9);
    CHECK_LOG("HelloRsLog(9)");
    float x = -3.0f;
    CHECK_EQ(bits_to_float(twice(&x)), -6.0f);
}
