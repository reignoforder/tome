// Mod lifecycle, approval, natives, crash isolation and hooks: the e2e test's
// checks of Tome's own behavior, against the real loader and the real
// fixture mods, with a fake game image instead of the game.
#include "harness.h"
#include "test.h"

#include "core/json.h"
#include "core/mod_state.h"
#include "core/signature.h"
#include "monocypher-ed25519.h"
#include "mod_manager.h"
#include "sha256.h"
#include "units.h"

#include <cstdint>
#include <cstdio>
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

using Native1 = int32_t (*)(int32_t);

} // namespace

TEST(host_starts_without_mods) {
    Home home;
    Session session;
    CHECK(session.start(home));
    CHECK_LOG("redirected 3 of 3 InitNatives call sites");
    CHECK(mod_manager().status().empty());
}

TEST(host_new_code_waits_for_approval) {
    Home home;
    CHECK(home.install_mod("crash"));
    Session session;
    CHECK(session.start(home));
    CHECK_EQ(state_of("crash"), std::string("approval"));
    CHECK_EQ(approval_of("crash"), std::string("new"));
    CHECK_NO_LOG("[crash] mode");  // its DLL never loaded
}

TEST(host_turning_on_allows_and_loads) {
    Home home;
    CHECK(home.install_mod("crash"));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("crash", true);
    CHECK_EQ(state_of("crash"), std::string("active"));
    CHECK_LOG("[crash] mode \"\"");
    std::string state;
    CHECK(home.read(L"tome\\state.json", state));
    CHECK(state.find("\"crash\"") != std::string::npos);
}

TEST(host_natives_bound_and_called) {
    Home home;
    CHECK(home.install_mod("crash"));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("crash", true);

    session.game.load_map();
    CHECK_EQ(session.game.engine_init_count(), 1u);
    CHECK_LOG("Engine initialized natives; bound");
    auto native = reinterpret_cast<Native1>(session.game.native("CrashNative"));
    CHECK(native != nullptr);
    if (native) CHECK_EQ(native(41), 42);
    CHECK(session.game.native("TomeIsLoaded") != nullptr);
}

// Once a map loads the fake has a game instance; what Tome reads of its
// players (the selection API, names) is "none", not a jump into the zeroed
// rest of CGameWar3::GetPlayer.
TEST(host_fake_game_has_no_players) {
    Home home;
    Session session;
    session.events = true;  // units::init
    CHECK(session.start(home));
    session.game.load_map();
    CHECK(tome::units::read_selection().units.empty());
    CHECK_EQ(tome::units::player_name(0), std::string());
    CHECK(!tome::units::is_human(0));
}

TEST(host_disabled_native_returns_zero) {
    Home home;
    CHECK(home.install_mod("crash"));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("crash", true);
    session.game.load_map();
    auto native = reinterpret_cast<Native1>(session.game.native("CrashNative"));
    CHECK(native != nullptr);
    if (!native) return;
    CHECK_EQ(native(1), 2);
    mod_manager().set_enabled("crash", false);
    CHECK_EQ(state_of("crash"), std::string("disabled"));
    CHECK_EQ(native(1), 0);  // the map keeps running; the native answers 0
    mod_manager().set_enabled("crash", true);
    CHECK_EQ(native(1), 2);
}

TEST(host_crash_in_load_is_a_load_failure) {
    Home home;
    CHECK(home.install_mod("crash"));
    CHECK(home.write(L"tome\\mods\\crash\\mode.txt", "load"));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("crash", true);
    CHECK_LOG("[crash] mode \"load\"");
    CHECK_LOG("Failed to load crash: crashed in tome_mod_load: access violation");
    auto all = mod_manager().status();
    const tome::ModStatus* s = find_status(all, "crash");
    CHECK(s != nullptr);
    if (s) {
        CHECK(!s->running);
        CHECK_EQ(s->state, std::string("error"));
        CHECK(!s->crash_calls.empty());
    }
}

