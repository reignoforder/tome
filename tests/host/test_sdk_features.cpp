// SPDX-License-Identifier: GPL-3.0-or-later
// API version 19 against the loader: timers (timers.cpp) driven by the fake
// game's frames and steps, the profile the crash guard keeps of every call
// into a mod (guard.cpp, core/profile.h), and the desync checks' registration
// and what they make of other players' actions (desync.cpp). The comparison
// itself is unit-tested (tests/test_desync.cpp); in a game, by
// tools/e2e-multiplayer.sh.
#include "harness.h"
#include "test.h"

#include "api.h"
#include "desync.h"
#include "events.h"
#include "frames.h"
#include "mod_handle.h"  // tome_mod

#include <windows.h>

#include <string>
#include <vector>

using host::Home;
using host::Session;

namespace {

const tome_api* g_api = nullptr;
std::vector<std::string> g_fired;

const tome_api* start(Home& home, Session& session) {
    if (!session.start(home)) return nullptr;
    tome::frames::set_main_thread(GetCurrentThreadId());
    g_api = tome::api_table();
    return g_api;
}

tome_mod* make_mod(const char* id) {
    auto mod = new tome_mod();  // lives for the test's process
    mod->id = id;
    mod->version = "1.0.0";
    return mod;
}

void frame(double delta) {
    tome_event_info info = tome::events::default_info_for_test(TOME_EVENT_TICK);
    info.delta = delta;
    tome::events::dispatch_for_test(info);
}

void step(double seconds) {
    tome_event_info info = tome::events::default_info_for_test(TOME_EVENT_GAME_TICK);
    info.delta = seconds;
    tome::events::dispatch_for_test(info);
}

void game_event(int32_t event) {
    tome::events::dispatch_for_test(tome::events::default_info_for_test(event));
}

void TOME_CALL record(tome_mod* mod, uint32_t timer, void* user) {
    g_fired.push_back(mod->id + " " + static_cast<const char*>(user) + " " + std::to_string(timer));
}

uint32_t g_self_stop = 0;
void TOME_CALL stop_self(tome_mod* mod, uint32_t timer, void*) {
    g_fired.push_back(mod->id + " stop-self");
    g_self_stop = g_api->timer_stop(mod, timer);
}

size_t count(const std::string& prefix) {
    size_t n = 0;
    for (const std::string& s : g_fired) n += s.rfind(prefix, 0) == 0;
    return n;
}

} // namespace

