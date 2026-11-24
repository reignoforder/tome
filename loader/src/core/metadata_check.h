// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Whether a tome.mod.json would run here, as the mod manager decides it: the
// mod API's check_metadata (tome.h, API version 20), for mods that pick a
// release to install, and the "requires" check discovery runs on every
// installed mod. Dependencies aren't part of it: they depend on what else
// is installed (list_mods, version_match).

#include "mappings.h"
#include "metadata.h"
#include "resolver.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tome {

// Errors for the "requires" names `build` lacks: mapped names
// ("CUnit::Foo"), and "property:" capabilities (core/objects.h). A mod's
// own names ("<id>/<name>") are skipped: they're checked against that
// mod's mapping file once every mod is read.
std::vector<std::string> unmet_requires(const ModMetadata& meta, const mappings::Build& build);

struct MetadataCheck {
    // 1: it would run here (its dependencies allowing); 0: valid, but for
    // another game, edition or Tome, or it needs names this build doesn't
    // map; -1: not a valid tome.mod.json.
    int32_t result = -1;
    std::string reason;  // "" for 1; otherwise every problem, "; "-separated
};

// Why discovery refuses a mod's DLL under a Shade that loads none.
inline constexpr const char* kShadeRefusesDll =
    "its DLL can't run under this Shade, which runs only code recompiled with the game (Wasm mods run there; a newer "
    "Shade runs DLLs too)";

// `build`: the running build's mappings, or null if Tome doesn't know it
// (then "requires" names aren't checked, as in discovery, which runs such
// mods anyway). `dlls_run`:
// false where mods' DLLs don't (a Shade without them: kShadeRefusesDll).
MetadataCheck check_metadata_text(std::string_view text, const ResolveEnvironment& env, const mappings::Build* build,
                                  bool dlls_run = true);

// Copies `text` into `out` (`size` bytes, NUL-terminated, cut short at a
// code point boundary if it doesn't fit). Nothing for a null `out` or size 0.
void copy_text(const std::string& text, char* out, size_t size);

} // namespace tome
