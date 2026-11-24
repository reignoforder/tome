// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/tome_settings.h"

namespace tome::tome_settings {
namespace {

constexpr const char* kSchema = R"({
  "type": "object",
  "properties": {
    "widescreen": {
      "type": "boolean",
      "title": "Widescreen",
      "default": true,
      "description": "Shows more of the map at the sides on a screen wider than 4:3, instead of stretching the picture. Also in multiplayer."
    },
    "update_check": {
      "type": "boolean",
      "title": "Check for Updates",
      "default": false,
      "description": "Once a day at startup, asks GitHub whether there's a newer Tome and says so. Installs only when you click Update Now (or with Install Updates Automatically on); sends nothing about you or your mods."
    },
    "update_install": {
      "type": "boolean",
      "title": "Install Updates Automatically",
      "default": false,
      "description": "With Check for Updates on: installs a new Tome as soon as it's found, if it's signed with Tome's release key, without asking. It runs from the next start; the game isn't restarted for it."
    },
    "harden_heap": {
      "type": "boolean",
      "title": "Stop on Memory Corruption",
      "default": false,
      "description": "Ends the game at once if Windows finds its memory corrupted, as an exploit might do, instead of running on. From the next start; not under Wine."
    }
  }
})";

settings::Schema parse() {
    settings::Schema schema;
    json::Value root;
    std::vector<std::string> errors, warnings;
    if (json::parse(kSchema, root).ok) settings::parse_schema(root, schema, errors, warnings);
    return schema;
}

} // namespace

const settings::Schema& schema() {
    static const settings::Schema s = parse();
    return s;
}

bool get_bool(const json::Value& values, const char* key) {
    return get_bool(schema(), values, key);
}

bool get_bool(const settings::Schema& schema, const json::Value& values, const char* key) {
    if (const json::Value* v = values.find(key); v && v->is_bool()) return v->as_bool();
    const settings::Setting* s = schema.find(key);
    return s && s->fallback.is_bool() && s->fallback.as_bool();
}

std::string patch_key(std::string_view name) {
    return kPatchPrefix + std::string(name);
}

settings::Schema with_patches(const settings::Schema& base, const std::vector<PatchInfo>& patches) {
    settings::Schema out = base;
    for (const PatchInfo& p : patches) {
        if (base.find(p.name)) continue;
        settings::Setting s;
        s.key = patch_key(p.name);
        s.title = "Patch: " + std::string(p.name);
        std::string summary(p.summary);
        if (!summary.empty() && summary[0] >= 'a' && summary[0] <= 'z') {
            summary[0] = static_cast<char>(summary[0] - 'a' + 'A');
        }
        s.description = (p.now.empty() ? "" : p.now + " ") + summary +
                        ". A fix built into Tome; turned off, it isn't applied from the next start.";
        s.type = settings::Type::Bool;
        s.fallback = json::Value::boolean(true);
        out.settings.push_back(std::move(s));
    }
    return out;
}

std::string patch_off_reason(std::string_view name, const std::vector<std::string>& flags, const json::Value& values) {
    for (const std::string& f : flags) {
        if (f == "all" || f == name) return "turned off by -tomenopatch";
    }
    const json::Value* v = values.find(patch_key(name));
    if (v && v->is_bool() && !v->as_bool()) return "turned off in Tome > Settings";
    return "";
}

} // namespace tome::tome_settings
