// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Built-in patches: Tome's own fixes for game bugs and vulnerabilities,
// compiled into tome.mix and applied once at startup. Unlike mods they can't
// be listed, toggled or removed, so a fix for something like the JASS
// "memhack" typecast is always in force.
//
// Each patch names the mapped symbols it needs (and, optionally, the builds
// it's been validated on). The patch manager skips a patch unless the running
// build maps every symbol it needs -- the same mapping database that already
// tells Tome which build is running -- so a patch runs only where it fits, and
// automatically covers any later build whose mapping defines the same symbols.
// See docs/architecture.md, "Patches".

#include "core/mappings.h"

#include <cstdint>
#include <string>

namespace tome::patches {

struct Patch {
    const char* name;
    const char* summary;
    // NULL-terminated allowlist of build names (mappings/<build>.json "name"),
    // or null to run on any build that maps every symbol in `symbols`.
    const char* const* builds;
    // NULL-terminated mapped symbol names the patch needs. The patch is
    // skipped unless the running build maps every one (verified against the
    // loaded game.dll at startup, and again when each is hooked).
    const char* const* symbols;
    // NULL-terminated mapped functions to reserve once the patch is applied:
    // the mod-facing hook/unhook API then refuses them, so a mod can't add a
    // handler that interferes with the patch or try to drop it (hooks::reserve).
    // Usually the functions the patch hooks; null to reserve nothing.
    const char* const* reserve;
    // Whether the patch is needed where the game runs (e.g. only under
    // Wine), checked before `install`; null if always. Returns false, with
    // the reason in `report`, to skip it: counted as skipped, not applied.
    // only_under_wine for the Wine-only fixes.
    bool (*needed)(std::string& report);
    // Applies the patch; `base` is game.dll's load address. Returns false,
    // with a reason in `report`, if it couldn't be applied -- the game then
    // runs with this one patch absent, the others still in force.
    bool (*install)(uintptr_t base, const BuildMappings& build, std::string& report);
};

// Applies every registered patch that fits the running build. Call after the
// build is identified and verified. Always returns true: a patch that doesn't
// fit or fails is logged and skipped. `report` summarizes what was applied.
bool install(uintptr_t base, const BuildMappings& build, std::string& report);

// `needed` for a fix of a Wine bug: true under Wine, else false with
// "not needed outside Wine".
bool only_under_wine(std::string& report);

// background-mute, once the game window is subclassed, on the main thread:
// undoes a mute a crashed or quit run left in the system mixer, and keeps
// the flag that tells it so in `tome_dir` from now on.
void background_mute_start(const std::wstring& tome_dir);

} // namespace tome::patches
