/*
 * e2e fixture: hooks RCString::GetText in all three phases at priority 10
 * (so its BEFORE and AFTER run ahead of hook-a's), then calls GetText on a
 * probe string on every enable and logs what came back.
 */
#include "../hook_probe.h"

typedef const char*(__attribute__((thiscall)) * get_text_fn)(const probe_string* self);

static const tome_api* api;
static get_text_fn get_text;
static probe_rep probe_rep_ = {{0}, "TomeHookProbe"};
static probe_string probe = {{0}, &probe_rep_};

static void TOME_CALL before(tome_mod* mod, tome_hook_call* call, void* user) {
    (void)user;
    if (probe_text(call)) api->log(mod, TOME_LOG_INFO, "hook: b-before");
}

static void TOME_CALL replace(tome_mod* mod, tome_hook_call* call, void* user) {
    (void)user;
    if (!probe_text(call)) {
        api->hook_call_next(call);
        return;
    }
    api->log(mod, TOME_LOG_INFO, "hook: b-replace");
    api->hook_call_next(call);
    tome_logf(api, mod, TOME_LOG_INFO, "hook: b-next-returned \"%s\"", (const char*)(uintptr_t)call->result);
}

static void TOME_CALL after(tome_mod* mod, tome_hook_call* call, void* user) {
    (void)user;
    if (probe_text(call)) api->log(mod, TOME_LOG_INFO, "hook: b-after");
}

TOME_EXPORT int TOME_CALL tome_mod_load(const tome_api* tome, tome_mod* mod) {
    (void)mod;
    api = tome;
    get_text = (get_text_fn)tome_resolve(api, "RCString::GetText");
    return tome_has_hooks(api) && get_text ? 0 : 1;
}

TOME_EXPORT void TOME_CALL tome_mod_enable(tome_mod* mod) {
    const char* text;
    if (api->hook(mod, "RCString::GetText", TOME_HOOK_BEFORE, 10, before, NULL) != TOME_OK ||
        api->hook(mod, "RCString::GetText", TOME_HOOK_REPLACE, 10, replace, NULL) != TOME_OK ||
        api->hook(mod, "RCString::GetText", TOME_HOOK_AFTER, 10, after, NULL) != TOME_OK) {
        return;
    }
    api->log(mod, TOME_LOG_INFO, "probe start");
    text = get_text(&probe);
    tome_logf(api, mod, TOME_LOG_INFO, "probe returned \"%s\"", text ? text : "(null)");
}
