#pragma once

// Per-build addresses in game.dll, loaded from the mapping database
// (mappings/*.json, compiled into tome.mix). RVAs: add the module base at runtime.

#include "core/mappings.h"

#include <windows.h>

#include <string>
#include <vector>

namespace tome {

// Cheap identification from the in-memory PE header only (no file I/O), so it
// can run inside DllMain. The SHA-256 check happens later, off the loader lock.
const BuildMappings* find_build_by_header(HMODULE game);

const BuildMappings* find_build_by_sha256(const char* sha256);

// The mapping of a loaded blizzard.ax build (module "blizzard.ax"), by its
// PE header; null if it isn't one.
const BlizzardAxMappings* find_blizzard_ax_by_header(HMODULE ax);

// Checks the loaded module's code against the build's byte signatures and
// call sites (mappings::verify_code). Memory only, so it can run inside
// DllMain; it must run before Tome patches anything. Each error names a symbol.
bool verify_loaded_build(HMODULE game, const BuildMappings& build, std::vector<std::string>& errors);

// The same for another loaded module and its mapping.
bool verify_loaded_module(HMODULE module, const mappings::Build& build, std::vector<std::string>& errors);

// Problems found while loading the mapping files ("1.27b.json: ..."). A file
// with any problem is skipped, so its build counts as unsupported.
const std::vector<std::string>& mapping_errors();

} // namespace tome
