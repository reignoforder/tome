// SPDX-License-Identifier: GPL-3.0-or-later
// sdk/include/tome/tome.hpp against a mock API table: Mod::call's argument
// binding, version checks, callbacks and their lifetime, the helpers.
// Built like Tome itself (no exceptions), so this is also the header's
// no-exceptions configuration; hello-cpp builds it with them, as C++17.
#include <tome/tome.hpp>

#include "test.h"

#include <cstddef>
#include <cstring>
#include <string>

namespace {

tome_mod* const kMod = reinterpret_cast<tome_mod*>(0x1234);

std::string last_log;
tome_mod* last_mod;
std::string last_name;
tome_event_fn event_fn;
void* event_user;
void* native_fn;

void TOME_CALL mock_log(tome_mod* mod, tome_log_level, const char* message) {
    last_mod = mod;
    last_log = message;
}

tome_result TOME_CALL mock_on_event(tome_mod* mod, int32_t, tome_event_fn fn, void* user) {
    last_mod = mod;
    event_fn = fn;
    event_user = user;
    return TOME_OK;
}

tome_result TOME_CALL mock_register_command(tome_mod* mod, const char* name, tome_command_fn, void*) {
    last_mod = mod;
    last_name = name;
    return std::string(name) == "taken" ? TOME_ERR_NOT_OWNER : TOME_OK;
}

tome_click_fn click_fn;
void* click_user;
tome_result TOME_CALL mock_frame_on_click(tome_mod*, tome_frame*, tome_click_fn fn, void* user) {
    click_fn = fn;
    click_user = user;
    return TOME_OK;
}

tome_result TOME_CALL mock_on_keybind(tome_mod*, const char*, tome_keybind_fn, void*) {
    return TOME_OK;
}

tome_popup_fn popup_fn;
void* popup_user;
tome_result TOME_CALL mock_popup(tome_mod*, const tome_popup*, tome_popup_fn fn, void* user, uint32_t*) {
    popup_fn = fn;
    popup_user = user;
    return TOME_OK;
}

tome_result TOME_CALL mock_register_native(tome_mod*, const char* name, void* fn) {
    last_name = name;
    native_fn = fn;
    return TOME_OK;
}

size_t TOME_CALL mock_setting_string(tome_mod*, const char* key, char* buffer, size_t size) {
    std::string value = std::string(key) == "greeting" ? "Γειά σου" : "";
    if (size) snprintf(buffer, size, "%s", value.c_str());
    return value.size();
}

double TOME_CALL mock_setting_number(tome_mod*, const char*, double fallback) {
    return fallback * 2;
}

const tome_symbol* TOME_CALL mock_resolve(const char*) {
    static tome_symbol sym = {sizeof(tome_symbol), "X", TOME_SYMBOL_FUNCTION, 0x1000, 0x10, "cdecl", "void()"};
    return &sym;
}

tome_api table(size_t size) {
    tome_api api;
    std::memset(&api, 0, sizeof(api));
    api.size = static_cast<uint32_t>(size);
    api.api_version = TOME_API_VERSION;
    api.log = mock_log;
    api.register_native = mock_register_native;
    api.resolve = mock_resolve;
    api.on_event = mock_on_event;
    api.register_command = mock_register_command;
    api.setting_string = mock_setting_string;
    api.setting_number = mock_setting_number;
    api.frame_on_click = mock_frame_on_click;
    api.on_keybind = mock_on_keybind;
    api.popup = mock_popup;
    return api;
}

int32_t twice(tome_jass_integer value) {
    return value * 2;
}

struct Counted {
    int* drops;
    explicit Counted(int* d) : drops(d) {}
    Counted(Counted&& other) noexcept : drops(other.drops) { other.drops = nullptr; }
    ~Counted() {
        if (drops) ++*drops;
    }
};

} // namespace

