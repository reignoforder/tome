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

const char* to_string(ModState state);

} // namespace tome
