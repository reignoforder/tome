// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Shared by the hook-a and hook-b e2e fixtures: a fake engine string that
 * RCString::GetText reads like a real one ([[this + 8] + 0x1C]), and a
 * check that picks the probe's calls out of the game's own.
 */
#ifndef TOME_E2E_HOOK_PROBE_H
#define TOME_E2E_HOOK_PROBE_H

#include <tome/tome.h>

#include <string.h>

typedef struct probe_rep {
    char pad[0x1C];
    const char* text;
} probe_rep;

typedef struct probe_string {
    char pad[8];
    probe_rep* rep;
} probe_string;

/* The text of the string a GetText call reads, if it's a probe's. */
static inline const char* probe_text(const tome_hook_call* call) {
    const probe_string* s = (const probe_string*)(uintptr_t)call->args[0];
    if (!s || !s->rep || !s->rep->text) return NULL;
    return strncmp(s->rep->text, "TomeHookProbe", 13) == 0 ? s->rep->text : NULL;
}

#endif