TEST(host_crash_in_enable_is_caught) {
    Home home;
    CHECK(home.install_mod("crash"));
    CHECK(home.write(L"tome\\mods\\crash\\mode.txt", "enable"));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("crash", true);
    CHECK_LOG("[crash] mode \"enable\"");
    CHECK_LOG("crash crashed in tome_mod_enable: access violation");
    CHECK(!find_status(mod_manager().status(), "crash")->running);
    CHECK_LOG("[crash] disabled");  // it still got tome_mod_disable
}

TEST(host_crash_in_native_returns_zero_and_disables) {
    Home home;
    CHECK(home.install_mod("crash"));
    CHECK(home.write(L"tome\\mods\\crash\\mode.txt", "native"));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("crash", true);
    session.game.load_map();
    auto native = reinterpret_cast<Native1>(session.game.native("CrashNative"));
    CHECK(native != nullptr);
    if (!native) return;
    CHECK_EQ(native(5), 0);  // crashed: 0, and the caller carries on
    CHECK(!find_status(mod_manager().status(), "crash")->running);
    CHECK_LOG("[crash] disabled");
    CHECK_EQ(native(5), 0);  // and from then on
}

// The hook handlers that ran during hook-b's nth probe call of
// RCString::GetText, in order, then what the call returned (as the e2e
// test's probe_sequence).
std::string probe_sequence(int n) {
    std::string log = host::log_text(), out;
    int seen = 0;
    size_t at = 0;
    while (at < log.size()) {
        size_t end = log.find('\n', at);
        if (end == std::string::npos) end = log.size();
        std::string line = log.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find("[hook-b] probe start") != std::string::npos) {
            ++seen;
            continue;
        }
        if (seen != n) continue;
        size_t hook = line.find("] hook: ");
        if (hook != std::string::npos && (line.find("[hook-a]") != std::string::npos ||
                                          line.find("[hook-b]") != std::string::npos)) {
            out += line.substr(hook + 8) + " ";
        }
        size_t returned = line.find("[hook-b] probe returned ");
        if (returned != std::string::npos) return out + "=> " + line.substr(returned + 24);
    }
    return out + "(no probe " + std::to_string(n) + ")";
}

#define CHECK_PROBE(n, expected)                                                                     \
    do {                                                                                             \
        std::string got_ = probe_sequence(n);                                                        \
        if (got_ != (expected))                                                                      \
            test::fail(__FILE__, __LINE__, "probe " #n ": expected " + std::string(expected) +       \
                                               "\n  got " + got_);                                  \
    } while (0)

TEST(host_hooks_run_in_priority_order_and_revoke) {
    const std::string both = "b-before a-before b-replace b-next-returned \"TomeHookProbe from hook-a\" b-after "
                              "a-after => \"TomeHookProbe from hook-a\"";
    const std::string b_only = "b-before b-replace b-next-returned \"TomeHookProbe\" b-after => \"TomeHookProbe\"";
    Home home;
    CHECK(home.install_mod("hook-a"));
    CHECK(home.install_mod("hook-b"));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("hook-a", true);
    mod_manager().set_enabled("hook-b", true);
    CHECK_LOG("[hook-a] hooked RCString::GetText");
    CHECK_LOG("Hooked RCString::GetText at 0x");
    CHECK_LOG("(5 bytes moved, 1 arguments)");
    CHECK_PROBE(1, both);

    mod_manager().set_enabled("hook-a", false);
    mod_manager().set_enabled("hook-b", false);
    mod_manager().set_enabled("hook-b", true);
    CHECK_PROBE(2, b_only);

    mod_manager().set_enabled("hook-a", true);
    mod_manager().set_enabled("hook-b", false);
    mod_manager().set_enabled("hook-b", true);
    CHECK_PROBE(3, both);

    // One patch for the whole time, however many handlers came and went.
    size_t from = 0;
    int patches = 0;
    std::string log = host::log_text();
    while ((from = log.find("Hooked RCString::GetText at", from)) != std::string::npos) {
        ++patches;
        ++from;
    }
    CHECK_EQ(patches, 1);
}