TEST(host_timers_real_clock) {
    Home home;
    Session session;
    const tome_api* api = start(home, session);
    CHECK(api != nullptr);
    if (!api) return;
    CHECK(api->api_version >= 19 && tome_has_timers(api));
    tome_mod* mod = make_mod("timer-mod");
    uint32_t once = 0, every = 0, stopper = 0;
    CHECK_EQ(api->timer_start(mod, TOME_CLOCK_REAL, 0.5, 0, record, const_cast<char*>("once"), &once), TOME_OK);
    CHECK_EQ(api->timer_start(mod, TOME_CLOCK_REAL, 0.25, 0.25, record, const_cast<char*>("every"), &every), TOME_OK);
    CHECK_EQ(api->timer_start(mod, TOME_CLOCK_REAL, 0, 1, stop_self, nullptr, &stopper), TOME_OK);
    CHECK(once != 0 && every != 0 && once != every);
    // 1 s in frames of 0.125 s (exact in binary: no rounding at the edges).
    for (int i = 0; i < 8; ++i) frame(0.125);
    CHECK_EQ(count("timer-mod once"), 1u);
    CHECK_EQ(count("timer-mod every"), 4u);
    CHECK_EQ(count("timer-mod stop-self"), 1u);  // stopped from its own callback
    CHECK_EQ(g_self_stop, static_cast<uint32_t>(TOME_OK));
    CHECK_EQ(api->timer_stop(mod, once), TOME_ERR_INVALID_ARGUMENT);  // fired for the last time
    // Another mod can't stop it; its own can.
    tome_mod* other = make_mod("other");
    CHECK_EQ(api->timer_stop(other, every), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(api->timer_stop(mod, every), TOME_OK);
    for (int i = 0; i < 8; ++i) frame(0.125);
    CHECK_EQ(count("timer-mod every"), 4u);

    // Refusals.
    CHECK_EQ(api->timer_start(mod, 7, 1, 0, record, nullptr, nullptr), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(api->timer_start(mod, TOME_CLOCK_REAL, -1, 0, record, nullptr, nullptr), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(api->timer_start(mod, TOME_CLOCK_REAL, 1, 0, nullptr, nullptr, nullptr), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(api->timer_start(nullptr, TOME_CLOCK_REAL, 1, 0, record, nullptr, nullptr), TOME_ERR_INVALID_ARGUMENT);
    CHECK_LOG("[timer-mod] timer_start: the delay and interval must be 0 or more seconds");
    // Game time only in a game.
    uint32_t id = 123;
    CHECK_EQ(api->timer_start(mod, TOME_CLOCK_GAME, 1, 0, record, nullptr, &id), TOME_ERR_UNAVAILABLE);
    CHECK_EQ(id, 0u);
    mod->revoke_all();
}

TEST(host_timers_revoked_on_disable) {
    Home home;
    Session session;
    const tome_api* api = start(home, session);
    CHECK(api != nullptr);
    if (!api) return;
    tome_mod* mod = make_mod("revoked");
    // Many timers, one revoker.
    for (int i = 0; i < 50; ++i) {
        CHECK_EQ(api->timer_start(mod, TOME_CLOCK_REAL, 0.1, 0.1, record, const_cast<char*>("t"), nullptr), TOME_OK);
    }
    CHECK_EQ(mod->revokers.size(), 1u);
    frame(0.1);
    CHECK_EQ(count("revoked t"), 50u);
    mod->revoke_all();
    frame(0.1);
    frame(0.1);
    CHECK_EQ(count("revoked t"), 50u);
    // A new one after the revoke has a revoker again.
    CHECK_EQ(api->timer_start(mod, TOME_CLOCK_REAL, 0.1, 0, record, const_cast<char*>("t"), nullptr), TOME_OK);
    CHECK_EQ(mod->revokers.size(), 1u);
    mod->revoke_all();  // nothing of this test's runs in the next ones
}

TEST(host_timers_game_clock) {
    Home home;
    Session session;
    const tome_api* api = start(home, session);
    CHECK(api != nullptr);
    if (!api) return;
    tome_mod* mod = make_mod("game-timers");
    game_event(TOME_EVENT_GAME_START);
    uint32_t every = 0, later = 0, real = 0;
    CHECK_EQ(api->timer_start(mod, TOME_CLOCK_GAME, 1.0, 1.0, record, const_cast<char*>("game"), &every), TOME_OK);
    CHECK_EQ(api->timer_start(mod, TOME_CLOCK_GAME, 100, 0, record, const_cast<char*>("later"), &later), TOME_OK);
    CHECK_EQ(api->timer_start(mod, TOME_CLOCK_REAL, 0.05, 0.05, record, const_cast<char*>("real"), &real), TOME_OK);
    // Frames don't move game time.
    for (int i = 0; i < 30; ++i) frame(0.1);
    CHECK_EQ(count("game-timers game"), 0u);
    CHECK_EQ(count("game-timers real"), 30u);
    // 2.5 s of game time in steps of 0.005 s, as the game takes them.
    for (int i = 0; i < 500; ++i) step(0.005f);
    CHECK_EQ(count("game-timers game"), 2u);
    // The game ends: its timers stop, real ones go on.
    game_event(TOME_EVENT_GAME_END);
    CHECK_EQ(api->timer_stop(mod, later), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(api->timer_stop(mod, every), TOME_ERR_INVALID_ARGUMENT);
    frame(0.1);
    CHECK_EQ(count("game-timers real"), 31u);
    // A new game starts its clock at 0.
    game_event(TOME_EVENT_GAME_START);
    CHECK_EQ(api->timer_start(mod, TOME_CLOCK_GAME, 0.01, 0, record, const_cast<char*>("next"), nullptr), TOME_OK);
    step(0.005);
    CHECK_EQ(count("game-timers next"), 0u);
    step(0.005);
    CHECK_EQ(count("game-timers next"), 1u);
    CHECK_EQ(count("game-timers later"), 0u);
    // Nothing of this test's runs in the next ones: no game, no timers.
    game_event(TOME_EVENT_GAME_END);
    mod->revoke_all();
}

namespace {

void TOME_CALL slow(tome_mod*, uint32_t, void*) {
    Sleep(30);
}

void TOME_CALL slow_message(tome_mod*, const char*, const void*, size_t, void*) {
    Sleep(40);
}

void TOME_CALL publish_to_slow(tome_mod* mod, uint32_t, void*) {
    g_api->publish(mod, "slow.topic", "x", 1);
}

} // namespace

TEST(host_profile_times_calls) {
    Home home;
    Session session;
    const tome_api* api = start(home, session);
    CHECK(api != nullptr);
    if (!api) return;
    tome_mod* busy = make_mod("busy");
    tome_mod* caller = make_mod("caller");
    tome_mod* listener = make_mod("listener");
    CHECK_EQ(api->timer_start(busy, TOME_CLOCK_REAL, 0, 0, slow, nullptr, nullptr), TOME_OK);
    CHECK_EQ(api->subscribe(listener, "slow.topic", slow_message, nullptr), TOME_OK);
    CHECK_EQ(api->timer_start(caller, TOME_CLOCK_REAL, 0, 0, publish_to_slow, nullptr, nullptr), TOME_OK);
    frame(0.1);

    tome::profile::Stats b = busy->profile_stats(tome::events::frames());
    CHECK_EQ(b.calls, 1u);
    CHECK(b.slowest_ms >= 25);
    CHECK_EQ(b.slowest, std::string("timer callback"));
    // The listener's time is its own, not the caller's that published to it.
    tome::profile::Stats l = listener->profile_stats(tome::events::frames());
    tome::profile::Stats c = caller->profile_stats(tome::events::frames());
    CHECK_EQ(l.calls, 1u);
    CHECK(l.total_ms >= 35);
    CHECK_EQ(c.calls, 1u);
    CHECK(c.total_ms < 20);
    // A new DLL starts afresh.
    busy->clear_fault();
    CHECK_EQ(busy->profile_stats(tome::events::frames()).calls, 0u);
    listener->revoke_all();
}

namespace {

uint64_t TOME_CALL checksum(tome_mod*, void* user) {
    return reinterpret_cast<uintptr_t>(user);
}

} // namespace

TEST(host_checksum_registration) {
    Home home;
    Session session;
    const tome_api* api = start(home, session);
    CHECK(api != nullptr);
    if (!api) return;
    tome_mod* mod = make_mod("summed");
    CHECK_EQ(api->on_checksum(nullptr, checksum, nullptr), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(api->on_checksum(mod, checksum, reinterpret_cast<void*>(1)), TOME_OK);
    CHECK_EQ(api->on_checksum(mod, checksum, reinterpret_cast<void*>(2)), TOME_OK);  // replaces it
    CHECK_EQ(mod->revokers.size(), 1u);
    CHECK_EQ(api->on_checksum(mod, nullptr, nullptr), TOME_OK);  // removes it
    CHECK_EQ(api->on_checksum(mod, checksum, nullptr), TOME_OK);
    mod->revoke_all();
    CHECK_EQ(api->on_checksum(mod, checksum, nullptr), TOME_OK);
    CHECK_EQ(mod->revokers.size(), 1u);

    // Other players' actions: malformed ones are dropped and logged a few
    // at a time; well-formed ones for a round this computer never had wait
    // quietly.
    tome::desync_api::on_game_start();
    for (int i = 0; i < 20; ++i) tome::desync_api::receive(2, "1 x summed", "0000000000000001", false);
    CHECK_LOG("Desync check: player 2 sent a malformed checksum (\"1 x summed\"); dropped");
    CHECK_LOG("Desync check: player 2 sent too many malformed checksums; not logging the rest");
    tome::desync_api::receive(3, "1 1 summed", "0000000000000001", false);
    CHECK_NO_LOG("player 3");
    mod->revoke_all();
}
