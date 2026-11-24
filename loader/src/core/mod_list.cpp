// SPDX-License-Identifier: GPL-3.0-or-later
#include "mod_list.h"

#include <algorithm>
#include <cstring>

namespace tome::mod_list {

int32_t state_code(std::string_view state) {
    if (state == "active") return TOME_MOD_ACTIVE;
    if (state == "disabled") return TOME_MOD_DISABLED;
    if (state == "approval") return TOME_MOD_APPROVAL;
    if (state == "waiting") return TOME_MOD_WAITING;
    return TOME_MOD_ERROR;
}

Snapshot::Snapshot(const std::vector<ModStatus>& status) {
    entries_.reserve(status.size());
    for (const ModStatus& s : status) {
        Entry e;
        e.id = s.id;
        e.name = s.name;
        e.version = s.version;
        e.folder = s.folder;
        e.dir = s.dir;
        e.reason = s.reason;
        e.state = state_code(s.state);
        e.enabled = s.user_enabled;
        e.package = s.package;
        e.sync_required = s.sync == "required";
        e.restart_pending = s.restart_pending;
        e.provides = s.provides;
        entries_.push_back(std::move(e));
    }
    provides_.resize(entries_.size());
    infos_.resize(entries_.size());
    for (size_t i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        for (const std::string& p : e.provides) provides_[i].push_back(p.c_str());
        tome_mod_info& info = infos_[i];
        info = {};
        info.size = sizeof(info);
        info.id = e.id.c_str();
        info.name = e.name.c_str();
        info.version = e.version.c_str();
        info.folder = e.folder.c_str();
        info.dir = e.dir.c_str();
        info.state = e.state;
        info.reason = e.reason.c_str();
        info.enabled = e.enabled;
        info.package = e.package;
        info.sync_required = e.sync_required;
        info.restart_pending = e.restart_pending;
        info.provides = provides_[i].empty() ? nullptr : provides_[i].data();
        info.provides_count = static_cast<int32_t>(provides_[i].size());
    }
}

int32_t Snapshot::fill(tome_mod_info* out, uint32_t info_size, int32_t max) const {
    if (max < 0 || (max > 0 && (!out || info_size < sizeof(uint32_t)))) return -1;
    const size_t copy = std::min<size_t>(info_size, sizeof(tome_mod_info));
    for (int32_t i = 0; i < max && static_cast<size_t>(i) < infos_.size(); ++i) {
        char* place = reinterpret_cast<char*>(out) + static_cast<size_t>(i) * info_size;
        std::memset(place, 0, info_size);
        std::memcpy(place, &infos_[static_cast<size_t>(i)], copy);
        reinterpret_cast<tome_mod_info*>(place)->size = static_cast<uint32_t>(copy);
    }
    return static_cast<int32_t>(infos_.size());
}

bool Snapshot::same_as(const std::vector<ModStatus>& status) const {
    if (status.size() != entries_.size()) return false;
    for (size_t i = 0; i < status.size(); ++i) {
        const Entry& e = entries_[i];
        const ModStatus& s = status[i];
        if (e.id != s.id || e.name != s.name || e.version != s.version || e.folder != s.folder || e.dir != s.dir ||
            e.reason != s.reason || e.state != state_code(s.state) || e.enabled != s.user_enabled ||
            e.package != s.package || e.sync_required != (s.sync == "required") ||
            e.restart_pending != s.restart_pending || e.provides != s.provides) {
            return false;
        }
    }
    return true;
}

} // namespace tome::mod_list