TEST(host_crash_in_replace_hook_runs_the_original) {
    Home home;
    CHECK(home.install_mod("crash"));
    CHECK(home.write(L"tome\\mods\\crash\\mode.txt", "hook"));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("crash", true);
    CHECK_LOG("crash crashed in replace hook RCString::GetText: access violation");
    CHECK_LOG("[crash] probe returned \"TomeCrashProbe\"");
}

TEST(host_new_assets_wait_for_approval) {
    Home home;
    CHECK(home.install_mod("asset-map"));
    Session session;
    CHECK(session.start(home));
    CHECK_EQ(state_of("asset-map"), std::string("approval"));
    CHECK_LOG("asset-map: adds game files; turn it on to allow them");
    mod_manager().set_enabled("asset-map", true);
    CHECK(state_of("asset-map") != std::string("approval"));
    std::string state;
    CHECK(home.read(L"tome\\state.json", state));
    CHECK(state.find("\"schema\": 3") != std::string::npos);
}

TEST(host_schema_2_state_asks_again) {
    // Older approvals aren't carried over: the mod asks, and the file is
    // written as schema 3.
    Home home;
    CHECK(home.install_mod("asset-map"));
    CHECK(home.write(L"tome\\state.json", R"({"schema":2,"mods":{"asset-map":{"enabled":true}}})"));
    Session session;
    CHECK(session.start(home));
    CHECK_EQ(state_of("asset-map"), std::string("approval"));
    std::string state;
    CHECK(home.read(L"tome\\state.json", state));
    CHECK(state.find("\"schema\": 3") != std::string::npos);
}

