// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/intermod.h"

#include <algorithm>

namespace tome::intermod {

bool valid_name(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
    });
}

Result Services::add(const std::string& owner, const std::string& name, uint32_t version, const void* table) {
    if (owner.empty() || !valid_name(name) || !table) return Result::Invalid;
    for (Service& s : services_) {
        if (s.name != name) continue;
        if (s.owner != owner) return Result::Taken;
        s.version = version;
        s.table = table;
        return Result::Updated;
    }
    services_.push_back({name, owner, version, table});
    return Result::Ok;
}

bool Services::remove(const std::string& owner, const std::string& name) {
    return std::erase_if(services_, [&](const Service& s) { return s.owner == owner && s.name == name; }) > 0;
}

std::optional<Service> Services::find(const std::string& name, uint32_t version) const {
    for (const Service& s : services_) {
        if (s.name == name && (version == 0 || s.version == version)) return s;
    }
    return std::nullopt;
}

} // namespace tome::intermod
