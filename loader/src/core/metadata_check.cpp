// SPDX-License-Identifier: GPL-3.0-or-later
#include "metadata_check.h"

#include "json.h"
#include "objects.h"

#include <cstring>

namespace tome {

std::vector<std::string> unmet_requires(const ModMetadata& meta, const mappings::Build& build) {
    std::vector<std::string> errors;
    for (const std::string& name : meta.requires_) {
        if (name.find('/') != std::string::npos) continue;
        // A single ':' is a capability ("property:mana"); "Class::Method" is a mapping.
        const bool capability = name.find(':') != std::string::npos && name.find("::") == std::string::npos;
        std::string error;
        if (capability) {
            if (!objects::capability_met(name, [&](const char* s) { return build.find(s) != nullptr; }, error)) {
                errors.push_back(error);
            }
        } else if (!build.find(name)) {
            errors.push_back("needs \"" + name + "\", which isn't mapped for " + build.name);
        }
    }
    return errors;
}

MetadataCheck check_metadata_text(std::string_view text, const ResolveEnvironment& env, const mappings::Build* build,
                                  bool dlls_run) {
    MetadataCheck check;
    json::Value root;
    json::ParseResult parsed = json::parse(text, root);
    if (!parsed.ok) {
        check.reason = "tome.mod.json: " + parsed.error;
        return check;
    }
    ModMetadata meta;
    MetadataResult result = parse_metadata(root, meta);
    auto add = [&](const std::string& problem) { check.reason += (check.reason.empty() ? "" : "; ") + problem; };
    if (!result.ok) {
        for (const std::string& e : result.errors) add(e);
        if (check.reason.empty()) add("invalid tome.mod.json");
        return check;
    }
    ModState state;
    std::string reason;
    if (!compatible(meta, env, state, reason)) add(reason);
    // As discovery: an unknown build checks no "requires" names.
    if (build) {
        for (const std::string& e : unmet_requires(meta, *build)) add(e);
    }
    // As discovery: a DLL for this game where none loads.
    if (!dlls_run && !meta.entry_for(env.game_version).empty()) add(kShadeRefusesDll);
    check.result = check.reason.empty() ? 1 : 0;
    return check;
}

void copy_text(const std::string& text, char* out, size_t size) {
    if (!out || size == 0) return;
    size_t cut = text.size();
    if (cut >= size) {
        cut = size - 1;
        // Back to the start of the code point the cut falls in.
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    }
    std::memcpy(out, text.data(), cut);
    out[cut] = '\0';
}

} // namespace tome
