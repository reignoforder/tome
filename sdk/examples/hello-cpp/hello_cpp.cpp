/*
 * The hello example in C++, with tome/tome.hpp: logs each lifecycle step,
 * looks up a mapped game function, provides JASS natives (numbers, strings
 * and a callback), adds a "Hello (C++)" button to the main menu (from its
 * own ui/hello_cpp.fdf), logs game events, hooks the game's
 * CTextFrame::SetText, adds a /hellocpp chat command, and has settings
 * (config.schema.json), a keybind (Greet, Ctrl+Shift+J) and a popup.
 *
 * Every call goes through Mod::call, so none needs a wrapper in the SDK.
 * `/hellocpp throw` throws on purpose: Tome logs it and disables the mod, as
 * for a crash.
 */
#include <tome/tome.hpp>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

/* native HelloCppLog takes integer value returns nothing */
int32_t hello_log(tome_jass_integer value) {
    tome::mod().log(TOME_LOG_INFO, "HelloCppLog(%d) called from JASS", (int)value);
    return 0;
}

/* native HelloCppDouble takes real value returns real */
tome_jass_real_ret hello_double(tome_jass_real_arg value) {
    return tome_jass_real_bits(*value * 2.0f);
}

/* native HelloCppGreet takes string name returns string */
tome_jass_string_ret hello_greet(tome_jass_string name) {
    tome::Mod mod = tome::mod();
    const char* who = mod.call(&tome_api::jass_string, name);
    if (!who || !*who) who = "world";
    // Letters, not bytes: JASS's StringLength would count "Ζεύς" as 8.
    std::string text = std::string("Hello from C++, ") + who + "! (" + std::to_string(tome_utf8_length(who)) +
                       " letters)";
    return mod.call(&tome_api::jass_string_create, text);
}

/* native HelloCppCall takes code callback returns boolean */
tome_jass_boolean hello_call(tome_jass_code callback) {
    int32_t result = 0;
    tome_result status = tome::mod().call(&tome_api::jass_code_call, callback, &result);
    tome::mod().log(TOME_LOG_INFO, "HelloCppCall: callback returned %d (status %d)", (int)result, (int)status);
    return result;
}

// Hooks run on whichever thread calls the game's function.
std::atomic<unsigned> set_text_calls{0};

void greet(tome::Mod mod, const std::string& who, const char* source) {
    std::string greeting = mod.text(&tome_api::setting_string, "greeting");
    if (greeting.empty()) greeting = "Hello from C++";
    double seconds = mod.call(&tome_api::setting_number, "seconds", 5.0);
    std::string text = greeting + ", " + (who.empty() ? "world" : who) + ".";
    mod.log(TOME_LOG_INFO, "%s: %s (%.1f s)", source, text.c_str(), seconds);
    mod.call(&tome_api::display_text, text, (float)seconds);
}

void add_button(tome::Mod mod, std::shared_ptr<int> clicks) {
    tome_frame* parent = mod.call(&tome_api::frame_get, "ControlLayer", 0);
    tome_frame* anchor = mod.call(&tome_api::frame_get, "MainMenuFrame", 0);
    if (!parent || !anchor || mod.call(&tome_api::load_fdf, "ui/hello_cpp.fdf") != TOME_OK) return;
    // After a disable and enable on the same screen the hidden button is
    // still there: a name and context can only be created once.
    tome_frame* button = mod.call(&tome_api::frame_get, "HelloCppButton", 0);
    if (button) {
        mod.call(&tome_api::frame_set_visible, button, 1);
    } else {
        button = mod.call(&tome_api::frame_create, "HelloCppButton", parent, 0, 0);
    }
    if (!button) return;
    // Top left, below the Rust example's.
    mod.call(&tome_api::frame_set_point, button, TOME_FRAMEPOINT_TOPLEFT, anchor, TOME_FRAMEPOINT_TOPLEFT, 0.02f,
             -0.06f);
    tome_result clicked = mod.call(&tome_api::frame_on_click, button, [mod, clicks](tome_frame*) {
        ++*clicks;
        mod.log(TOME_LOG_INFO, "Hello (C++) button clicked (%d)", *clicks);
        std::string text = "Clicked " + std::to_string(*clicks);
        mod.call(&tome_api::frame_set_text, mod.call(&tome_api::frame_get, "HelloCppButtonText", 0), text);
    });
    if (clicked == TOME_OK) mod.log(TOME_LOG_INFO, "added a Hello (C++) button to the main menu");
}

