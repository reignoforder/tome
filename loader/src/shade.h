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

// Makes the function at `target` run `push value; jmp dest` instead. Returns
// the address that runs its original code (the trampoline), 0 if `target`
// isn't a function Shade can hook.
uintptr_t hook_function(uintptr_t target, uintptr_t dest, uintptr_t value);

// Runs `push value; jmp dest` from an address of its own (what an import
// slot or a hook's entry points at), 0 on failure.
uintptr_t make_stub(uintptr_t dest, uintptr_t value);

// Makes the call instruction (E8 rel32) at `site` call `dest` instead.
bool hook_call(uintptr_t site, uintptr_t dest);

} // namespace tome::shade
