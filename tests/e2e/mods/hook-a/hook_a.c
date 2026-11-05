/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * e2e fixture: hooks RCString::GetText before and after the game's function,
 * at the default priority. Before, it swaps the probe's string for its own,
 * so the game's function reads hook-a's text.
 *
 * Also checks, at game start, that Tome refuses a code value the map script
 * never passed to a native: one from an earlier map would name another
 * function.
 */
#include "../hook_probe.h"

static const tome_api* api;
static probe_rep own_rep = {{0}, "TomeHookProbe from hook-a"};
static probe_string own_string = {{0}, &own_rep};

static void TOME_CALL before(tome_mod* mod, tome_hook_call* call, void* user) {
    (void)user;
    if (!probe_text(call)) return;
    api->log(mod, TOME_LOG_INFO, "hook: a-before");
    call->args[0] = (uint32_t)(uintptr_t)&own_string;
}

static void TOME_CALL after(tome_mod* mod, tome_hook_call* call, void* user) {
    (void)user;
    if (!probe_text(call)) return;
    api->log(mod, TOME_LOG_INFO, "hook: a-after");
}

static void TOME_CALL on_game_start(tome_mod* mod, const tome_event_info* info, void* user) {
    int32_t result = -1;
    tome_result status;
    (void)info;
    (void)user;
    if (!tome_has_jass_values(api)) return;
    status = api->jass_code_call(0x7FFFFFF0u, &result);
    tome_logf(api, mod, TOME_LOG_INFO, "unseen code value: status %d, result %d", (int)status, (int)result);
}

TOME_EXPORT int TOME_CALL tome_mod_load(const tome_api* tome, tome_mod* mod) {
    (void)mod;
    api = tome;
    return tome_has_hooks(api) ? 0 : 1;
}

TOME_EXPORT void TOME_CALL tome_mod_enable(tome_mod* mod) {
    if (api->hook(mod, "RCString::GetText", TOME_HOOK_BEFORE, 0, before, NULL) == TOME_OK &&
        api->hook(mod, "RCString::GetText", TOME_HOOK_AFTER, 0, after, NULL) == TOME_OK) {
        api->log(mod, TOME_LOG_INFO, "hooked RCString::GetText");
    }
    if (tome_has_events(api)) api->on_event(mod, TOME_EVENT_GAME_START, on_game_start, NULL);
}
