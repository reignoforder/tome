#pragma once

#include "mappings.h"

#include <windows.h>

#include <cstdint>
#include <string>

namespace tome {

struct GameBuild {
    HMODULE module = nullptr;
    uintptr_t base = 0;        // add RVAs from the mappings to this
    std::wstring path;
    std::string file_version;
    uint32_t pe_timestamp = 0;
    std::string sha256;
    const BuildMappings* known = nullptr;  // null if the build isn't supported
};

// Gathers version information about a loaded game.dll and matches it
// against the known builds. Returns false only if the module can't be read.
bool identify_game_build(HMODULE game, GameBuild& out);

// True when the game runs under Wine (ntdll exports wine_get_version).
bool running_on_wine();

} // namespace tome
