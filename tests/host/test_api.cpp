// SPDX-License-Identifier: GPL-3.0-or-later
// Parts of the mod API no fixture mod uses, called directly through the API
// table with handles made here: services and messages between mods
// (intermod.cpp), Tome's own settings (tome_settings.cpp), and the parts of
// restarting that need no game window (restart.cpp).
#include "harness.h"
#include "test.h"

#include "api.h"
#include "events.h"
#include "frames.h"
#include "hooks.h"
#include "core/keys.h"
#include "keybinds.h"
#include "main_thread.h"
#include "mod_manager.h"
#include "patches.h"
#include "mod_handle.h"  // tome_mod
#include "restart.h"
#include "core/tome_settings.h"
#include "tome_settings.h"

#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

using host::Home;
using host::Session;

namespace {

// What the callbacks saw, in order.
std::vector<std::string> g_seen;

void TOME_CALL on_service(tome_mod* mod, const char* name, int32_t available, void* user) {
    g_seen.push_back(mod->id + " " + name + (available ? " up" : " down") + (user ? " +" : ""));
}

void TOME_CALL on_message(tome_mod* mod, const char* topic, const void* data, size_t size, void*) {
    std::string text = data ? std::string(static_cast<const char*>(data), size) : "(none)";
    g_seen.push_back(mod->id + " " + topic + " " + text);
}

const tome_api* g_api = nullptr;
tome_mod* g_echo_from = nullptr;

// Publishes the same topic again from inside its delivery: refused.
void TOME_CALL republish(tome_mod* mod, const char* topic, const void*, size_t, void*) {
    g_seen.push_back(mod->id + " republish " + std::to_string(g_api->publish(g_echo_from, topic, "x", 1)));
}

// Withdraws the next listener before it's called: it's skipped.
tome_mod* g_late = nullptr;
void TOME_CALL unsubscribe_late(tome_mod* mod, const char* topic, const void*, size_t, void*) {
    g_seen.push_back(mod->id + " " + topic + " first");
    g_late->revoke_all();
}

void TOME_CALL drop_late_listener(tome_mod* mod, const char* name, int32_t available, void*) {
    g_seen.push_back(mod->id + " " + name + (available ? " up first" : " down first"));
    g_late->revoke_all();
}

tome_mod* make_mod(const char* id) {
    auto mod = new tome_mod();  // lives for the test's process
    mod->id = id;
    mod->version = "1.0.0";
    return mod;
}

bool seen(const std::string& line) {
    for (const std::string& s : g_seen) {
        if (s == line) return true;
    }
    return false;
}

std::string seen_text() {
    std::string all;
    for (const std::string& s : g_seen) all += "  " + s + "\n";
    return all;
}

#define CHECK_SEEN(line) \
    do { \
        if (!seen(line)) test::fail(__FILE__, __LINE__, std::string("not seen: ") + (line) + "\nseen:\n" + seen_text()); \
    } while (0)

// The API table of a started session, with this thread as the game's main thread.
const tome_api* start(Home& home, Session& session) {
    if (!session.start(home)) return nullptr;
    tome::frames::set_main_thread(GetCurrentThreadId());
    g_api = tome::api_table();
    return g_api;
}

DWORD WINAPI call_off_main_thread(LPVOID param) {
    auto mod = static_cast<tome_mod*>(param);
    int table = 0;
    bool all_refused = g_api->service_register(mod, "off.thread", 1, &table) == TOME_ERR_WRONG_THREAD &&
                       g_api->service_unregister(mod, "off.thread") == TOME_ERR_WRONG_THREAD &&
                       g_api->on_service(mod, "off.thread", on_service, nullptr) == TOME_ERR_WRONG_THREAD &&
                       g_api->subscribe(mod, "off.thread", on_message, nullptr) == TOME_ERR_WRONG_THREAD &&
                       g_api->publish(mod, "off.thread", "x", 1) == 0;
    return all_refused ? 0 : 1;
}

} // namespace

