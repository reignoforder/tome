// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The two games one game.dll runs: Reign of Chaos and The Frozen Throne.
//
// The game reads `Game.expansion` at startup, before Tome is attached (0 for
// Reign of Chaos: war3.exe -classic, which "Warcraft III.exe" passes; 1 for
// The Frozen Throne). Until then it holds -1: Unknown.
//
// tome.mod.json's "edition" names the one game a mod is for ("roc" or "tft");
// a mod without it runs in both.

#include <cstdint>
#include <string_view>

namespace tome {

enum class Edition : int32_t {
    Unknown = -1,       // not read yet; or, as a mod's "edition", both games
    ReignOfChaos = 0,
    FrozenThrone = 1,
};

// The game's own value (Game.expansion): 0, 1, anything else Unknown.
Edition edition_from_game(int32_t value);

// "Reign of Chaos" / "The Frozen Throne" / "an unknown edition".
const char* edition_name(Edition edition);

// "roc" or "tft", lowercase like tome.mod.json's other keywords. Anything else: false.
bool parse_edition(std::string_view text, Edition& out);

// Whether a mod for `wanted` (Unknown: both) runs in the `running` game. An
// Unknown running game (not read yet) takes every mod.
bool edition_matches(Edition wanted, Edition running);

} // namespace tome
