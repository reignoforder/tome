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
    if (const json::Value* v = values.find(key); v && v->is_bool()) return v->as_bool();
    const settings::Setting* s = schema().find(key);
    return s && s->fallback.is_bool() && s->fallback.as_bool();
}

} // namespace tome::tome_settings
