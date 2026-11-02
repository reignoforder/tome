// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Per-mod settings (docs/research.md §8.3): the schema a mod ships as
// "config" in tome.mod.json (config.schema.json), and the values Tome keeps
// in <mod>/config/settings.json. The mod menu generates its settings page
// from the schema; mods read the values through the API (version 7).
//
// The schema is a small subset of JSON Schema:
//
//   { "type": "object",
//     "properties": {
//       "show":  { "type": "boolean", "title": "Show bars", "default": true },
//       "scale": { "type": "number", "minimum": 0.5, "maximum": 2, "step": 0.1, "default": 1 },
//       "count": { "type": "integer", "minimum": 0, "maximum": 10, "default": 3 },
//       "mode":  { "type": "string", "enum": ["auto", "always"], "enumTitles": ["Auto", "Always"] },
//       "name":  { "type": "string", "maxLength": 32, "default": "" } } }
//
// Settings appear in the order of "properties".

#include "json.h"

#include <string>
#include <string_view>
#include <vector>

namespace tome::settings {

enum class Type { Bool, Integer, Number, String };

struct Choice {
    std::string value;
    std::string title;
};

struct Setting {
    std::string key;
    std::string title;        // defaults to the key
    std::string description;
    Type type = Type::Bool;
    json::Value fallback;     // "default", or false / the minimum / 0 / the first choice / ""
    bool has_minimum = false, has_maximum = false;
    double minimum = 0, maximum = 0;
    double step = 0;          // Integer, Number: what the menu's -/+ buttons change; never 0 after parsing
    bool text_input = false;  // Integer, Number: "input": "text", a box to type the value in instead of -/+
    std::vector<Choice> choices;  // String: an "enum"; empty for free text
    size_t max_length = 256;      // String, in bytes
};

struct Schema {
    std::vector<Setting> settings;
    const Setting* find(std::string_view key) const;
};

constexpr size_t kMaxSettings = 64;
constexpr size_t kMaxKeyLength = 64;

// False with errors for a schema Tome can't use. Unknown keywords are
// ignored (with a warning), so a schema written for full JSON Schema tools
// still works as far as Tome understands it.
bool parse_schema(const json::Value& root, Schema& out, std::vector<std::string>& errors,
                  std::vector<std::string>& warnings);

// `value` as a valid value of `setting`: numbers rounded (Integer) and
// clamped to the range, strings cut to max_length. False (with `error`) if
// it has the wrong type or isn't one of the choices.
bool coerce(const Setting& setting, const json::Value& value, json::Value& out, std::string& error);

// Every setting's value: those in `stored` (settings.json) that are valid,
// the default for the rest. Members of `stored` the schema doesn't know are
// kept as they are, so an older version of the mod doesn't lose a newer
// one's settings. `warnings` names values that were replaced.
json::Value load_values(const Schema& schema, const json::Value& stored, std::vector<std::string>& warnings);

// What the menu shows for a value: "On"/"Off", the choice's title, a number
// without trailing zeros, or the text.
std::string display(const Setting& setting, const json::Value& value);

// A number the player typed for an Integer or Number setting ("250", " 1.5 "),
// as a value for coerce (which clamps and rounds it). False if the text isn't
// a number.
bool parse_typed_number(const std::string& text, json::Value& out);

// The menu's buttons: the next value in `direction` (+1 or -1). Bool flips,
// choices cycle, numbers move by `step` within the range. Free text doesn't
// change.
json::Value step(const Setting& setting, const json::Value& value, int direction);

const char* type_name(Type type);

} // namespace tome::settings
