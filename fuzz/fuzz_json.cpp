// tome.mod.json and a settings schema (from mods): parse, then every reader
// that takes the parsed value, and a round trip through serialize.
#include "core/json.h"
#include "core/keys.h"
#include "core/metadata.h"
#include "core/settings.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

using namespace tome;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string_view text(reinterpret_cast<const char*>(data), size);
    json::Value root;
    if (!json::parse(text, root).ok) return 0;

    std::string again = json::serialize(root);
    json::Value reparsed;
    if (!json::parse(again, reparsed).ok) __builtin_trap();  // serialize must produce valid JSON

    ModMetadata meta;
    parse_metadata(root, meta);

    settings::Schema schema;
    std::vector<std::string> errors, warnings;
    if (settings::parse_schema(root, schema, errors, warnings)) {
        const json::Value* values = root.find("values");
        json::Value loaded = settings::load_values(schema, values ? *values : json::Value(), warnings);
        for (const settings::Setting& s : schema.settings) {
            const json::Value* v = loaded.find(s.key);
            if (!v) continue;
            settings::display(s, *v);
            settings::step(s, *v, 1);
            settings::step(s, *v, -1);
        }
    }

    std::vector<keys::Declared> declared;
    if (const json::Value* list = root.find("keybinds")) keys::parse_declared(*list, declared, errors);
    return 0;
}
