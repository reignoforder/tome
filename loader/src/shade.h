// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shade (a static recompiler that runs the game on other platforms) runs
// Tome as recompiled code. The game's code is C there, not the bytes in
// memory: writing a jmp or a call's target into it changes nothing. Shade's
// own natives ("shade.dll", which exists only there) do what those writes do.

#include <cstdint>

namespace tome::shade {

// True when running under Shade (checked once).
bool active();

// Whether this Shade can hook functions: it has ShadeOriginal (version 2).
bool can_hook_functions();

// The address that runs `target`'s original code (the trampoline), before
// it's hooked: the same one stays valid while and after it is. 0 if
// `target` isn't a function Shade can hook (or !can_hook_functions()).
// Stored before hook_function, so no call through the hook finds it missing.
uintptr_t original(uintptr_t target);

// Makes the function at `target` run `push value; jmp dest` instead (on
// before this returns). False if `target` isn't a function Shade can hook.
bool hook_function(uintptr_t target, uintptr_t dest, uintptr_t value);

// Undoes hook_function: `target` runs its own code again (the trampoline
// it returned stays valid, for calls already on their way). False if it
// can't (a Shade without ShadeUnhook): the hook then stays on.
bool unhook(uintptr_t target);

// Runs `push value; jmp dest` from an address of its own (what an import
// slot or a hook's entry points at), 0 on failure.
uintptr_t make_stub(uintptr_t dest, uintptr_t value);

// Makes the call instruction (E8 rel32) at `site` call `dest` instead.
bool hook_call(uintptr_t site, uintptr_t dest);

} // namespace tome::shade