TEST(host_services_between_mods) {
    Home home;
    Session session;
    const tome_api* api = start(home, session);
    CHECK(api != nullptr);
    if (!api) return;
    tome_mod* maps = make_mod("maps");
    tome_mod* user = make_mod("user");
    tome_mod* rival = make_mod("rival");
    static const int table_v2 = 2;

    // A listener before the service exists hears it come and go.
    CHECK_EQ(api->on_service(user, "maps.core", on_service, nullptr), TOME_OK);
    CHECK_EQ(api->service_register(maps, "maps.core", 2, &table_v2), TOME_OK);
    CHECK_SEEN("user maps.core up");
    CHECK_LOG("[maps] offers service maps.core (version 2)");
    CHECK(api->service_get(user, "maps.core", 2) == &table_v2);
    CHECK(api->service_get(user, "maps.core", 3) == nullptr);
    CHECK(api->service_get(nullptr, "maps.core", 2) == nullptr);

    // A listener after it exists is told at once.
    g_seen.clear();
    CHECK_EQ(api->on_service(rival, "maps.core", on_service, &g_seen), TOME_OK);
    CHECK_SEEN("rival maps.core up +");

    // Only one mod offers a name; names and arguments are checked.
    CHECK_EQ(api->service_register(rival, "maps.core", 3, &table_v2), TOME_ERR_NOT_OWNER);
    CHECK_LOG("[rival] service_register(maps.core): another mod offers it");
    CHECK_EQ(api->service_register(rival, "Bad Name", 1, &table_v2), TOME_ERR_INVALID_ARGUMENT);
    CHECK_LOG("needs a valid name");
    CHECK_EQ(api->service_register(nullptr, "maps.x", 1, &table_v2), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(api->service_unregister(rival, "maps.core"), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(api->service_unregister(nullptr, "maps.core"), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(api->on_service(user, "Bad Name", on_service, nullptr), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(api->on_service(user, nullptr, on_service, nullptr), TOME_ERR_INVALID_ARGUMENT);

    // Withdrawn: the listeners hear it, the provider doesn't.
    g_seen.clear();
    CHECK_EQ(api->service_unregister(maps, "maps.core"), TOME_OK);
    CHECK_SEEN("user maps.core down");
    CHECK_SEEN("rival maps.core down +");
    CHECK(api->service_get(user, "maps.core", 0) == nullptr);

    // Offered again, then the provider is disabled: its revoker withdraws it.
    CHECK_EQ(api->service_register(maps, "maps.core", 2, &table_v2), TOME_OK);
    g_seen.clear();
    maps->revoke_all();
    CHECK_SEEN("user maps.core down");
    CHECK(api->service_get(user, "maps.core", 0) == nullptr);

    // A disabled listener hears nothing more.
    user->revoke_all();
    g_seen.clear();
    CHECK_EQ(api->service_register(maps, "maps.core", 2, &table_v2), TOME_OK);
    CHECK(!seen("user maps.core up"));
    CHECK_SEEN("rival maps.core up +");
}

TEST(host_service_listener_revoked_by_an_earlier_one_is_skipped) {
    Home home;
    Session session;
    const tome_api* api = start(home, session);
    CHECK(api != nullptr);
    if (!api) return;
    tome_mod* provider = make_mod("provider");
    tome_mod* first = make_mod("first");
    g_late = make_mod("late");
    static const int table = 1;
    CHECK_EQ(api->on_service(first, "svc", drop_late_listener, nullptr), TOME_OK);
    CHECK_EQ(api->on_service(g_late, "svc", on_service, nullptr), TOME_OK);
    CHECK_EQ(api->service_register(provider, "svc", 1, &table), TOME_OK);
    CHECK_SEEN("first svc up first");
    CHECK(!seen("late svc up"));
}

TEST(host_messages_between_mods) {
    Home home;
    Session session;
    const tome_api* api = start(home, session);
    CHECK(api != nullptr);
    if (!api) return;
    tome_mod* a = make_mod("a");
    tome_mod* b = make_mod("b");
    tome_mod* c = make_mod("c");

    CHECK_EQ(api->subscribe(b, "news", on_message, nullptr), TOME_OK);
    CHECK_EQ(api->subscribe(c, "news", on_message, nullptr), TOME_OK);
    CHECK_EQ(api->subscribe(a, "news", on_message, nullptr), TOME_OK);
    // The publisher's own subscription isn't called.
    CHECK_EQ(api->publish(a, "news", "hi", 2), 2);
    CHECK_SEEN("b news hi");
    CHECK_SEEN("c news hi");
    CHECK(!seen("a news hi"));
    CHECK_EQ(api->publish(a, "quiet", "hi", 2), 0);
    CHECK_EQ(api->publish(a, "news", nullptr, 0), 2);

    // Bad arguments.
    CHECK_EQ(api->subscribe(a, "Bad Topic", on_message, nullptr), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(api->subscribe(a, "news", nullptr, nullptr), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(api->publish(a, "Bad Topic", "x", 1), 0);
    CHECK_EQ(api->publish(a, "news", nullptr, 4), 0);
    CHECK_EQ(api->publish(nullptr, "news", "x", 1), 0);

    // Unsubscribed on disable.
    g_seen.clear();
    c->revoke_all();
    CHECK_EQ(api->publish(a, "news", "again", 5), 1);
    CHECK(!seen("c news again"));
}

TEST(host_message_loops_and_late_unsubscribes) {
    Home home;
    Session session;
    const tome_api* api = start(home, session);
    CHECK(api != nullptr);
    if (!api) return;
    tome_mod* sender = make_mod("sender");
    tome_mod* echo = make_mod("echo");
    g_echo_from = make_mod("echo2");
    CHECK_EQ(api->subscribe(echo, "loop", republish, nullptr), TOME_OK);
    CHECK_EQ(api->publish(sender, "loop", "x", 1), 1);
    CHECK_SEEN("echo republish 0");
    CHECK_LOG("[echo2] publish(loop): already being delivered; ignored");
    // Delivered once more afterwards: the topic isn't left marked.
    CHECK_EQ(api->publish(sender, "loop", "x", 1), 1);

    tome_mod* first = make_mod("first");
    g_late = make_mod("late");
    CHECK_EQ(api->subscribe(first, "t", unsubscribe_late, nullptr), TOME_OK);
    CHECK_EQ(api->subscribe(g_late, "t", on_message, nullptr), TOME_OK);
    CHECK_EQ(api->publish(sender, "t", "m", 1), 1);
    CHECK_SEEN("first t first");
    CHECK(!seen("late t m"));
}

TEST(host_intermod_calls_off_the_main_thread_are_refused) {
    Home home;
    Session session;
    const tome_api* api = start(home, session);
    CHECK(api != nullptr);
    if (!api) return;
    tome_mod* mod = make_mod("worker");
    HANDLE thread = CreateThread(nullptr, 0, call_off_main_thread, mod, 0, nullptr);
    CHECK(thread != nullptr);
    if (!thread) return;
    WaitForSingleObject(thread, INFINITE);
    DWORD code = 1;
    GetExitCodeThread(thread, &code);
    CloseHandle(thread);
    CHECK_EQ(code, 0u);
    CHECK_LOG("[worker] service_register called off the game's main thread; ignored");
}

TEST(host_tome_settings_read_and_written) {
    Home home;
    Session session;
    CHECK(session.start(home));
    tome::tome_settings::load(&home.paths());
    tome::tome_settings::init();
    CHECK_LOG("Tome settings: widescreen on");
    CHECK(tome::tome_settings::widescreen());

    std::string error;
    CHECK(!tome::tome_settings::set("no-such-setting", tome::json::Value::boolean(true), error));
    CHECK(error.find("no Tome setting") != std::string::npos);
    CHECK(!tome::tome_settings::set("widescreen", tome::json::Value::string("loud"), error));
    CHECK(tome::tome_settings::set("widescreen", tome::json::Value::boolean(false), error));
    CHECK(!tome::tome_settings::widescreen());
    CHECK_LOG("Tome setting widescreen: Off");
    const tome::json::Value* value = tome::tome_settings::values().find("widescreen");
    CHECK(value && value->is_bool() && !value->as_bool());
    std::string text;
    CHECK(home.read(L"tome\\settings.json", text));
    tome::json::Value saved;
    CHECK(tome::json::parse(text, saved).ok && saved.find("widescreen") && !saved.find("widescreen")->as_bool());

    // Read back at the next start, unknown keys kept.
    CHECK(home.write(L"tome\\settings.json", "{\"widescreen\": false, \"later\": 1}"));
    tome::tome_settings::load(&home.paths());
    tome::tome_settings::init();
    CHECK_LOG("Tome settings: widescreen off");
    CHECK(tome::tome_settings::set("widescreen", tome::json::Value::boolean(true), error));
    CHECK(home.read(L"tome\\settings.json", text));
    CHECK(text.find("\"later\"") != std::string::npos);
}

// harden_heap is off by default and read once per start: a change waits for
// the next one (Windows can't turn heap termination off again).
TEST(host_tome_settings_harden_heap_at_start) {
    Home home;
    Session session;
    CHECK(session.start(home));
    tome::tome_settings::load(&home.paths());
    tome::tome_settings::init();
    CHECK_LOG("stop on memory corruption off");
    CHECK(!tome::tome_settings::harden_heap_at_start());
    std::string error;
    CHECK(tome::tome_settings::set("harden_heap", tome::json::Value::boolean(true), error));
    CHECK(!tome::tome_settings::harden_heap_at_start());  // from the next start
    tome::tome_settings::load(&home.paths());
    tome::tome_settings::init();
    CHECK_LOG("stop on memory corruption on");
    CHECK(tome::tome_settings::harden_heap_at_start());
    CHECK(tome::tome_settings::set("harden_heap", tome::json::Value::boolean(false), error));
    tome::tome_settings::load(&home.paths());
    tome::tome_settings::init();
    CHECK(!tome::tome_settings::harden_heap_at_start());
}

TEST(host_tome_settings_invalid_file_uses_defaults) {
    Home home;
    Session session;
    CHECK(session.start(home));
    CHECK(home.write(L"tome\\settings.json", "[1, 2]"));
    tome::tome_settings::load(&home.paths());
    tome::tome_settings::init();
    CHECK_LOG("tome/settings.json is invalid (not an object); using the defaults");
    CHECK(home.write(L"tome\\settings.json", "{oops"));
    tome::tome_settings::load(&home.paths());
    tome::tome_settings::init();
    CHECK_LOG("tome/settings.json is invalid (");
    CHECK(home.write(L"tome\\settings.json", "{\"widescreen\": \"yes please\"}"));
    tome::tome_settings::load(&home.paths());
    tome::tome_settings::init();
    CHECK_LOG("tome/settings.json: ");
    CHECK(tome::tome_settings::widescreen());
}

TEST(host_restart_without_a_window_is_refused) {
    Home home;
    Session session;
    CHECK(session.start(home));
    CHECK(!tome::restart::request("test"));
    CHECK_LOG("Restart asked for by test, but there's no game window to close");
    // Nothing pending: the shutdown starts no helper.
    tome::restart::on_shutdown();
    CHECK_NO_LOG("Restart: helper started");
    CHECK(!tome::restart::is_helper_process());
}

TEST(host_restart_helper_starts_the_game_again) {
    Home home;
    host::open_log(home);
    // Malformed arguments: nothing happens.
    TomeRestartW(nullptr, nullptr, nullptr, 0);
    wchar_t one_bar[] = L"123|only";
    TomeRestartW(nullptr, nullptr, one_bar, 0);

    // A process id that isn't one (nothing to wait for) and a program that
    // leaves a file behind: cmd.exe, as the "game".
    wchar_t system[MAX_PATH];
    UINT len = GetSystemDirectoryW(system, MAX_PATH);
    CHECK(len > 0 && len < MAX_PATH);
    std::wstring cmd = std::wstring(system) + L"\\cmd.exe";
    std::wstring marker = home.paths().home_dir + L"\\restarted";
    std::wstring args = L"4294967294|" + cmd + L"|cmd.exe /c echo x> \"" + marker + L"\"";
    TomeRestartW(nullptr, nullptr, args.data(), 0);
    bool appeared = false;
    for (int i = 0; i < 100 && !appeared; ++i) {
        appeared = GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (!appeared) Sleep(100);
    }
    CHECK(appeared);
}

DWORD WINAPI do_nothing(LPVOID) {
    return 0;
}

// The examples' handlers no fixture reaches in the e2e test: events its
// map doesn't cause, the C++ example's Greet key (a popup) and settings.
TEST(host_examples_events_keys_and_settings) {
    Home home;
    CHECK(home.install_mod("hello"));
    CHECK(home.install_mod("hello-cpp"));
    Session session;
    session.events = true;
    CHECK(session.start(home));
    tome::frames::set_main_thread(GetCurrentThreadId());
    tome::mod_manager().set_enabled("hello", true);
    tome::mod_manager().set_enabled("hello-cpp", true);

    tome_unit unit = {};
    unit.size = sizeof(unit);
    unit.name = "Footman";
    unit.type_id = 0x68666f6f;  // 'hfoo'
    tome_event_info info = tome::events::default_info_for_test(TOME_EVENT_RESEARCH_FINISHED);
    info.unit = &unit;
    info.object_name = "Defend";
    info.object_id = 0x52686465;  // 'Rhde'
    tome::events::dispatch_for_test(info);
    CHECK_LOG("[hello] Defend (Rhde) researched in Footman (hfoo) of player 1");
    for (int32_t event : {TOME_EVENT_UNIT_DETECTED, TOME_EVENT_UNIT_HIDDEN}) {
        info = tome::events::default_info_for_test(event);
        info.unit = &unit;
        info.player = 2;
        tome::events::dispatch_for_test(info);
    }
    CHECK_LOG("[hello] Footman (hfoo) of player 1 detected by player 3");
    CHECK_LOG("[hello] Footman (hfoo) of player 1 hidden by player 3");
    info = tome::events::default_info_for_test(TOME_EVENT_PLAYER_LEFT);
    info.player = 1;
    info.player_name = "Jaina";
    tome::events::dispatch_for_test(info);
    CHECK_LOG("[hello] player 2 (Jaina) left");

    // Keys. Popups need the game's window, which a test has none of: the
    // C++ example then greets at once instead.
    CHECK(tome::keybinds::press_for_test("hello-cpp", "greet"));
    CHECK_LOG("[hello-cpp] Greet key: ");
    CHECK(tome::keybinds::press_for_test("hello", "popup"));

    std::string error;
    CHECK_EQ(tome::mod_manager().set_setting("hello-cpp", "greeting", tome::json::Value::string("Hi"), error), TOME_OK);
    CHECK_LOG("[hello-cpp] setting changed: greeting");

    // A thread started now, as the game starts them: the mods' CRTs see it
    // (MinGW's would reset its x87 precision; sdk/support/fpreset.c).
    HANDLE thread = CreateThread(nullptr, 0, do_nothing, nullptr, 0, nullptr);
    CHECK(thread != nullptr);
    if (thread) {
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }
}

// The built-in patches against a mapping that lacks some of their symbols
// (another build's, say): those are skipped, saying which symbol; the rest
// install on the fake game image or fail there, and the report says which.
// patches::install points the hook manager at the build it's given: point it
// back at the session's when a test's own copy of the mapping goes away.
struct RestoreHookBuild {
    uintptr_t base;
    const tome::mappings::Build* build;
    ~RestoreHookBuild() { tome::hooks::init(base, build); }
};

TEST(host_patches_skipped_without_their_symbols) {
    Home home;
    Session session;
    CHECK(session.start(home));
    const tome::BuildMappings& known = *session.game.build().known;
    tome::mappings::Build data = *known.data;
    std::erase_if(data.symbols, [](const tome::mappings::Symbol& s) {
        return s.name == "Movie::SetDisplayMode";
    });
    tome::BuildMappings partial = known;
    partial.data = &data;
    RestoreHookBuild restore{session.game.base(), known.data};
    std::string report;
    tome::patches::install(session.game.base(), partial, report);
    if (report.find("movie-frame-length skipped (1.27b has no mapping for Movie::SetDisplayMode)") ==
        std::string::npos) {
        test::fail(__FILE__, __LINE__, report);
    }
}

// A patch the player turned off isn't applied (nor its functions reserved),
// on any build; the others still are.
TEST(host_patches_turned_off) {
    Home home;
    Session session;
    CHECK(session.start(home));
    RestoreHookBuild restore{session.game.base(), session.game.build().known->data};
    std::string report;
    tome::patches::install(session.game.base(), *session.game.build().known, report, [](std::string_view name) {
        return name == "memhack-guard" ? std::string("turned off by -tomenopatch") : std::string();
    });
    if (report.find("memhack-guard off (turned off by -tomenopatch)") == std::string::npos ||
        report.find(", 1 turned off") == std::string::npos ||
        report.find("lan-map-guard applied") == std::string::npos) {
        test::fail(__FILE__, __LINE__, report);
    }
    CHECK_EQ(tome::patches::count(tome::patches::State::Off), 1);
    // What install did is kept: the handshake and the mod menu read it.
    tome::patches::Outcome off = tome::patches::outcome("memhack-guard");
    CHECK(off.state == tome::patches::State::Off);
    CHECK_EQ(off.reason, std::string("turned off by -tomenopatch"));
    CHECK(tome::patches::outcome("lan-map-guard").state == tome::patches::State::Applied);
    CHECK(tome::patches::outcome("no-such-patch").state == tome::patches::State::NotInstalled);
    tome::settings::Schema menu = tome::tome_settings::menu_schema();
    const tome::settings::Setting* row = menu.find("patch.memhack-guard");
    CHECK(row && row->description.rfind("Not in force now: turned off by -tomenopatch. Closes", 0) == 0);
    row = menu.find("patch.lan-map-guard");
    CHECK(row && row->description.rfind("In force. Refuses", 0) == 0);
}

// Each patch has a setting, on until the player turns it off; patch_off
// says so from the next load (DllMain's, before the patches install).
TEST(host_tome_settings_patch_switches) {
    Home home;
    Session session;
    CHECK(session.start(home));
    CHECK(home.write(L"tome\\settings.json", "{\"patch.blp-guard\": false}"));
    tome::tome_settings::load(&home.paths());
    tome::tome_settings::init();
    CHECK_EQ(tome::tome_settings::patch_off("blp-guard"), std::string("turned off in Tome > Settings"));
    CHECK(tome::tome_settings::patch_off("memhack-guard").empty());
    const tome::settings::Schema& schema = tome::tome_settings::schema_with_patches();
    // One per patch but widescreen, whose own setting governs it.
    CHECK_EQ(schema.settings.size(), tome::tome_settings::schema().settings.size() + tome::patches::all().size() - 1);
    for (const tome::patches::Patch* p : tome::patches::all()) {
        CHECK(!schema.find(tome::tome_settings::patch_key(p->name)) == (std::string_view(p->name) == "widescreen"));
    }
    const tome::json::Value* on = tome::tome_settings::values().find("patch.memhack-guard");
    CHECK(on && on->is_bool() && on->as_bool());

    std::string error;
    CHECK(tome::tome_settings::set("patch.blp-guard", tome::json::Value::boolean(true), error));
    CHECK_LOG("Tome setting patch.blp-guard: On (from the next start)");
    CHECK(tome::tome_settings::set("patch.memhack-guard", tome::json::Value::boolean(false), error));
    tome::tome_settings::load(&home.paths());
    CHECK(tome::tome_settings::patch_off("blp-guard").empty());
    CHECK(!tome::tome_settings::patch_off("memhack-guard").empty());
    CHECK(tome::tome_settings::set("patch.memhack-guard", tome::json::Value::boolean(true), error));
}

// A patch's variant for a build: the one naming it, else the default; none
// without a default; with no variants, one that adds nothing.
TEST(host_patch_variant_for_a_build) {
    using tome::patches::Patch;
    using tome::patches::Variant;
    const char* const old_builds[] = {"1.24e", "1.26a", nullptr};
    const char* const extra[] = {"Extra", nullptr};
    int old_params = 1, default_params = 2;
    const Variant both[] = {{old_builds, extra, nullptr, &old_params}, {nullptr, nullptr, nullptr, &default_params}};
    Patch patch{};
    patch.variants = both;
    CHECK(tome::patches::variant_for(patch, "1.26a") == &both[0]);
    CHECK(tome::patches::variant_for(patch, "1.24e") == &both[0]);
    CHECK(tome::patches::variant_for(patch, "1.27b") == &both[1]);
    patch.variants = std::span<const Variant>(both, 1);
    CHECK(tome::patches::variant_for(patch, "1.27b") == nullptr);
    patch.variants = {};
    const Variant* plain = tome::patches::variant_for(patch, "1.27b");
    CHECK(plain && !plain->builds && !plain->symbols && !plain->skip && !plain->params);
}

// The same mapping under another build's name: the patches pick that
// build's variant. ambient-sound isn't for 1.26a (and says why);
// memhack-guard's 1.26a variant needs the shared constructor, which the fake
// game doesn't map.
TEST(host_patches_use_the_builds_variant) {
    Home home;
    Session session;
    CHECK(session.start(home));
    const tome::BuildMappings& known = *session.game.build().known;
    tome::mappings::Build data = *known.data;
    data.name = "1.26a";
    tome::BuildMappings renamed = known;
    renamed.data = &data;
    RestoreHookBuild restore{session.game.base(), known.data};
    std::string report;
    tome::patches::install(session.game.base(), renamed, report);
    for (const char* expected : {"ambient-sound skipped (not for 1.26a: its CSound::InitMIDI still makes the MIDI sound",
                                 "memhack-guard skipped (1.26a has no mapping for JassVarDesc::ctor)"}) {
        if (report.find(expected) == std::string::npos) test::fail(__FILE__, __LINE__, std::string(expected) + "\n" + report);
    }
}

// Every mapped function's first byte changed (another tool patched the
// game, say): no patch can hook its functions, and each one fails, saying
// which function.
TEST(host_patches_fail_on_changed_code) {
    Home home;
    Session session;
    CHECK(session.start(home));
    const tome::BuildMappings& known = *session.game.build().known;
    for (const tome::mappings::Symbol& s : known.data->symbols) {
        if (s.kind != tome::mappings::Kind::Function) continue;
        *reinterpret_cast<uint8_t*>(session.game.base() + s.value) ^= 0xFF;
    }
    std::string report;
    CHECK(!tome::patches::install(session.game.base(), known, report) || report.find("FAILED") != std::string::npos);
    for (const char* failed : {"memhack-guard FAILED (Jass::compile: ", "blp-guard FAILED (CBLPFile::Open: ",
                               "ambient-sound FAILED (CSound::InitMIDI: ",
                               "movie-frame-length FAILED (Movie::SetDisplayMode: "}) {
        if (report.find(failed) == std::string::npos) test::fail(__FILE__, __LINE__, std::string(failed) + "\n" + report);
    }
}

// An old layout's <home>/mods (before tome/mods): moved, with each mod's
// config/ going to tome/mods/config/<id>, as far as that's possible.
TEST(host_old_layout_migrated) {
    Home home;
    host::open_log(home);
    tome::Paths paths;
    paths.home_dir = home.paths().home_dir + L"\\old";
    paths.tome_dir = paths.home_dir + L"\\tome";
    paths.mods_dir = paths.tome_dir + L"\\mods";
    paths.mod_config_dir = paths.mods_dir + L"\\config";
    auto dir = [&](const std::wstring& relative) {
        CreateDirectoryW((paths.home_dir + relative).c_str(), nullptr);
    };
    auto file = [&](const std::wstring& relative, const std::string& text) {
        CHECK(tome::write_file_atomic(paths.home_dir + relative, text));
    };
    for (const wchar_t* d : {L"", L"\\tome", L"\\mods", L"\\mods\\alpha", L"\\mods\\alpha\\config", L"\\mods\\beta",
                             L"\\mods\\beta\\config", L"\\mods\\nameless", L"\\mods\\nameless\\config",
                             L"\\mods\\plain"}) {
        dir(d);
    }
    file(L"\\mods\\alpha\\tome.mod.json", R"({"id": "alpha"})");
    file(L"\\mods\\alpha\\config\\settings.json", R"({"x": 1})");
    file(L"\\mods\\alpha\\config\\notes.txt", "kept");
    file(L"\\mods\\beta\\tome.mod.json", R"({"id": "beta"})");
    file(L"\\mods\\beta\\config\\settings.json", R"({"y": 2})");
    file(L"\\mods\\nameless\\tome.mod.json", "{}");
    file(L"\\mods\\nameless\\config\\a.txt", "a");
    file(L"\\mods\\stray.txt", "not a mod");

    std::vector<std::string> notes = tome::migrate_layout(paths);
    std::string all;
    for (const std::string& n : notes) all += n + "\n";
    for (const char* expected : {"moved ", "moved alpha\\config\\settings.json to config\\alpha.json",
                                 "moved alpha\\config to config\\alpha", "moved beta\\config\\settings.json to config\\beta.json",
                                 "nameless\\config: no mod id in tome.mod.json; left where it is"}) {
        if (all.find(expected) == std::string::npos) test::fail(__FILE__, __LINE__, std::string(expected) + "\n" + all);
    }
    std::string text;
    CHECK(tome::read_file(paths.mod_config_dir + L"\\alpha.json", text) && text == R"({"x": 1})");
    CHECK(tome::read_file(paths.mod_config_dir + L"\\alpha\\notes.txt", text) && text == "kept");
    CHECK(GetFileAttributesW((paths.mods_dir + L"\\beta\\config").c_str()) == INVALID_FILE_ATTRIBUTES);  // emptied, removed

    // An old mods folder next to the new one: left alone, with a note.
    dir(L"\\mods");
    notes = tome::migrate_layout(paths);
    CHECK(notes.size() == 1 && notes[0].find("isn't read any more") != std::string::npos);
}

// A mod's config/ moved while config\<id> (or <id>.json) is there already.
TEST(host_old_layout_conflicts_left_alone) {
    Home home;
    host::open_log(home);
    tome::Paths paths;
    paths.home_dir = home.paths().home_dir + L"\\old";
    paths.tome_dir = paths.home_dir + L"\\tome";
    paths.mods_dir = paths.tome_dir + L"\\mods";
    paths.mod_config_dir = paths.mods_dir + L"\\config";
    for (const wchar_t* d : {L"", L"\\tome", L"\\mods", L"\\mods\\gamma", L"\\mods\\gamma\\config", L"\\mods\\config",
                             L"\\mods\\config\\gamma"}) {
        CreateDirectoryW((paths.home_dir + d).c_str(), nullptr);
    }
    CHECK(tome::write_file_atomic(paths.home_dir + L"\\mods\\gamma\\tome.mod.json", R"({"id": "gamma"})"));
    CHECK(tome::write_file_atomic(paths.home_dir + L"\\mods\\gamma\\config\\settings.json", "{}"));
    CHECK(tome::write_file_atomic(paths.home_dir + L"\\mods\\gamma\\config\\more.txt", "x"));
    CHECK(tome::write_file_atomic(paths.home_dir + L"\\mods\\config\\gamma.json", "{}"));
    std::vector<std::string> notes = tome::migrate_layout(paths);
    std::string all;
    for (const std::string& n : notes) all += n + "\n";
    for (const char* expected : {"gamma\\config\\settings.json: config\\gamma.json exists already; not moved",
                                 "gamma\\config: config\\gamma exists already; not moved"}) {
        if (all.find(expected) == std::string::npos) test::fail(__FILE__, __LINE__, std::string(expected) + "\n" + all);
    }
}

// The API's small functions and refusals, called the way a mod would.
TEST(host_api_basics_and_refusals) {
    Home home;
    CHECK(home.install_mod("hello"));
    Session session;
    const tome_api* api = start(home, session);
    CHECK(api != nullptr);
    if (!api) return;
    tome_mod* mod = make_mod("plain");
    mod->config_dir_w = home.paths().mod_config_dir + L"\\plain";
    mod->config_dir = tome::narrow(mod->config_dir_w);
    CHECK_EQ(std::string(api->mod_id(mod)), std::string("plain"));
    CHECK_EQ(std::string(api->mod_id(nullptr)), std::string(""));
    CHECK_EQ(std::string(api->mod_version(mod)), std::string("1.0.0"));
    CHECK_EQ(std::string(api->mod_version(nullptr)), std::string(""));
    CHECK_EQ(std::string(api->config_dir(nullptr)), std::string(""));
    CHECK_EQ(std::string(api->config_dir(mod)), mod->config_dir);
    CHECK(GetFileAttributesW(mod->config_dir_w.c_str()) != INVALID_FILE_ATTRIBUTES);  // made on demand

    api->log(mod, TOME_LOG_WARN, "a warning");
    api->log(mod, TOME_LOG_DEBUG, "a detail");
    CHECK_LOG("[plain] a warning");

    // Natives: one nobody declared, and hello's (declared, so another mod's).
    CHECK_EQ(api->register_native(mod, "NobodysNative", reinterpret_cast<void*>(&do_nothing)), TOME_ERR_NOT_DECLARED);
    CHECK_EQ(api->register_native(mod, "HelloLog", reinterpret_cast<void*>(&do_nothing)), TOME_ERR_NOT_OWNER);
    CHECK_LOG("[plain] register_native(HelloLog): declared by another mod");

    // No Storm.dll in the fake game: no game files.
    char buffer[16];
    CHECK_EQ(api->read_game_file("war3map.j", buffer, sizeof(buffer)), size_t(0));
    CHECK_EQ(api->read_game_file(nullptr, buffer, sizeof(buffer)), size_t(0));

    // Hooks: a name that isn't mapped; unhooking what was never hooked.
    auto handler = [](tome_mod*, tome_hook_call*, void*) {};
    CHECK_EQ(api->hook(mod, "No::SuchFunction", TOME_HOOK_BEFORE, 0, handler, nullptr), TOME_ERR_UNAVAILABLE);
    CHECK_LOG("[plain] hook(No::SuchFunction): ");
    CHECK_EQ(api->unhook(mod, "No::SuchFunction", TOME_HOOK_BEFORE, handler, nullptr), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(api->unhook(nullptr, "RCString::GetText", TOME_HOOK_BEFORE, handler, nullptr), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(api->hook(mod, "RCString::GetText", TOME_HOOK_BEFORE, 0, handler, nullptr), TOME_OK);
    CHECK_EQ(api->unhook(mod, "RCString::GetText", TOME_HOOK_BEFORE, handler, nullptr), TOME_OK);
    CHECK_EQ(api->unhook(mod, "RCString::GetText", TOME_HOOK_BEFORE, handler, nullptr), TOME_ERR_INVALID_ARGUMENT);

    // A function a built-in patch reserved.
    std::string report;
    tome::patches::install(session.game.base(), *session.game.build().known, report);
    CHECK_EQ(api->hook(mod, "Jass::compile", TOME_HOOK_BEFORE, 0, handler, nullptr), TOME_ERR_NOT_OWNER);
    CHECK_LOG("[plain] hook(Jass::compile): reserved by a built-in Tome patch");
    CHECK_EQ(api->unhook(mod, "Jass::compile", TOME_HOOK_BEFORE, handler, nullptr), TOME_ERR_NOT_OWNER);
    // A variant reserves only what it hooks: memhack-guard's default one the
    // local constructor, not 1.26a's shared one.
    CHECK(tome::hooks::is_reserved("JassLocalVarDesc::ctor"));
    CHECK(!tome::hooks::is_reserved("JassVarDesc::ctor"));
}

// Game objects (API version 16) outside a game: nothing is an object, and
// properties and natives are unavailable instead of touching anything.
TEST(host_objects_outside_a_game) {
    Home home;
    Session session;
    const tome_api* api = start(home, session);
    CHECK(api != nullptr);
    if (!api) return;
    CHECK(tome_has_objects(api));
    tome_mod* mod = make_mod("plain");
    uint32_t not_an_object[8] = {};
    int32_t type = -1;
    CHECK_EQ(api->object_type(nullptr), int32_t(TOME_OBJECT_NONE));
    CHECK_EQ(api->object_type(not_an_object), int32_t(TOME_OBJECT_NONE));
    CHECK(api->object_of(0x100000, &type) == nullptr);
    CHECK_EQ(type, int32_t(TOME_OBJECT_NONE));
    CHECK_EQ(api->handle_of(not_an_object), 0u);
    CHECK(api->player_object(0) == nullptr);
    tome_unit unit;
    CHECK(tome_get_unit_info(api, not_an_object, &unit) != TOME_OK);
    CHECK_EQ(api->unit_info(not_an_object, nullptr, sizeof(unit)), TOME_ERR_INVALID_ARGUMENT);
    double value = 1;
    CHECK(api->get_property(not_an_object, TOME_PROPERTY_LIFE, &value) != TOME_OK);
    CHECK(value == 0);
    CHECK_EQ(api->get_property(not_an_object, TOME_PROPERTY_LIFE, nullptr), TOME_ERR_INVALID_ARGUMENT);
    CHECK(tome_property_value(api, not_an_object, TOME_PROPERTY_MANA, -1) == -1);
    CHECK_EQ(api->set_property(nullptr, not_an_object, TOME_PROPERTY_LIFE, 1), TOME_ERR_INVALID_ARGUMENT);
    CHECK(api->set_property(mod, not_an_object, TOME_PROPERTY_LIFE, 1) != TOME_OK);
    tome_jass_value args[1] = {tome_jass_int(42)};
    tome_jass_value result = tome_jass_int(7);
    CHECK(api->jass_call(mod, "I2S", args, 1, &result) != TOME_OK);
    CHECK(result.string == nullptr);  // zeroed whatever happens
    CHECK_EQ(api->jass_call(mod, nullptr, args, 1, &result), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(api->jass_call(mod, "I2S", nullptr, 1, &result), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(api->jass_call(nullptr, "I2S", args, 1, &result), TOME_ERR_INVALID_ARGUMENT);
    void* listed[4];
    // No game (0), or the session didn't install the objects hooks (-1): nothing either way.
    CHECK(api->list_objects(TOME_OBJECT_UNIT, listed, 4) <= 0);
    CHECK(api->list_objects(TOME_OBJECT_PLAYER, nullptr, 0) <= 0);
    CHECK_EQ(api->list_objects(TOME_OBJECT_OTHER, listed, 4), -1);
    CHECK_EQ(api->list_objects(TOME_OBJECT_UNIT, nullptr, 4), -1);
    CHECK_EQ(api->list_objects(TOME_OBJECT_UNIT, listed, -1), -1);
    // No game: nobody hosts.
    CHECK(tome_has_host_player(api));
    CHECK_EQ(api->host_player(), -1);
    // Messages between players: only in a game; bad arguments first.
    CHECK(tome_has_net_messages(api));
    char bytes[TOME_NET_MAX_MESSAGE + 1] = {};
    CHECK_EQ(api->net_send(mod, nullptr, 4), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(api->net_send(mod, bytes, TOME_NET_MAX_MESSAGE + 1), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(api->net_send(nullptr, bytes, 4), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(api->net_send(mod, bytes, TOME_NET_MAX_MESSAGE), TOME_ERR_UNAVAILABLE);
    CHECK_EQ(api->net_send(mod, nullptr, 0), TOME_ERR_UNAVAILABLE);
    CHECK_EQ(api->on_net_message(mod, nullptr, nullptr), TOME_ERR_INVALID_ARGUMENT);
    // The lobby (API version 21): no lobby here; bad arguments first.
    CHECK(tome_has_lobby(api));
    char lobby_bytes[TOME_LOBBY_MAX_MESSAGE + 1] = {};
    CHECK_EQ(api->lobby_send(mod, 0, nullptr, 4), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(api->lobby_send(mod, 0, lobby_bytes, TOME_LOBBY_MAX_MESSAGE + 1), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(api->lobby_send(nullptr, 0, lobby_bytes, 4), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(api->lobby_send(mod, TOME_LOBBY_EVERYONE, lobby_bytes, TOME_LOBBY_MAX_MESSAGE), TOME_ERR_UNAVAILABLE);
    CHECK_EQ(api->on_lobby_message(mod, nullptr, nullptr), TOME_ERR_INVALID_ARGUMENT);
    tome_lobby_player players[2];
    CHECK_EQ(api->lobby_players(players, 2), -1);
    CHECK_EQ(api->lobby_players(nullptr, 0), -1);
    CHECK_EQ(api->lobby_players(nullptr, 2), -1);
    CHECK_EQ(api->on_lobby_start(mod, nullptr, nullptr), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(api->lobby_start(nullptr), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(api->lobby_start(mod), TOME_ERR_UNAVAILABLE);
    CHECK_EQ(api->lobby_show(mod, nullptr), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(api->lobby_show(mod, "hello"), TOME_ERR_UNAVAILABLE);
}

// Versions and the installed mods (API version 20): the mod list as the
// mod menu has it, kept alive for the mod that asked, and the event that
// says it changed once a refresh installs, removes or toggles a mod.
int g_mods_changed = 0;
void TOME_CALL count_mods_changed(tome_mod*, const tome_event_info* info, void*) {
    if (info->event == TOME_EVENT_MODS_CHANGED) ++g_mods_changed;
}

const tome_mod_info* find_mod(const std::vector<tome_mod_info>& list, const char* id) {
    for (const tome_mod_info& m : list) {
        if (std::strcmp(m.id, id) == 0) return &m;
    }
    return nullptr;
}

// A mod without code: tome/mods/<folder>/tome.mod.json.
bool write_mod(Home& home, const wchar_t* folder, const char* metadata) {
    CreateDirectoryW((home.paths().mods_dir + L"\\" + folder).c_str(), nullptr);
    return home.write(std::wstring(L"tome\\mods\\") + folder + L"\\tome.mod.json", metadata);
}

std::vector<tome_mod_info> list_all(const tome_api* api, tome_mod* mod) {
    int32_t count = tome_list_mods(api, mod, nullptr, 0);
    std::vector<tome_mod_info> list(count > 0 ? static_cast<size_t>(count) : 0);
    if (count > 0) CHECK_EQ(tome_list_mods(api, mod, list.data(), count), count);
    return list;
}

TEST(host_versions_and_installed_mods) {
    Home home;
    CHECK(home.install_mod("hello"));
    CHECK(write_mod(home, L"lib-plain",
                     R"({"schema": 1, "id": "lib-plain", "version": "2.1.0-rc.1", "name": "Plain library",
                         "game": "*", "tome": "*", "provides": ["plain-api"], "sync": "required"})"));
    CHECK(write_mod(home, L"needs-lib",
                     R"({"schema": 1, "id": "needs-lib", "version": "1.0.0", "game": "*", "tome": "*", "sync": "client",
                         "depends": {"missing-lib": "^1"}})"));
    CHECK(write_mod(home, L"broken", "{not json"));
    // Valid metadata, but its DLL is missing: an error that still says what it is.
    CHECK(write_mod(home, L"no-dll",
                     R"({"schema": 1, "id": "no-dll", "version": "3.0.0", "name": "No DLL", "game": "*", "tome": "*",
                         "sync": "client", "entry": "gone.dll"})"));
    Session session;
    session.events = true;
    const tome_api* api = start(home, session);
    CHECK(api != nullptr);
    if (!api) return;
    CHECK(tome_has_mod_list(api));
    CHECK_EQ(api->api_version, 21u);

    // Versions, by Tome's rules; the details are in tests/test_version.cpp.
    int32_t order = 5;
    CHECK_EQ(api->version_compare(TOME_VERSION_MOD, "1.2.3", "1.10.0", &order), TOME_OK);
    CHECK_EQ(order, -1);
    CHECK_EQ(api->version_compare(TOME_VERSION_GAME, "1.27b", "1.27.1.7085", &order), TOME_OK);
    CHECK_EQ(order, 0);
    CHECK_EQ(api->version_compare(TOME_VERSION_MOD, "1.2", "1.2.0", &order), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(api->version_compare(TOME_VERSION_MOD, nullptr, "1.2.0", &order), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(api->version_compare(TOME_VERSION_MOD, "1.2.0", "1.2.0", nullptr), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(api->version_compare(7, "1.2.0", "1.2.0", &order), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(api->version_match(TOME_VERSION_MOD, "^1.2", "1.9.0"), 1);
    CHECK_EQ(api->version_match(TOME_VERSION_MOD, "^1.2", "2.0.0"), 0);
    CHECK_EQ(api->version_match(TOME_VERSION_MOD, "*", api->tome_version), 1);
    CHECK_EQ(api->version_match(TOME_VERSION_GAME, ">=1.26a <1.28", api->game_info()->file_version), 1);
    CHECK_EQ(api->version_match(TOME_VERSION_MOD, ">=banana", "1.0.0"), -1);
    CHECK_EQ(api->version_match(TOME_VERSION_GAME, nullptr, "1.27b"), -1);
    CHECK_EQ(api->version_match(-1, "*", "1.0.0"), -1);

    CHECK_EQ(std::string(api->mods_dir()), tome::narrow(home.paths().mods_dir));

    // A release's tome.mod.json, before it's installed: checked against this
    // Tome, the fake 1.27b game and its mapping database.
    const std::string fits =
        R"({"schema": 1, "id": "release", "version": "2.0.0", "game": "1.27b", "tome": "*", "sync": "client",
            "requires": ["CTextFrame::SetText"]})";
    char reason[128] = "untouched";
    CHECK_EQ(api->check_metadata(fits.data(), fits.size(), reason, sizeof(reason)), 1);
    CHECK_EQ(std::string(reason), std::string());
    const std::string other_game =
        R"({"schema": 1, "id": "release", "version": "2.0.0", "game": "1.26a", "tome": "*", "sync": "client"})";
    CHECK_EQ(api->check_metadata(other_game.data(), other_game.size(), reason, sizeof(reason)), 0);
    CHECK_EQ(std::string(reason), std::string("supports game 1.26a, this is 1.27b"));
    const std::string unmapped =
        R"({"schema": 1, "id": "release", "version": "2.0.0", "game": "*", "tome": "*", "sync": "client",
            "requires": ["No::SuchFunction"]})";
    CHECK_EQ(api->check_metadata(unmapped.data(), unmapped.size(), nullptr, 0), 0);
    CHECK_EQ(api->check_metadata("{", 1, reason, 4), -1);
    CHECK_EQ(std::string(reason), std::string("tom"));  // cut to fit
    CHECK_EQ(api->check_metadata(nullptr, 5, reason, sizeof(reason)), -1);
    CHECK_EQ(api->check_metadata(nullptr, 0, reason, sizeof(reason)), -1);  // empty text

    tome_mod* mod = make_mod("zeppelin");
    CHECK_EQ(api->list_mods(nullptr, nullptr, 0, 0), -1);
    CHECK_EQ(api->list_mods(mod, nullptr, sizeof(tome_mod_info), 1), -1);
    CHECK_EQ(api->list_mods(mod, nullptr, sizeof(tome_mod_info), -1), -1);
    std::vector<tome_mod_info> list = list_all(api, mod);
    CHECK_EQ(list.size(), size_t(5));
    const tome_mod_info* hello = find_mod(list, "hello");
    const tome_mod_info* lib = find_mod(list, "lib-plain");
    const tome_mod_info* needs = find_mod(list, "needs-lib");
    const tome_mod_info* broken = find_mod(list, "");
    CHECK(hello && lib && needs && broken);
    if (!hello || !lib || !needs || !broken) return;
    // A DLL nobody allowed yet: waits for the player.
    CHECK_EQ(hello->state, TOME_MOD_APPROVAL);
    CHECK_EQ(hello->enabled, 0);
    CHECK_EQ(std::string(hello->folder), std::string("hello"));
    CHECK(hello->reason[0] != '\0');
    CHECK_EQ(lib->enabled, 1);
    CHECK_EQ(std::string(lib->reason), std::string());
    CHECK_EQ(lib->state, TOME_MOD_ACTIVE);
    CHECK_EQ(std::string(lib->version), std::string("2.1.0-rc.1"));
    CHECK_EQ(std::string(lib->name), std::string("Plain library"));
    CHECK_EQ(lib->sync_required, 1);
    CHECK_EQ(lib->package, 0);
    CHECK_EQ(lib->provides_count, 1);
    CHECK_EQ(std::string(lib->provides[0]), std::string("plain-api"));
    CHECK_EQ(std::string(lib->dir), tome::narrow(home.paths().mods_dir + L"\\lib-plain"));
    CHECK_EQ(needs->state, TOME_MOD_ERROR);
    CHECK(std::string(needs->reason).find("missing-lib") != std::string::npos);
    CHECK_EQ(broken->state, TOME_MOD_ERROR);
    CHECK_EQ(std::string(broken->folder), std::string("broken"));
    CHECK_EQ(std::string(broken->version), std::string());
    const tome_mod_info* no_dll = find_mod(list, "no-dll");
    CHECK(no_dll != nullptr);
    if (no_dll) {
        CHECK_EQ(no_dll->state, TOME_MOD_ERROR);
        CHECK_EQ(std::string(no_dll->version), std::string("3.0.0"));
        CHECK_EQ(std::string(no_dll->name), std::string("No DLL"));
        CHECK(std::string(no_dll->reason).find("gone.dll") != std::string::npos);
    }

    // Nothing changed since: a refresh says nothing.
    CHECK_EQ(api->on_event(mod, TOME_EVENT_MODS_CHANGED, count_mods_changed, nullptr), TOME_OK);
    tome::mod_manager().refresh();
    CHECK_EQ(g_mods_changed, 0);

    // A mod installed: the next refresh lists it and says so. The strings
    // of the list this mod was given before stay valid until it asks again.
    const char* old_lib_version = lib->version;
    CHECK(write_mod(home, L"missing-lib",
                     R"({"schema": 1, "id": "missing-lib", "version": "1.4.0", "game": "*", "tome": "*", "sync": "client"})"));
    tome::mod_manager().refresh();
    CHECK_EQ(g_mods_changed, 1);
    CHECK_EQ(std::string(old_lib_version), std::string("2.1.0-rc.1"));
    CHECK_EQ(tome_list_mods(api, mod, nullptr, 0), 6);  // counting keeps the old list
    CHECK_EQ(std::string(old_lib_version), std::string("2.1.0-rc.1"));
    list = list_all(api, mod);
    CHECK_EQ(list.size(), size_t(6));
    needs = find_mod(list, "needs-lib");
    CHECK(needs && needs->state == TOME_MOD_ACTIVE);
    CHECK(find_mod(list, "missing-lib") != nullptr);

    // Turned off by the player: disabled, not wanted, no reason.
    tome::mod_manager().set_enabled("needs-lib", false);
    CHECK_EQ(g_mods_changed, 2);
    list = list_all(api, mod);
    needs = find_mod(list, "needs-lib");
    CHECK(needs != nullptr);
    if (needs) {
        CHECK_EQ(needs->state, TOME_MOD_DISABLED);
        CHECK_EQ(needs->enabled, 0);
        CHECK_EQ(std::string(needs->reason), std::string());
    }

    // Removed.
    CHECK(home.remove(L"tome\\mods\\broken"));
    tome::mod_manager().refresh();
    CHECK_EQ(g_mods_changed, 3);
    list = list_all(api, mod);
    CHECK_EQ(list.size(), size_t(5));
    CHECK(find_mod(list, "") == nullptr);
}

// Keybinds: a broken keybinds.json, the game's CustomKeys.txt, keys asked
// for by name, one the mod never declared, and resetting every key.
TEST(host_keybinds_files_and_reset) {
    Home home;
    CHECK(home.install_mod("hello"));
    CHECK(home.write(L"tome\\keybinds.json", "[]"));
    Session session;
    const tome_api* api = start(home, session);
    CHECK(api != nullptr);
    if (!api) return;
    tome::Paths paths = home.paths();
    paths.game_dir = paths.home_dir;  // CustomKeys.txt is the game's
    CHECK(home.write(L"CustomKeys.txt", "[AHbz]\nHotkey=B\n"));
    tome::keybinds::init(paths);
    CHECK_LOG("tome/keybinds.json is invalid (not an object); using every mod's default keys");
    CHECK_LOG("CustomKeys.txt: 1 hotkeys, checked against keybinds");
    tome::mod_manager().set_enabled("hello", true);

    tome_mod* mod = make_mod("hello-alike");
    auto on_key = [](tome_mod*, const char*, void*) {};
    CHECK_EQ(api->on_keybind(mod, "undeclared", on_key, nullptr), TOME_ERR_NOT_DECLARED);
    CHECK_LOG("[hello-alike] on_keybind(undeclared): not declared in tome.mod.json \"keybinds\"");

    // hello's Greet key by name, into a buffer too small, and whole.
    tome_mod* hello = make_mod("hello");
    char name[32];
    const size_t size = api->keybind_name(hello, "greet", nullptr, 0);
    CHECK(size > 0);
    CHECK_EQ(api->keybind_name(hello, "greet", name, 3), size);
    CHECK_EQ(std::strlen(name), size_t(2));
    CHECK_EQ(api->keybind_name(hello, "greet", name, sizeof(name)), size);
    CHECK_EQ(std::string(name), std::string("Ctrl+G"));

    tome::keys::Key key;
    CHECK(tome::keys::parse("Ctrl+J", key));
    tome::keybinds::set("hello", "greet", key);
    CHECK_EQ(api->keybind_name(hello, "greet", name, sizeof(name)), size_t(6));
    CHECK_EQ(std::string(name), std::string("Ctrl+J"));
    tome::keybinds::reset_all();
    CHECK_LOG("Keybinds reset to their defaults");
    api->keybind_name(hello, "greet", name, sizeof(name));
    CHECK_EQ(std::string(name), std::string("Ctrl+G"));
}

// background-mute: a flag left by a run that ended while muted, then the
// game window going to the background and back, and closing. With or
// without an audio device (each path logs what it did), nothing is left
// muted and the flag goes.
TEST(host_background_mute_round_trip) {
    Home home;
    Session session;
    CHECK(session.start(home));
    std::string report;
    tome::patches::install(session.game.base(), *session.game.build().known, report);
    CHECK(report.find("background-mute applied") != std::string::npos);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TomeTestGame";
    RegisterClassW(&wc);
    HWND window = CreateWindowW(L"TomeTestGame", L"game", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr,
                                wc.hInstance, nullptr);
    CHECK(window != nullptr);
    if (!window) return;
    CHECK(tome::main_thread::install(window));

    CHECK(home.write(L"tome\\background-mute.flag", ""));
    tome::patches::background_mute_start(home.paths().tome_dir);
    CHECK(!home.exists(L"tome\\background-mute.flag"));

    SendMessageW(window, WM_ACTIVATEAPP, FALSE, 0);
    SendMessageW(window, WM_ACTIVATEAPP, TRUE, 0);
    SendMessageW(window, WM_ACTIVATEAPP, FALSE, 0);
    DestroyWindow(window);
    CHECK(!home.exists(L"tome\\background-mute.flag"));
}
