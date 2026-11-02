// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Bookkeeping for mods talking to each other (tome.h, API version 11):
// services (a mod publishes a table of functions under a name, others look
// it up) and topics (publish/subscribe messages). Portable; the loader's
// intermod.cpp adds the locks, the main thread and the guarded calls.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tome::intermod {

// Service and topic names: 1 to 64 characters of a-z, 0-9, '.', '-', '_'.
// By convention they start with the mod's id ("hp-bars.api").
bool valid_name(const std::string& name);

struct Service {
    std::string name;
    std::string owner;       // mod id
    uint32_t version = 0;    // bumped by the provider when it changes the table incompatibly
    const void* table = nullptr;
};

enum class Result { Ok, Updated, Invalid, Taken };

class Services {
public:
    // One provider per name; a mod may register several names. Registering
    // a name the owner already has replaces its version and table (Updated).
    Result add(const std::string& owner, const std::string& name, uint32_t version, const void* table);

    // Removes `name` if `owner` registered it. True if something was removed.
    bool remove(const std::string& owner, const std::string& name);

    // The service, if its version is exactly `version` or, for
    // `version` 0, any. Versions are compatibility levels, not a range.
    std::optional<Service> find(const std::string& name, uint32_t version) const;

private:
    std::vector<Service> services_;
};

} // namespace tome::intermod
