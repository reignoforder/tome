#pragma once

// Custom JASS natives (docs/research.md §3.3, §5.5).
//
// Every native declared by an installed mod gets a permanent slot and a small
// machine-code thunk. The thunk is what the engine binds; it forwards to the
// mod's implementation, or returns 0 (null/false/0.0) while none is attached.
// So a map that declares a mod's natives still loads when that mod is
// disabled, and toggling a mod never invalidates a compiled map script.

#include "mappings.h"

#include <tome/tome.h>

#include <cstdint>
#include <string>
#include <vector>

namespace tome::natives {

// Redirects the engine's InitNatives call sites so Tome's natives get bound
// right after the built-in ones. Safe to call from DllMain.
bool install_hooks(uintptr_t game_base, const BuildMappings& build, std::string& report);

// Signals that the initial mod load finished. Until then (at most 10 s) the
// InitNatives hook waits, so a map that loads immediately (-loadfile) never
// sees natives whose mod hasn't been enabled yet. Idempotent.
void set_mods_ready();

// How many maps have started loading (InitNatives calls after startup; the
// game's startup call runs before Tome is loaded). Main thread.
unsigned map_load_count();

// Declares a native for `owner` (a mod id, or "tome"). Re-declaring with the
// same owner is fine; a different owner is a conflict. Returns false with
// `error` set on a bad declaration or conflict.
bool declare(const std::string& owner, const std::string& declaration, std::string& error);

// Attaches/detaches an implementation. The native must already be declared
// by `owner`. A mod's natives (`mod` set) run under a crash guard.
enum class AttachResult { Ok, NotDeclared, NotOwner };
AttachResult attach(const std::string& owner, const std::string& name, void* fn, tome_mod* mod = nullptr);
void detach(const std::string& owner, const std::string& name);

struct NativeInfo {
    std::string name;
    std::string signature;
    std::string owner;
    bool attached;
    unsigned bind_count;  // how many times the engine has bound it (once per InitNatives)
};
std::vector<NativeInfo> list();

// The text of a JASS string argument (an RCString*), or NULL. Any thread
// that's inside the native.
const char* string_text(const void* jass_string);

// Fills the version 6 part of the API table: strings and code. Each one
// fails (or returns null) until install_hooks has run.
void fill_api(tome_api& api);

} // namespace tome::natives
