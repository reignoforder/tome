/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * A WebAssembly mod for tests/host/test_wasm.cpp: it exercises the C runtime's
 * callbacks (sdk/support/tome_wasm.c) that no example uses: lobby messages,
 * the host's Start (one callback allows, one refuses with a reason) and typed
 * hooks on functions the test's own mapping file puts in the fake game.
 * Every line it logs is tagged [c-probe] in tome.log.
 */
#include <tome/tome_wasm.h>

static void log_line(const char* text) {
    tome_log(TOME_LOG_INFO, text);
}

/* A lobby message, as one line: who sent it, whether the host did, its data. */
static void on_message(int32_t player, int32_t from_host, const void* data, uint32_t size, void* user) {
    char line[160] = "lobby message from player ";
    char text[64];
    uint32_t n = size < sizeof(text) - 1 ? size : (uint32_t)sizeof(text) - 1;
    (void)user;
    for (uint32_t i = 0; i < n; ++i) text[i] = ((const char*)data)[i];
    text[n] = 0;
    tome_append_int(line, sizeof(line), player);
    tome_append(line, sizeof(line), from_host ? " (host): " : ": ");
    tome_append(line, sizeof(line), text);
    log_line(line);
}

static int32_t start_allow(char* reason, void* user) {
    (void)reason;
    (void)user;
    return TOME_START_ALLOW;
}

static int32_t start_refuse(char* reason, void* user) {
    (void)user;
    tome_append(reason, TOME_LOBBY_REASON_SIZE, "c-probe is not ready");
    return TOME_START_REFUSE;
}

/* A hook's block: [flags][result][argument 0]... as words. */
static void make_x_ten(void* block, void* user) {
    union { float f; uint32_t u; } ten;
    (void)user;
    ten.f = 10.0f;
    ((uint32_t*)block)[2] = ten.u;
}

static void skip_with_99(void* block, void* user) {
    (void)user;
    ((uint32_t*)block)[0] |= TOME_WASM_HOOK_SKIP;
    ((uint32_t*)block)[1] = 99;
}

static void add_1000(void* block, void* user) {
    (void)user;
    ((uint32_t*)block)[1] += 1000;
}

static void set_a_100(void* block, void* user) {
    (void)user;
    ((uint32_t*)block)[2] = 100;  /* an int argument it may only read: refused */
}

int tome_mod_load(void) {
    return 0;
}

void tome_mod_enable(void) {
    char line[96] = "lobby outside a game: players ";
    tome_on_lobby_message(on_message, 0);
    tome_on_lobby_start(start_allow, 0);
    tome_on_lobby_start(start_refuse, 0);
    tome_append_int(line, sizeof(line), tome_lobby_players(0, 0));
    tome_append(line, sizeof(line), ", send ");
    tome_append_int(line, sizeof(line), tome_lobby_send(TOME_LOBBY_EVERYONE, "x", 1));
    log_line(line);
    tome_hook("c-probe/Scale", TOME_HOOK_BEFORE, 0, make_x_ten, 0);
    tome_hook("c-probe/Skip", TOME_HOOK_BEFORE, 0, skip_with_99, 0);
    tome_hook("c-probe/Twice", TOME_HOOK_AFTER, 0, add_1000, 0);
    tome_hook("c-probe/Plus", TOME_HOOK_BEFORE, 0, set_a_100, 0);
    log_line("enabled");
}

void tome_mod_disable(void) {
    log_line("disabled");
}