namespace {

// The crash fixture's code hash: its DLL is its only code file.
std::string crash_hash(const Home& home) {
    std::wstring dll = home.paths().mods_dir + L"\\crash\\crash.dll";
    return tome::sha256_bytes(tome::mod_state::manifest({{"crash.dll", tome::sha256_file(dll)}}));
}

// tome/cache/file-hashes.json with one entry for the crash fixture's DLL:
// its real stamp (or one byte larger), and `sha`.
bool plant_dll_hash(Home& home, const std::string& sha, uint64_t extra_size = 0) {
    const std::wstring dll = home.paths().mods_dir + L"\\crash\\crash.dll";
    HANDLE in = CreateFileW(dll.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (in == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info;
    const bool ok = GetFileInformationByHandle(in, &info);
    CloseHandle(in);
    if (!ok) return false;
    auto wide = [](DWORD high, DWORD low) { return (uint64_t(high) << 32) | low; };
    char stamp[96];
    std::snprintf(stamp, sizeof(stamp), "%llx-%llx-%llx-%llx-%lx",
                  static_cast<unsigned long long>(wide(info.nFileSizeHigh, info.nFileSizeLow) + extra_size),
                  static_cast<unsigned long long>(wide(info.ftLastWriteTime.dwHighDateTime, info.ftLastWriteTime.dwLowDateTime)),
                  static_cast<unsigned long long>(wide(info.ftCreationTime.dwHighDateTime, info.ftCreationTime.dwLowDateTime)),
                  static_cast<unsigned long long>(wide(info.nFileIndexHigh, info.nFileIndexLow)),
                  static_cast<unsigned long>(info.dwVolumeSerialNumber));
    tome::json::Value entry = tome::json::Value::object();
    entry.set("stamp", tome::json::Value::string(stamp));
    entry.set("sha256", tome::json::Value::string(sha));
    tome::json::Value files = tome::json::Value::object();
    files.set(tome::narrow(dll), std::move(entry));
    tome::json::Value root = tome::json::Value::object();
    root.set("version", tome::json::Value::number(1));
    root.set("files", std::move(files));
    CreateDirectoryW((home.paths().tome_dir + L"\\cache").c_str(), nullptr);
    return home.write(L"tome\\cache\\file-hashes.json", tome::json::serialize(root));
}

std::string code_hash_of(const char* id) {
    auto all = mod_manager().status();
    const tome::ModStatus* s = find_status(all, id);
    return s ? s->code_hash : "(missing)";
}

} // namespace

TEST(host_file_hashes_are_remembered_across_starts) {
    Home home;
    CHECK(home.install_mod("crash"));
    // An entry with the file's stamp is taken instead of reading the file.
    const std::string planted(64, 'c');
    CHECK(plant_dll_hash(home, planted));
    Session session;
    CHECK(session.start(home));
    CHECK_EQ(code_hash_of("crash"), tome::sha256_bytes(tome::mod_state::manifest({{"crash.dll", planted}})));
    std::string cache;
    CHECK(home.read(L"tome\\cache\\file-hashes.json", cache));
    CHECK(cache.find(planted) != std::string::npos);  // still used: kept
}

TEST(host_file_hashes_of_changed_files_are_read_again) {
    Home home;
    CHECK(home.install_mod("crash"));
    CHECK(plant_dll_hash(home, std::string(64, 'c'), 1));  // another size: not this file
    Session session;
    CHECK(session.start(home));
    CHECK_EQ(code_hash_of("crash"), crash_hash(home));
    std::string cache;
    CHECK(home.read(L"tome\\cache\\file-hashes.json", cache));
    CHECK(cache.find(std::string(64, 'c')) == std::string::npos);
    CHECK(cache.find(tome::sha256_file(home.paths().mods_dir + L"\\crash\\crash.dll")) != std::string::npos);
}

TEST(host_folder_code_hash_is_saved) {
    // Saved with the folder's print, so the next start opens none of its
    // files (core/hash_cache.h tests the lookup).
    Home home;
    CHECK(home.install_mod("crash"));
    Session session;
    CHECK(session.start(home));
    std::string text;
    CHECK(home.read(L"tome\\cache\\file-hashes.json", text));
    tome::json::Value root;
    CHECK(tome::json::parse(text, root).ok);
    const tome::json::Value* folders = root.find("folders");
    const tome::json::Value* crash =
        folders ? folders->find(tome::narrow(home.paths().mods_dir + L"\\crash")) : nullptr;
    const tome::json::Value* sha = crash ? crash->find("sha256") : nullptr;
    CHECK(sha && sha->is_string() && sha->as_string() == crash_hash(home));
}

TEST(host_approval_of_other_code_asks_again) {
    Home home;
    CHECK(home.install_mod("crash"));
    CHECK(home.write(L"tome\\state.json", R"({"schema":2,"mods":{"crash":{"enabled":true,"approved":")" +
                                              std::string(64, 'a') + R"(","approved_from":"elsewhere"}}})"));
    Session session;
    CHECK(session.start(home));
    CHECK_EQ(state_of("crash"), std::string("approval"));
    CHECK_EQ(approval_of("crash"), std::string("changed"));
    CHECK_NO_LOG("[crash] mode");
}

namespace {

// Signs the installed crash fixture as sdk/support/tome_sign.py would, with
// the key made from `seed`; returns the public key (base64).
std::string sign_crash(Home& home, uint8_t seed_byte) {
    uint8_t seed[32], secret[64];
    tome::signature::Key key;
    std::memset(seed, seed_byte, sizeof(seed));
    crypto_ed25519_key_pair(secret, key.data(), seed);
    const std::wstring dir = home.paths().mods_dir + L"\\crash";
    const std::string code = tome::sha256_bytes(tome::mod_state::manifest({{"crash.dll", tome::sha256_file(dir + L"\\crash.dll")}}));
    const std::string text = tome::signature::message("crash", code, tome::sha256_file(dir + L"\\tome.mod.json"));
    tome::signature::Signature sig;
    sig.key = key;
    crypto_ed25519_sign(sig.signature.data(), secret, reinterpret_cast<const uint8_t*>(text.data()), text.size());
    const std::string public_key = tome::signature::encode_key(key);
    home.write(L"tome\\mods\\crash\\tome.sig",
               "{\"schema\": 1, \"key\": \"" + public_key + "\", \"signature\": \"" +
                   tome::signature::base64_encode(sig.signature.data(), sig.signature.size()) + "\"}");
    return public_key;
}

// state.json allowing other code for crash, from another folder, signed by `key`.
std::string allowed_with_key(const std::string& key) {
    return R"({"schema":3,"mods":{"crash":{"enabled":true,"approved":")" + std::string(64, 'a') +
           R"(","approved_from":"elsewhere","approved_key":")" + key + R"("}}})";
}

} // namespace

