/*
 * A Tome mod in C++: a JASS native, a game event and a chat command.
 * tome.hpp's Mod::call reaches every function in tome.h, with lambdas for
 * callbacks. More of it in use: sdk/examples/hello-cpp.
 */
#include <tome/tome.hpp>

#include <string>

namespace {

/* native MyCppModAdd takes integer a, integer b returns integer */
int32_t my_add(tome_jass_integer a, tome_jass_integer b) {
    return a + b;
}

bool load(tome::Mod mod) {
    mod.log(TOME_LOG_INFO, "loaded (Tome %s)", mod.api()->tome_version);
    return true;
}

// Register everything here: Tome revokes it when the mod is disabled.
void enable(tome::Mod mod) {
    mod.call(&tome_api::register_native, "MyCppModAdd", tome::native<my_add>);
    mod.call(&tome_api::on_event, TOME_EVENT_GAME_START, [mod](const tome_event_info* info) {
        mod.log(TOME_LOG_INFO, "game started on %s", info->map_name);
    });
    // /mycppmod <text> in the in-game chat.
    mod.call(&tome_api::register_command, "mycppmod", [mod](const char* args) {
        std::string text = *args ? args : "Hello from my-cpp-mod";
        mod.call(&tome_api::display_text, text, 5.0f);
    });
}

}  // namespace

TOME_EXPORT_MOD(load, enable, nullptr, nullptr)
