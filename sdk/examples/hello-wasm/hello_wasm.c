/* SPDX-License-Identifier: MIT */
/*
 * The example mod as a WebAssembly module (include/tome/tome_wasm.h): it
 * runs sandboxed, so it can only do what Tome's imports let it, and Tome
 * doesn't ask players to trust it with their computer. Every line it logs
 * is tagged [hello-wasm] in tome/tome.log.
 */
#include <tome/tome_wasm.h>

static uint32_t g_ticks = 0;

static void log_line(const char* a, const char* b) {
    char line[256] = "";
    tome_append(line, sizeof(line), a);
    tome_append(line, sizeof(line), b);
    tome_log(TOME_LOG_INFO, line);
}

/* /hellowasm <name>: the greeting setting, `repeat` times. */
static void on_command(const char* args, void* user) {
    (void)user;
    char line[160] = "";
    tome_setting_string("greeting", line, 64);
    tome_append(line, sizeof(line), ", ");
    tome_append(line, sizeof(line), args[0] ? args : "world");
    tome_append(line, sizeof(line), ".");
    const int32_t repeat = tome_setting_int("repeat", 1);
    for (int32_t i = 0; i < repeat; ++i) log_line("/hellowasm command: ", line);
    tome_display_text(line, 5.0f);
    /* Other mods listening on the topic hear it (not this one). */
    const int32_t reached = tome_publish("hello-wasm.greeted", line, (uint32_t)tome_strlen(line));
    char note[64] = "published to ";
    tome_append_int(note, sizeof(note), reached);
    tome_append(note, sizeof(note), reached == 1 ? " mod" : " mods");
    tome_log(TOME_LOG_INFO, note);
}

static void on_setting(const char* key, void* user) {
    (void)user;
    log_line("setting changed: ", key);
}

static void on_key(const char* id, void* user) {
    (void)user;
    char key[32];
    tome_keybind_name(id, key, sizeof(key));
    log_line("key pressed: ", key);
}

static void on_event(const tome_wasm_event* info, void* user) {
    (void)user;
    switch (info->event) {
    case TOME_EVENT_MAP_LOAD: log_line("map load: ", info->map_path); break;
    case TOME_EVENT_GAME_START: log_line("game start: ", info->map_name); break;
    case TOME_EVENT_GAME_END: log_line("game end: ", info->map_name); break;
    case TOME_EVENT_TICK:
        if (++g_ticks == 100) tome_log(TOME_LOG_INFO, "100 ticks");
        break;
    }
}

static void on_main_menu(int32_t ui_event, void* user) {
    (void)ui_event;
    (void)user;
    /* Frames are handles: looked up by name each time they're used. */
    const tome_frame_t button = tome_frame_get("SinglePlayerButton", 0);
    log_line("main menu: SinglePlayerButton ", button ? "found" : "missing");
}

/* --- natives (tome.mod.json "natives") --- */

static uint32_t native_add(const uint32_t* args, void* user) {
    (void)user;
    return (uint32_t)((int32_t)args[0] + (int32_t)args[1]);
}

static uint32_t native_half(const uint32_t* args, void* user) {
    (void)user;
    return tome_real_bits(tome_real(args[0]) / 2.0f);
}

static char g_greeting[128];

static uint32_t native_greet(const uint32_t* args, void* user) {
    (void)user;
    const char* name = tome_string(args[0]);
    g_greeting[0] = 0;
    tome_append(g_greeting, sizeof(g_greeting), "Hello, ");
    tome_append(g_greeting, sizeof(g_greeting), name ? name : "nobody");
    return tome_string_result(g_greeting);
}

/* Runs a function of the map script, which may call this mod's natives
 * again (a nested call into the module: the runtime keeps this call's
 * arguments intact meanwhile). */
static uint32_t native_call(const uint32_t* args, void* user) {
    (void)user;
    const uint32_t code = args[0];
    int32_t result = 0;
    const int32_t status = tome_jass_code_call(code, &result);
    char line[96] = "HelloWasmCall: callback returned ";
    tome_append_int(line, sizeof(line), result);
    tome_append(line, sizeof(line), args[0] == code ? ", its argument intact" : ", its argument CHANGED");
    tome_append(line, sizeof(line), status == TOME_OK ? "" : " (failed)");
    tome_log(TOME_LOG_INFO, line);
    return (uint32_t)result;
}

/* A handle result must be one of the call's own arguments of that type. */
static uint32_t native_same(const uint32_t* args, void* user) {
    (void)user;
    return args[0];
}

int tome_mod_load(void) {
    const tome_wasm_game_info* game = tome_game_info();
    log_line("loaded, Warcraft III ", game->build_name);
    return 0;
}

void tome_mod_enable(void) {
    tome_register_command("hellowasm", on_command, 0);
    tome_on_setting_changed(on_setting, 0);
    tome_on_keybind("greet", on_key, 0);
    tome_on_event(TOME_EVENT_MAP_LOAD, on_event, 0);
    tome_on_event(TOME_EVENT_GAME_START, on_event, 0);
    tome_on_event(TOME_EVENT_GAME_END, on_event, 0);
    tome_on_event(TOME_EVENT_TICK, on_event, 0);
    tome_on_ui(TOME_UI_MAIN_MENU, on_main_menu, 0);
    tome_register_native("HelloWasmAdd", native_add, 0);
    tome_register_native("HelloWasmHalf", native_half, 0);
    tome_register_native("HelloWasmGreet", native_greet, 0);
    tome_register_native("HelloWasmSame", native_same, 0);
    tome_register_native("HelloWasmCall", native_call, 0);
    tome_log(TOME_LOG_INFO, "enabled");
}

void tome_mod_disable(void) {
    tome_log(TOME_LOG_INFO, "disabled");
}
