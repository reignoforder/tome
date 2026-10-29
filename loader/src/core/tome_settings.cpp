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
