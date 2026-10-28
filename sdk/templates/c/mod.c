/*
 * A Tome mod in C: a JASS native, a game event and a chat command. The API
 * and what each function does: tome.h. More of it in use: sdk/examples/hello.
 */
#include <tome/tome.h>

static const tome_api* api;

/* native MyCModAdd takes integer a, integer b returns integer */
static int32_t my_add(tome_jass_integer a, tome_jass_integer b) {
    return a + b;
}

static void TOME_CALL on_game_start(tome_mod* mod, const tome_event_info* info, void* user) {
    (void)user;
    tome_logf(api, mod, TOME_LOG_INFO, "game started on %s", info->map_name);
}

/* /mycmod <text> in the in-game chat. */
static void TOME_CALL on_command(tome_mod* mod, const char* args, void* user) {
    (void)mod;
    (void)user;
    api->display_text(*args ? args : "Hello from my-c-mod", 5.0f);
}

TOME_EXPORT int TOME_CALL tome_mod_load(const tome_api* tome, tome_mod* mod) {
    api = tome;
    if (!tome_has_events(api)) return 1; /* needs API version 5 */
    tome_logf(api, mod, TOME_LOG_INFO, "loaded (Tome %s)", api->tome_version);
    return 0;
}

/* Register everything here: Tome revokes it when the mod is disabled. */
TOME_EXPORT void TOME_CALL tome_mod_enable(tome_mod* mod) {
    api->register_native(mod, "MyCModAdd", (void*)my_add);
    api->on_event(mod, TOME_EVENT_GAME_START, on_game_start, NULL);
    api->register_command(mod, "mycmod", on_command, NULL);
}
