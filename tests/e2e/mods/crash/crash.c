/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * e2e fixture: writes to address 0 on purpose, in the place the first word
 * of mode.txt (in the mod's folder, read on every load and enable) names:
 *   load    in tome_mod_load
 *   enable  in tome_mod_enable
 *   native  in CrashNative (otherwise it returns its argument + 1)
 *   tick    in its tick handler, on the 30th tick
 *   hook    in a REPLACE handler of RCString::GetText, on its own probe
 *           string, before calling next; tome_mod_enable logs what the call
 *           returned
 *   thread  on a thread of its own, started in tome_mod_enable: a crash
 *           outside every call Tome guards (crash_handler.h reports it)
 */
#include "../hook_probe.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>

typedef const char*(__attribute__((thiscall)) * get_text_fn)(const probe_string* self);

static const tome_api* api;
static char mode[16];
static int ticks;
static get_text_fn get_text;
static probe_rep probe_rep_ = {{0}, "TomeCrashProbe"};
static probe_string probe = {{0}, &probe_rep_};

/* A null pointer read at run time: a write through a constant null is
   undefined, and GCC drops it below -O2 (above, it puts a trap there). */
static volatile int* volatile null_pointer;

static void crash(void) {
    *null_pointer = 1;
}

static DWORD WINAPI crash_thread(LPVOID unused) {
    (void)unused;
    crash();
    return 0;
}

static void read_mode(tome_mod* mod) {
    char path[512];
    FILE* file;
    snprintf(path, sizeof(path), "%s\\mode.txt", api->mod_dir(mod));
    mode[0] = '\0';
    file = fopen(path, "r");
    if (file) {
        if (fscanf(file, "%15s", mode) != 1) mode[0] = '\0';
        fclose(file);
    }
}

static tome_jass_integer CrashNative(tome_jass_integer value) {
    if (strcmp(mode, "native") == 0) crash();
    return value + 1;
}

static void TOME_CALL on_tick(tome_mod* mod, const tome_event_info* info, void* user) {
    (void)mod;
    (void)info;
    (void)user;
    if (++ticks == 30) crash();
}

static void TOME_CALL replace(tome_mod* mod, tome_hook_call* call, void* user) {
    const probe_string* s = (const probe_string*)(uintptr_t)call->args[0];
    (void)mod;
    (void)user;
    if (s == &probe) crash();
    api->hook_call_next(call);
}

TOME_EXPORT int TOME_CALL tome_mod_load(const tome_api* tome, tome_mod* mod) {
    api = tome;
    read_mode(mod);
    tome_logf(api, mod, TOME_LOG_INFO, "mode \"%s\"", mode);
    if (strcmp(mode, "load") == 0) crash();
    get_text = (get_text_fn)tome_resolve(api, "RCString::GetText");
    return tome_has_hooks(api) && get_text ? 0 : 1;
}

TOME_EXPORT void TOME_CALL tome_mod_enable(tome_mod* mod) {
    read_mode(mod);
    tome_logf(api, mod, TOME_LOG_INFO, "mode \"%s\"", mode);
    api->register_native(mod, "CrashNative", (void*)CrashNative);
    if (strcmp(mode, "enable") == 0) crash();
    if (strcmp(mode, "tick") == 0) {
        ticks = 0;
        api->on_event(mod, TOME_EVENT_TICK, on_tick, NULL);
    }
    if (strcmp(mode, "thread") == 0) {
        HANDLE thread = CreateThread(NULL, 0, crash_thread, NULL, 0, NULL);
        if (thread) CloseHandle(thread);
    }
    if (strcmp(mode, "hook") == 0) {
        const char* text;
        api->hook(mod, "RCString::GetText", TOME_HOOK_REPLACE, 0, replace, NULL);
        text = get_text(&probe);
        tome_logf(api, mod, TOME_LOG_INFO, "probe returned \"%s\"", text ? text : "(null)");
    }
}

TOME_EXPORT void TOME_CALL tome_mod_disable(tome_mod* mod) {
    api->log(mod, TOME_LOG_INFO, "disabled");
}

TOME_EXPORT void TOME_CALL tome_mod_unload(tome_mod* mod) {
    api->log(mod, TOME_LOG_INFO, "unloading");
}
