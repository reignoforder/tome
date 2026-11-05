// SPDX-License-Identifier: GPL-3.0-or-later
#include "assets.h"

#include "text.h"

#include <algorithm>

namespace tome::assets {
namespace {

using text::iequals;

bool applies_now(ReloadMode mode, Moment moment) {
    switch (moment) {
    case Moment::Startup: return true;
    case Moment::MainMenu:
    case Moment::MapLoad: return mode != ReloadMode::Restart;
    case Moment::Refresh: return mode == ReloadMode::Live;
    }
    return false;
}

const ModAssets* find(const std::vector<ModAssets>& list, const std::string& id) {
    for (const ModAssets& m : list) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

} // namespace

std::string archive_path(std::string_view relative) {
    std::string out;
    size_t start = 0;
    while (start <= relative.size()) {
        size_t end = relative.find_first_of("/\\", start);
        if (end == std::string_view::npos) end = relative.size();
        std::string_view part = relative.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return {};
        for (char c : part) {
            if (c == ':' || static_cast<unsigned char>(c) < 0x20) return {};
        }
        if (!out.empty()) out += '\\';
        out += part;
        start = end + 1;
    }
    if (iequals(out, "(listfile)") || iequals(out, "(attributes)") || iequals(out, "(signature)")) return {};
    return out;
}

Plan plan(const std::vector<ModAssets>& mounted, const std::vector<ModAssets>& wanted, Moment moment,
          const std::set<std::string>& frozen) {
    Plan result;
    for (const ModAssets& w : wanted) {
        const ModAssets* current = find(mounted, w.id);
        if (frozen.count(w.id)) {
            if (current) result.mount.push_back(*current);
        } else if (current && current->fingerprint == w.fingerprint) {
            result.mount.push_back(w);
        } else if (applies_now(w.reload, moment)) {
            result.mount.push_back(w);
        } else {
            if (current) result.mount.push_back(*current);
            result.pending.push_back({w.id, w.reload});
        }
    }
    // Mods that aren't wanted any more but can't be removed yet keep their
    // place: right after the mod they followed before, if it's still there.
    for (size_t i = 0; i < mounted.size(); ++i) {
        const ModAssets& m = mounted[i];
        bool held = frozen.count(m.id) > 0;
        if (find(wanted, m.id) || (!held && applies_now(m.reload, moment))) continue;
        auto at = result.mount.begin();
        for (size_t j = i; j-- > 0;) {
            auto previous = std::find_if(result.mount.begin(), result.mount.end(),
                                         [&](const ModAssets& r) { return r.id == mounted[j].id; });
            if (previous != result.mount.end()) {
                at = previous + 1;
                break;
            }
        }
        result.mount.insert(at, m);
        if (!held) result.pending.push_back({m.id, m.reload});
    }
    result.changed = result.mount.size() != mounted.size();
    for (size_t i = 0; !result.changed && i < mounted.size(); ++i) {
        result.changed = result.mount[i].id != mounted[i].id || result.mount[i].fingerprint != mounted[i].fingerprint;
    }
    return result;
}

} // namespace tome::assets
