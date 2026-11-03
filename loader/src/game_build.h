// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/edition.h"
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
    Edition edition = Edition::Unknown;    // Reign of Chaos or The Frozen Throne (read_edition)
};

// The game the running game.dll is (Game.expansion), set before Tome is
// attached; Unknown before that, or for a build without mappings.
Edition read_edition(uintptr_t base, const BuildMappings* known);

// Gathers version information about a loaded game.dll and matches it
// against the known builds. Returns false only if the module can't be read.
bool identify_game_build(HMODULE game, GameBuild& out);

// True when the game runs under Wine (ntdll exports wine_get_version).
bool running_on_wine();

// CGameUI::Get(create, forget): the game UI (null outside a game with
// create 0). Main thread.
using GameUiGetFn = void*(__fastcall*)(int create, int forget);
inline GameUiGetFn game_ui_get(uintptr_t base, const BuildMappings& build) {
    return reinterpret_cast<GameUiGetFn>(base + build.game.game_ui_get);
}

} // namespace tome
