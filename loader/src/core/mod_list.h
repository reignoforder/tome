// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The installed mods as the mod API lists them (tome.h, API version 20:
// list_mods): a snapshot of the mod manager's status rows, made at the end
// of each refresh, that hands out tome_mod_info structs pointing into its
// own strings. Immutable once made, so any thread may read it, and a mod
// keeps the one it was last given alive for as long as it uses the strings.

#include "mod_status.h"

#include <tome/tome.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tome::mod_list {

// ModStatus::state ("active", "disabled", ...) as a tome_mod_state; anything
// unknown is TOME_MOD_ERROR.
int32_t state_code(std::string_view state);

class Snapshot {
public:
    explicit Snapshot(const std::vector<ModStatus>& status);
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;

    // Copies up to `max` entries into `out` (`info_size` bytes each: a mod
    // built against an older tome.h passes a smaller struct, a newer one a
    // larger, zeroed past what Tome knows), each with `size` set to what was
    // copied. Returns how many entries there are; -1 for bad arguments.
    int32_t fill(tome_mod_info* out, uint32_t info_size, int32_t max) const;

    size_t size() const { return entries_.size(); }
    // Whether a snapshot of `status` would hand out the same rows (compared
    // in place: nothing is copied).
    bool same_as(const std::vector<ModStatus>& status) const;

private:
    struct Entry {
        std::string id, name, version, folder, dir, reason;
        int32_t state = TOME_MOD_ERROR;
        bool enabled = false, package = false, sync_required = false, restart_pending = false;
        std::vector<std::string> provides;
    };
    std::vector<Entry> entries_;
    // Built once entries_ is complete, so the pointers stay put.
    std::vector<std::vector<const char*>> provides_;
    std::vector<tome_mod_info> infos_;
};

} // namespace tome::mod_list