TEST(tome_hpp_call_binds_arguments) {
    tome_api api = table(sizeof(tome_api));
    tome::Mod mod(&api, kMod);
    // The mod is filled in, a std::string passes as const char*.
    CHECK_EQ(mod.call(&tome_api::register_command, std::string("hi"), [](const char*) {}), TOME_OK);
    CHECK_EQ(last_mod, kMod);
    CHECK_EQ(last_name, std::string("hi"));
    // Functions without a tome_mod*, and results other than tome_result.
    CHECK_EQ(mod.call(&tome_api::resolve, "X")->value, uintptr_t(0x1000));
    CHECK_EQ(mod.call(&tome_api::setting_number, "seconds", 2.5), 5.0);
    // A function as void*, and tome::native's wrapper calls through.
    CHECK_EQ(mod.call(&tome_api::register_native, "Twice", tome::native<twice>), TOME_OK);
    CHECK_EQ(reinterpret_cast<int32_t (*)(tome_jass_integer)>(native_fn)(21), 42);
    mod.log(TOME_LOG_INFO, "%d and %s", 1, "two");
    CHECK_EQ(last_log, std::string("1 and two"));
}

TEST(tome_hpp_call_checks_the_table) {
    // A Tome whose table ends before on_event (API version 4).
    tome_api api = table(offsetof(tome_api, on_event));
    tome::Mod mod(&api, kMod);
    CHECK(mod.has(&tome_api::resolve));
    CHECK(!mod.has(&tome_api::on_event));
    event_fn = nullptr;
    CHECK_EQ(mod.call(&tome_api::on_event, TOME_EVENT_TICK, [](const tome_event_info*) {}), TOME_ERR_UNAVAILABLE);
    CHECK(event_fn == nullptr);
    CHECK_EQ(mod.text(&tome_api::setting_string, "greeting"), std::string());
    CHECK_EQ(mod.call(&tome_api::setting_number, "seconds", 2.5), 0.0);
    // A NULL entry counts as missing too.
    tome_api full = table(sizeof(tome_api));
    full.popup = nullptr;
    CHECK(!tome::Mod(&full, kMod).has(&tome_api::popup));
    CHECK(tome::Mod(&full, kMod).call(&tome_api::ui_root, TOME_UI_GAME) == nullptr);
}

TEST(tome_hpp_callbacks) {
    tome_api api = table(sizeof(tome_api));
    tome::Mod mod(&api, kMod);
    tome_event_info info = tome::sized<tome_event_info>();
    info.event = TOME_EVENT_HERO_LEVEL;
    info.level = 3;
    int seen = 0;
    tome_mod* seen_mod = nullptr;
    // Without the tome_mod*...
    mod.call(&tome_api::on_event, TOME_EVENT_HERO_LEVEL, [&](const tome_event_info* i) { seen += i->level; });
    event_fn(kMod, &info, event_user);
    CHECK_EQ(seen, 3);
    // ...or with it.
    mod.call(&tome_api::on_event, TOME_EVENT_HERO_LEVEL, [&](tome_mod* m, const tome_event_info* i) {
        seen_mod = m;
        seen += 10 * i->level;
    });
    event_fn(kMod, &info, event_user);
    CHECK_EQ(seen, 33);
    CHECK_EQ(seen_mod, kMod);
    // A plain C callback still passes as it is.
    CHECK_EQ(mod.call(&tome_api::on_event, TOME_EVENT_TICK, static_cast<tome_event_fn>(nullptr), nullptr), TOME_OK);
    CHECK(event_fn == nullptr);
}

