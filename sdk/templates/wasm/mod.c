/*
 * A Tome mod in C, as WebAssembly: a JASS native, a game event and a chat
 * command. What each function does: include/tome/tome_wasm.h (and tome.h).
 * More of it in use: sdk/examples/hello-wasm.
 */
#include <tome/tome_wasm.h>

/* native MyWasmModAdd takes integer a, integer b returns integer */
static uint32_t my_add(const uint32_t* args, void* user) {
    (void)user;
    return (uint32_t)((int32_t)args[0] + (int32_t)args[1]);
}

static void on_game_start(const tome_wasm_event* info, void* user) {
    (void)user;
    char line[128] = "game started on ";
    tome_append(line, sizeof(line), info->map_name);
    tome_log(TOME_LOG_INFO, line);
}

/* /mywasmmod <text> in the in-game chat. */
static void on_command(const char* args, void* user) {
    (void)user;
    tome_display_text(*args ? args : "Hello from my-wasm-mod", 5.0f);
}

int tome_mod_load(void) {
    tome_log(TOME_LOG_INFO, "loaded");
    return 0;
}

void tome_mod_enable(void) {
    tome_register_native("MyWasmModAdd", my_add, 0);
    tome_on_event(TOME_EVENT_GAME_START, on_game_start, 0);
    tome_register_command("mywasmmod", on_command, 0);
}