TEST(host_signed_update_by_the_same_key_runs) {
    Home home;
    CHECK(home.install_mod("crash"));
    const std::string key = sign_crash(home, 7);
    CHECK(home.write(L"tome\\state.json", allowed_with_key(key)));
    Session session;
    CHECK(session.start(home));
    CHECK_EQ(state_of("crash"), std::string("active"));
    CHECK_LOG("crash: code allowed (signed by the same key as before)");
    auto all = mod_manager().status();
    const tome::ModStatus* s = find_status(all, "crash");
    CHECK(s && s->signer == key);
    std::string state;
    CHECK(home.read(L"tome\\state.json", state));
    CHECK(state.find(std::string(64, 'a')) == std::string::npos);  // the new hash is recorded
    CHECK(state.find(key) != std::string::npos);
}

TEST(host_signed_by_another_key_asks) {
    Home home;
    CHECK(home.install_mod("crash"));
    sign_crash(home, 7);
    tome::signature::Key other_key;
    uint8_t seed[32], secret[64];
    std::memset(seed, 9, sizeof(seed));
    crypto_ed25519_key_pair(secret, other_key.data(), seed);
    CHECK(home.write(L"tome\\state.json", allowed_with_key(tome::signature::encode_key(other_key))));
    Session session;
    CHECK(session.start(home));
    CHECK_EQ(state_of("crash"), std::string("approval"));
    CHECK_EQ(approval_of("crash"), std::string("signer"));
    CHECK_LOG("crash: it's no longer signed by the author's key");
    CHECK_NO_LOG("[crash] mode");
}

TEST(host_tampered_signature_counts_as_unsigned) {
    Home home;
    CHECK(home.install_mod("crash"));
    const std::string key = sign_crash(home, 7);
    // A file added after signing.
    CHECK(home.write(L"tome\\mods\\crash\\extra.dll", "not what was signed"));
    CHECK(home.write(L"tome\\state.json", allowed_with_key(key)));
    Session session;
    CHECK(session.start(home));
    CHECK_EQ(state_of("crash"), std::string("approval"));
    CHECK_LOG("tome.sig doesn't match the mod's files");
    CHECK_NO_LOG("[crash] mode");
}

#ifdef TOME_TEST_SIGNED
TEST(host_python_signature_is_accepted) {
    // tests/host/make_signed_fixture.py: signed by sdk/support/tome_sign.py
    // with its test key, which state.json says the user allowed.
    const std::string key = "IVL40Zt5HSRFMkLhXy6rbLfP+ntqXtMAl5YOBpiB2xI=";
    Home home;
    CHECK(home.install_mod("crash-signed"));
    CHECK(home.write(L"tome\\state.json", allowed_with_key(key)));
    Session session;
    CHECK(session.start(home));
    CHECK_NO_LOG("tome.sig doesn't match");
    CHECK_EQ(state_of("crash"), std::string("active"));
    CHECK_LOG("crash: code allowed (signed by the same key as before)");
}

TEST(host_signed_package_is_accepted) {
    // packages/crash.tome: tome_package(crash SIGN_KEY host/test-signing.key).
    std::string package;
    CHECK(tome::read_file(host::win_path(TOME_TEST_BUILD_DIR "/packages/crash.tome"), package));
    Home home;
    CHECK(home.write(L"tome\\mods\\crash.tome", package));
    CHECK(home.write(L"tome\\state.json", allowed_with_key("IVL40Zt5HSRFMkLhXy6rbLfP+ntqXtMAl5YOBpiB2xI=")));
    Session session;
    CHECK(session.start(home));
    CHECK_LOG("Unpacked crash.tome");
    CHECK_NO_LOG("tome.sig doesn't match");
    CHECK_EQ(state_of("crash"), std::string("active"));
    CHECK_LOG("crash: code allowed (signed by the same key as before)");
}
#endif
