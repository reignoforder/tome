// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Portable parts of the asset overlay (loader/src/assets.cpp): where a mod's
// files go in the archive, and which mods' assets the archive should hold
// right now, given each mod's "reload" mode (docs/research.md §5).

#include "metadata.h"

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace tome::assets {

// The archive path of a file inside a mod's assets folder, e.g.
// "Units/Human/Footman/Footman.blp" -> "Units\Human\Footman\Footman.blp".
// Empty if the path is unusable: empty components, "." or "..", a drive
// letter, control characters, or names Storm reserves ("(listfile)").
std::string archive_path(std::string_view relative);

// One mod's contribution to the overlay. `fingerprint` changes whenever a file
// is added, removed or rewritten; mods without asset files aren't listed.
struct ModAssets {
    std::string id;
    ReloadMode reload = ReloadMode::Live;
    uint64_t fingerprint = 0;
};

// When the overlay is being brought up to date.
enum class Moment {
    Startup,   // before the game has loaded anything: everything applies
    Refresh,   // a mod was toggled or its files changed, possibly mid-game
    MainMenu,  // the main menu is showing: no map is loaded
    MapLoad,   // a map is starting to load and hasn't read any of its files yet
};

struct Pending {
    std::string id;
    ReloadMode reload;  // Map: waits for the main menu or a map load; Restart: for the next launch
};

struct Plan {
    std::vector<ModAssets> mount;  // the archive's contents, lowest precedence first; a mod
                                   // that's waiting keeps its place
    std::vector<Pending> pending;  // changes that have to wait
    bool changed = false;          // the archive has to be rebuilt (files or order differ)
};

// `mounted` is what the archive holds now; `wanted` is every running mod with
// assets, in load order (a later mod's files win). A change to a mod (added,
// removed or new files) applies now if its reload mode allows it at this
// moment; otherwise the mod keeps what it has mounted and is listed as
// pending. A removed mod is judged by the mode it was mounted with. A change
// of reload mode alone needs no rebuild; `mount` carries the new mode.
// `frozen` mods (locked during a multiplayer game, core/game_lock.h) keep
// exactly what they have mounted, in place, whatever `wanted` says, and
// aren't listed as pending: their changes apply once the game ends.
Plan plan(const std::vector<ModAssets>& mounted, const std::vector<ModAssets>& wanted, Moment moment,
          const std::set<std::string>& frozen = {});

} // namespace tome::assets