TEST(tome_hpp_callbacks_live_until_the_next_enable) {
    tome_api api = table(sizeof(tome_api));
    CHECK_EQ(tome::detail::load(&api, kMod, [](tome::Mod) { return true; }), 0);
    tome::detail::enable(kMod, nullptr);
    int drops = 0;
    tome::mod().call(&tome_api::on_event, TOME_EVENT_TICK,
                     [c = Counted(&drops)](const tome_event_info*) { (void)c; });
    // Tome revokes it after disable, but it may still be on the stack.
    tome::detail::disable(kMod, nullptr);
    CHECK_EQ(drops, 0);
    tome::detail::enable(kMod, nullptr);
    CHECK_EQ(drops, 1);
    CHECK_EQ(tome::detail::load(&api, kMod, [](tome::Mod) { return false; }), 1);
    tome::detail::unload(kMod, nullptr);
}

TEST(tome_hpp_helpers) {
    tome_api api = table(sizeof(tome_api));
    tome::Mod mod(&api, kMod);
    CHECK_EQ(mod.text(&tome_api::setting_string, "greeting"), std::string("Γειά σου"));
    CHECK_EQ(mod.text(&tome_api::setting_string, "missing"), std::string());
    tome_event_info info = tome::sized<tome_event_info>();
    CHECK_EQ(info.size, uint32_t(sizeof(tome_event_info)));
    CHECK(tome::has_field(&info, &tome_event_info::amount));
    info.size = offsetof(tome_event_info, amount);  // an API version 8 Tome
    CHECK(!tome::has_field(&info, &tome_event_info::amount));
    CHECK(tome::has_field(&info, &tome_event_info::y));
}

TEST(tome_hpp_dead_callbacks_are_freed) {
    tome_api api = table(sizeof(tome_api));
    CHECK_EQ(tome::detail::load(&api, kMod, [](tome::Mod) { return true; }), 0);
    tome::detail::enable(kMod, nullptr);
    tome::Mod mod = tome::mod();
    int drops = 0;
    // Refused: never registered, freed at once.
    CHECK_EQ(mod.call(&tome_api::register_command, "taken", [c = Counted(&drops)](const char*) { (void)c; }),
             TOME_ERR_NOT_OWNER);
    CHECK_EQ(drops, 1);
    // The same button or key again replaces the callback; another one doesn't.
    auto* button = reinterpret_cast<tome_frame*>(0x10);
    auto* other = reinterpret_cast<tome_frame*>(0x20);
    mod.call(&tome_api::frame_on_click, button, [c = Counted(&drops)](tome_frame*) { (void)c; });
    mod.call(&tome_api::frame_on_click, other, [c = Counted(&drops)](tome_frame*) { (void)c; });
    mod.call(&tome_api::frame_on_click, button, [c = Counted(&drops)](tome_frame*) { (void)c; });
    CHECK_EQ(drops, 2);
    mod.call(&tome_api::on_keybind, std::string("greet"), [c = Counted(&drops)](const char*) { (void)c; });
    mod.call(&tome_api::on_keybind, "greet", [c = Counted(&drops)](const char*) { (void)c; });
    CHECK_EQ(drops, 3);
    // A popup's callback runs once.
    int choices = 0;
    tome_popup popup = tome::sized<tome_popup>();
    mod.call(&tome_api::popup, &popup, [&choices, c = Counted(&drops)](uint32_t, int32_t) { ++choices; }, nullptr);
    popup_fn(kMod, 1, 0, popup_user);
    CHECK_EQ(choices, 1);
    CHECK_EQ(drops, 4);
    // A callback that replaces itself stays alive until it returns.
    int inside = -1;
    mod.call(&tome_api::frame_on_click, button, [&, mod, c = Counted(&drops)](tome_frame* b) {
        (void)c;
        mod.call(&tome_api::frame_on_click, b, [](tome_frame*) {});
        inside = drops;
    });
    CHECK_EQ(drops, 5);  // the one before it
    click_fn(kMod, button, click_user);
    CHECK_EQ(inside, 5);
    CHECK_EQ(drops, 6);
    tome::detail::disable(kMod, nullptr);
    tome::detail::unload(kMod, nullptr);
    CHECK_EQ(drops, 8);  // the other button's and the key's
}
