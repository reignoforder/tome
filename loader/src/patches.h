// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Built-in patches: Tome's own fixes for game bugs and vulnerabilities,
// compiled into tome.mix and applied once at startup. Unlike mods they can't
// be listed, toggled or removed, so a fix for something like the JASS
// "memhack" typecast is always in force.
//
// Each patch names the mapped symbols it needs. The patch manager skips a
// patch unless the running build maps every symbol it needs -- the same
// mapping database that already tells Tome which build is running -- so a
// patch runs only where it fits, and automatically covers any later build
// whose mapping defines the same symbols. Where builds differ, a patch lists
// variants: per build, what else it needs and what it does differently, so
// the version-specific parts live with the patch, not in its code paths.
// See docs/architecture.md, "Patches".

#include "core/mappings.h"

#include <cstdint>
#include <span>
#include <string>

namespace tome::patches {

// One way of applying a patch, for some builds.
struct Variant {
    // NULL-terminated build names (mappings/<build>.json "name") it's for,
    // or null: every build no other variant names (the default).
    const char* const* builds;
    // NULL-terminated mapped symbols it needs besides the patch's own
    // `symbols`; null for none.
    const char* const* symbols;
    // Non-null: the patch isn't for these builds, and this says why (e.g.
    // the bug isn't there); the patch is skipped with it.
    const char* skip;
    // What the patch does differently on these builds: its own struct,
    // which its install casts back. Null if nothing.
    const void* params;
    // NULL-terminated mapped functions to reserve besides the patch's own
    // `reserve` (the ones only this variant hooks); null for none.
    const char* const* reserve = nullptr;
};

struct Patch {
    const char* name;
    const char* summary;
    // The patch's variants: the one naming the running build, else the
    // default (builds null). With none at all, one default that adds
    // nothing; without a default, builds no variant names skip the patch.
    std::span<const Variant> variants;
    // NULL-terminated mapped symbol names the patch needs on every build.
    // The patch is skipped unless the running build maps every one, and its
    // variant's (verified against the loaded game.dll at startup, and again
    // when each is hooked).
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
    // Applies the patch; `base` is game.dll's load address, `variant` the
    // one chosen for the build. Returns false, with a reason in `report`, if
    // it couldn't be applied -- the game then runs with this one patch
    // absent, the others still in force.
    bool (*install)(uintptr_t base, const BuildMappings& build, const Variant& variant, std::string& report);
};

// The variant of `patch` for the build named `build`, or null if none is
// for it (no variant names it and there's no default).
const Variant* variant_for(const Patch& patch, const std::string& build);

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
