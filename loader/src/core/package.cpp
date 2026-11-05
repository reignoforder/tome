// SPDX-License-Identifier: GPL-3.0-or-later
#include "package.h"

#include <cstdio>

#include "text.h"

namespace tome::package {

std::string stem(std::string_view file) {
    if (file.size() <= kExtension.size()) return {};
    std::string_view tail = file.substr(file.size() - kExtension.size());
    if (!text::iequals(tail, kExtension)) return {};
    return std::string(file.substr(0, file.size() - kExtension.size()));
}

std::string unpacked_name(std::string_view stem, uint64_t hash) {
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(hash));
    return std::string(stem) + "." + hex;
}

bool is_unpacked_name(std::string_view name) {
    constexpr std::string_view kPartial = ".partial";
    if (name.size() > kPartial.size() && name.substr(name.size() - kPartial.size()) == kPartial) return true;
    // "<stem>.<16 lowercase hex digits>" with a non-empty stem.
    if (name.size() < 18 || name[name.size() - 17] != '.') return false;
    for (char c : name.substr(name.size() - 16)) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

std::string partial_name(std::string_view unpacked, uint32_t process_id) {
    return std::string(unpacked) + "." + std::to_string(process_id) + ".partial";
}

bool check_entries(const std::vector<zip::Entry>& entries, std::string& error) {
    for (const zip::Entry& e : entries) {
        if (!e.directory() && text::iequals(e.name, "tome.mod.json")) return true;
    }
    bool nested = false;
    for (const zip::Entry& e : entries) {
        size_t slash = e.name.rfind('/');
        if (!e.directory() && slash != std::string::npos &&
            text::iequals(std::string_view(e.name).substr(slash + 1), "tome.mod.json")) {
            nested = true;
        }
    }
    error = nested ? "tome.mod.json is in a subfolder; pack the mod folder's contents, not the folder"
                   : "no tome.mod.json in the package";
    return false;
}

} // namespace tome::package