bool load(tome::Mod mod) {
    const tome_game_info* game = mod.call(&tome_api::game_info);
    mod.log(TOME_LOG_INFO, "loaded (Tome %s, Warcraft III %s, game.dll at 0x%08lx)", mod.api()->tome_version,
            game->build_name, (unsigned long)game->game_base);
    // Listed in "requires", so Tome only loads this mod where it's mapped.
    const tome_symbol* set_text = mod.call(&tome_api::resolve, "CTextFrame::SetText");
    if (!set_text) return false;
    mod.log(TOME_LOG_INFO, "%s is a %s function at 0x%08lx (%s)", set_text->name, set_text->convention,
            (unsigned long)set_text->value, set_text->signature);
    return true;
}

void enable(tome::Mod mod) {
    mod.call(&tome_api::register_native, "HelloCppLog", tome::native<hello_log>);
    mod.call(&tome_api::register_native, "HelloCppDouble", tome::native<hello_double>);
    mod.call(&tome_api::register_native, "HelloCppGreet", tome::native<hello_greet>);
    mod.call(&tome_api::register_native, "HelloCppCall", tome::native<hello_call>);

    auto clicks = std::make_shared<int>(0);
    mod.call(&tome_api::on_ui, TOME_UI_MAIN_MENU, [mod, clicks](int32_t) { add_button(mod, clicks); });

    for (int32_t event : {TOME_EVENT_MAP_LOAD, TOME_EVENT_GAME_START, TOME_EVENT_GAME_END}) {
        mod.call(&tome_api::on_event, event, [mod](const tome_event_info* info) {
            static const char* const names[] = {"", "map load", "game start", "game end"};
            mod.log(TOME_LOG_INFO, "%s: %s (%s)", names[info->event], info->map_name, info->map_path);
        });
    }
    mod.call(&tome_api::on_event, TOME_EVENT_TICK, [mod, ticks = 0u](const tome_event_info* info) mutable {
        if (++ticks == 100) mod.log(TOME_LOG_INFO, "100 ticks (frame %u)", (unsigned)info->frame);
    });
    mod.call(&tome_api::on_event, TOME_EVENT_UNIT_DEATH, [mod](const tome_event_info* info) {
        char code[5];
        if (!info->unit) return;
        mod.log(TOME_LOG_INFO, "%s (%s) died%s%s", info->unit->name, tome_type_code(info->unit->type_id, code),
                info->other ? ", killed by " : "", info->other ? info->other->name : "");
    });

    set_text_calls = 0;
    tome_result hooked = mod.call(&tome_api::hook, "CTextFrame::SetText", TOME_HOOK_AFTER, 0,
                                  [mod](tome_hook_call* call) {
                                      if (set_text_calls++ != 0) return;
                                      // const char* text, after `this`.
                                      auto text = reinterpret_cast<const char*>(call->args[1]);
                                      mod.log(TOME_LOG_INFO, "first CTextFrame::SetText after enable: \"%s\"",
                                              text ? text : "");
                                  });
    if (hooked != TOME_OK) mod.log(TOME_LOG_WARN, "couldn't hook CTextFrame::SetText (%d)", (int)hooked);

    mod.call(&tome_api::register_command, "hellocpp", [mod](const char* args) {
        if (std::string(args) == "throw") throw std::runtime_error("/hellocpp throw");
        greet(mod, args, "/hellocpp command");
    });
    mod.call(&tome_api::on_setting_changed, [mod](const char* key) {
        mod.log(TOME_LOG_INFO, "setting changed: %s", key);
    });
    mod.call(&tome_api::on_keybind, "greet", [mod](const char*) {
        static const char* const options[] = {"Greet", "Close"};
        tome_popup popup = tome::sized<tome_popup>();
        popup.title = "Hello from C++";
        popup.text = "A popup from the |cffffcc00hello-cpp|r example.";
        popup.options = options;
        popup.option_count = 2;
        popup.default_option = 0;
        popup.cancel_option = 1;
        tome_result shown = mod.call(&tome_api::popup, &popup,
                                     [mod](uint32_t, int32_t choice) {
                                         if (choice == 0) greet(mod, "", "popup");
                                     },
                                     nullptr);
        if (shown != TOME_OK) greet(mod, "", "Greet key");
    });
    mod.log(TOME_LOG_INFO, "enabled");
}

void disable(tome::Mod mod) {
    // Tome revokes the natives, callbacks, hook and command itself and hides
    // the button; the natives return 0 until enabled again.
    mod.log(TOME_LOG_INFO, "disabled");
}

void unload(tome::Mod mod) {
    mod.log(TOME_LOG_INFO, "unloading");
}

}  // namespace

TOME_EXPORT_MOD(load, enable, disable, unload)
