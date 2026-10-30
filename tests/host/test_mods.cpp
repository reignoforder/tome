// Mod lifecycle, approval, natives, crash isolation and hooks: the e2e test's
// checks of Tome's own behavior, against the real loader and the real
// fixture mods, with a fake game image instead of the game.
#include "harness.h"
#include "test.h"

#include "mod_manager.h"

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

std::string state_of(const char* id) {
    auto all = mod_manager().status();
    const tome::ModStatus* s = find_status(all, id);
    return s ? s->state : "(missing)";
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
