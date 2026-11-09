// SPDX-License-Identifier: GPL-3.0-or-later
// tome.mod.json, a settings schema and a mod's own mapping file (from mods):
// parse, then every reader that takes the parsed value, and a round trip
// through serialize.
#include "core/json.h"
#include "core/keys.h"
#include "core/mappings.h"
#include "core/metadata.h"
#include "core/metadata_check.h"
#include "core/settings.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
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
    // The mod API's check_metadata, on the same bytes (a release a mod is about to install).
    static const ResolveEnvironment env = [] {
        ResolveEnvironment e;
        parse_semver("0.1.0", e.tome_version);
        parse_game_version("1.27b", e.game_version);
        return e;
    }();
    MetadataCheck check = check_metadata_text(text, env, nullptr);
    if (check.result < -1 || check.result > 1 || (check.result == 1) != check.reason.empty()) __builtin_trap();
    char reason[16];
    copy_text(check.reason, reason, sizeof(reason));
    if (std::memchr(reason, 0, sizeof(reason)) == nullptr) __builtin_trap();

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

    // A mod's mapping file, as read for 1.27b (core/mappings.h).
    static const mappings::Build running = [] {
        mappings::Build b;
        b.name = "1.27b";
        b.file_version = "1.27.1.7085";
        b.pe_timestamp = 0x584a499e;
        b.size_of_image = 0x00e6e000;
        b.sha256 = "d51e5680243fc90e19c9d6074f7fac433c466d3cf5f46e2364291725574d8236";
        return b;
    }();
    mappings::Build own;
    if (mappings::parse_mod_build(root, running, own).ok) {
        for (const mappings::Symbol& sym : own.symbols) {
            if (mappings::has_rva(sym.kind) && sym.value >= running.size_of_image) __builtin_trap();
            if (sym.value + sym.bytes.size() > running.size_of_image && sym.kind == mappings::Kind::Function) __builtin_trap();
        }
    }

    std::vector<keys::Declared> declared;
    if (const json::Value* list = root.find("keybinds")) keys::parse_declared(*list, declared, errors);
    return 0;
}
