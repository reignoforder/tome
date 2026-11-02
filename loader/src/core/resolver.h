// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Decides which mods can be active and in what order they load.

#include "metadata.h"

#include <string>
#include <vector>

namespace tome {

struct ResolveInput {
    const ModMetadata* meta = nullptr;
    bool user_enabled = true;
};

struct ResolveEnvironment {
    SemVer tome_version;
    GameVersion game_version;
};

enum class ModState {
    Active,
    DisabledByUser,
    IncompatibleGame,   // "game" range doesn't include this build
    IncompatibleTome,   // "tome" range doesn't include this Tome version
    MissingDependency,  // not installed, or installed in a version outside the range
    DependencyInactive, // installed but disabled or itself unable to load
    Breaks,             // a mod listed in "breaks" is active
    DuplicateId,
    Cycle,
};

struct ResolvedMod {
    ModState state = ModState::Active;
    std::string reason;                 // human-readable, for the log and mod menu
    std::vector<std::string> warnings;  // e.g. conflicts, missing recommendations
};

struct ResolveResult {
    std::vector<ResolvedMod> mods;  // parallel to the input
    std::vector<size_t> load_order; // indices of Active mods, dependencies first
};

ResolveResult resolve(const std::vector<ResolveInput>& inputs, const ResolveEnvironment& env);

// The mods to turn off along with inputs[target] (docs/research.md §5.7):
// enabled mods that resolve() lets run now but not once target is disabled,
// because they need it, directly or not, and no other mod that runs gives
// them what they need. Indices in input order, without target.
std::vector<size_t> dependents_of(const std::vector<ResolveInput>& inputs, const ResolveEnvironment& env,
                                  size_t target);

// The mods `result` turned off only because they need one of `waiting`
// (inputs left out of resolve() as not user_enabled for now: they wait for a
// multiplayer game to end, core/game_lock.h), directly or through other such
// mods: user-enabled mods in DependencyInactive whose every dependency no
// active mod meets is one a waiting mod could meet. Indices in input order,
// without `waiting` itself.
std::vector<size_t> waiting_dependents(const std::vector<ResolveInput>& inputs, const ResolveResult& result,
                                       const std::vector<size_t>& waiting);

const char* to_string(ModState state);

} // namespace tome
