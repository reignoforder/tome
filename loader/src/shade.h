// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shade (a static recompiler that runs the game on other platforms) runs
// Tome as recompiled code. The game's code is C there, not the bytes in
// memory: writing a jmp or a call's target into it changes nothing. Shade's
// own natives ("shade.dll", which exists only there) do what those writes do.

#include <cstdint>
#include <string>

namespace tome::shade {

// True when running under Shade (checked once).
bool active();

// Whether this Shade can hook functions: it has ShadeOriginal (version 2).
bool can_hook_functions();

// Whether this Shade loads mods' DLLs (version 4: 3 loaded them but had no
// ShadeModCodeHash, so approval couldn't cover what runs): their code is
// built apart from the game's (shade-recomp build-mod), and a DLL without
// it fails to load (LoadLibrary), Shade's log saying how to make it.
bool can_load_dlls();

// The SHA-256 (64 hex digits) of the library Shade runs for the mod DLL at
// `dll` (what runs there, not the DLL), "" if there's none (or
// !can_load_dlls()).
std::string mod_code_hash(const std::wstring& dll);

// What a failed LoadLibrary of a mod's DLL means under Shade, which fails
// a DLL whose code isn't built like a missing file (126) and one whose
// built code it can't use (another interface, its address taken) with 1114:
// neither says why on its own. "" for other errors, or if !active().
const char* load_error_hint(unsigned long error);

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
