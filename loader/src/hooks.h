#pragma once

// Inline hooks on mapped game functions, shared by every mod (tome.h,
// "Hooks"). Each hooked function gets one patch: its first instructions
// (at least 5 bytes, checked against the mapping's "bytes") move to a
// trampoline and a `jmp` to a per-target stub takes their place. The stub
// hands the registers and stack to a dispatcher, which runs the function's
// chain of handlers (core/hooks.h) and calls the trampoline for the
// original. Imports (calls game.dll makes to Windows DLLs through its
// import address table) get their slot pointed at the stub instead. The
// patch stays for the session; an empty chain just calls the original.

#include "core/hooks.h"
#include "core/mappings.h"

#include <cstdint>
#include <string>

namespace tome::hooks {

struct Target;

// Remembers the running build's mapping database. Call before any hook.
void init(uintptr_t game_base, const mappings::Build* build);

// The target for a mapped function or import, patching it on first use.
// Null (with `error`) if the name isn't one or can't be hooked.
Target* find_or_install(const std::string& name, std::string& error);

// The target if `name` is already hooked, otherwise null.
Target* find(const std::string& name);

// Reserves a mapped function for Tome's own built-in use (a patch): the
// mod-facing hook/unhook API refuses it, so a mod can't add a handler that
// interferes with the patch's chain (e.g. a REPLACE that suppresses a token)
// or try to drop it. Tome's internal add/remove (which take a Target*) are
// unaffected. Called once at startup, before any mod loads. Note this is an
// API-level boundary only: a mod is native code and can still write process
// memory directly, which Tome can't prevent for in-process plugins.
void reserve(const std::string& name);
bool is_reserved(const std::string& name);

// Patches the function at `address`, whose first bytes must match `bytes`
// (-1 matches anything). find_or_install uses it; tests call it directly.
Target* install(const std::string& name, uintptr_t address, mappings::Convention convention,
                const std::string& signature, const std::vector<int16_t>& bytes, std::string& error);

// Points the import address table slot at `slot` to a stub of its own, so
// every call through it runs the chain; the original is what the slot held.
// find_or_install uses it for "import" symbols; tests call it directly.
Target* install_import(const std::string& name, uintptr_t slot, mappings::Convention convention,
                       const std::string& signature, std::string& error);

// Adds to or removes from a target's chain. `seq` is filled in by add.
// remove revokes the handlers' gates first, so calls that already hold the
// old chain don't start them either.
// `removed_gates`, if given, receives the removed handlers' gates.
void add(Target* target, Handler handler);
size_t remove(Target* target, tome_mod* mod, int32_t phase, tome_hook_fn fn, void* user,
              std::vector<const void*>* removed_gates = nullptr);
// Removes the one handler registered with `gate`.
bool remove_gate(Target* target, const Gate* gate);

const char* name(const Target* target);

// Calls a function that takes `count` 32-bit stack arguments (cdecl or
// stdcall) and returns eax.
uint32_t call_with_stack(const void* fn, const uint32_t* args, uint32_t count);

// True once no handler behind `gates` is running: waits up to `wait_ms` for
// other threads to leave them. False if one is still running elsewhere, or
// at once if one is on this thread's own stack (the caller is inside it).
// Before freeing a mod's DLL, revoke its handlers, then check this.
bool idle(const std::vector<std::shared_ptr<Gate>>& gates, unsigned wait_ms);

} // namespace tome::hooks
