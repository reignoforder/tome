// SPDX-License-Identifier: GPL-3.0-or-later
#include "metadata.h"

#include "jass.h"

#include <cmath>
#include <cstdio>

namespace tome {
namespace {

// Fields Tome knows about but doesn't act on (yet). Accepted without a
// warning. "$schema" lets editors find sdk/schemas/tome.mod.schema.json.
const char* const kReservedFields[] = {"links", "$schema"};

const char* const kKnownFields[] = {
    "schema", "id", "version", "name", "description", "authors", "license", "icon",
    "game", "tome", "depends", "recommends", "conflicts", "breaks", "entry", "wasm", "assets",
    "requires", "natives", "provides", "sync", "reload", "load", "config", "keybinds",
};

class Reader {
public:
    Reader(const json::Value& root, MetadataResult& result) : root_(root), result_(result) {}

    const json::Value* get(const char* key, json::Value::Type type, bool required) {
        const json::Value* v = root_.find(key);
        if (!v) {
            if (required) error(std::string("missing required field \"") + key + "\"");
            return nullptr;
        }
        if (v->type() != type) {
            error(std::string("\"") + key + "\" must be a " + json::type_name(type) +
                  ", not a " + json::type_name(v->type()));
            return nullptr;
        }
        return v;
    }

    bool string(const char* key, std::string& out, bool required) {
        const json::Value* v = get(key, json::Value::Type::String, required);
        if (!v) return false;
        out = v->as_string();
        return true;
    }

    void string_list(const char* key, std::vector<std::string>& out) {
        const json::Value* v = get(key, json::Value::Type::Array, false);
        if (!v) return;
        for (const json::Value& item : v->items()) {
            if (!item.is_string()) {
                error(std::string("\"") + key + "\" must contain only strings");
                return;
            }
            out.push_back(item.as_string());
        }
    }

    void dependency_map(const char* key, std::vector<Dependency>& out) {
        const json::Value* v = get(key, json::Value::Type::Object, false);
        if (!v) return;
        for (const auto& [id, range_value] : v->members()) {
            if (!range_value.is_string()) {
                error(std::string("\"") + key + "." + id + "\" must be a version range string");
                continue;
            }
            SemVerRange range;
            std::string err;
            if (!parse_semver_range(range_value.as_string(), range, err)) {
                error(std::string("\"") + key + "." + id + "\": " + err);
                continue;
            }
            out.push_back(Dependency{id, std::move(range)});
        }
    }

    void error(std::string message) { result_.errors.push_back(std::move(message)); }
    void warning(std::string message) { result_.warnings.push_back(std::move(message)); }

private:
    const json::Value& root_;
    MetadataResult& result_;
};

bool is_known_field(const std::string& key) {
    for (const char* known : kKnownFields) {
        if (key == known) return true;
    }
    for (const char* reserved : kReservedFields) {
        if (key == reserved) return true;
    }
    return false;
}

// Rejects absolute paths and anything escaping the mod folder.
bool is_safe_relative_path(const std::string& path) {
    if (path.empty() || path[0] == '/' || path[0] == '\\' || path.find(':') != std::string::npos) {
        return false;
    }
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find_first_of("/\\", start);
        std::string part = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part == "..") return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

} // namespace

bool is_valid_mod_id(std::string_view id) {
    if (id.size() < 2 || id.size() > kMaxIdLength) return false;
    if (id[0] < 'a' || id[0] > 'z') return false;
    for (char c : id) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

const char* to_string(SyncMode mode) {
    return mode == SyncMode::Required ? "required" : "client";
}

const char* to_string(ReloadMode mode) {
    switch (mode) {
    case ReloadMode::Live:    return "live";
    case ReloadMode::Map:     return "map";
    case ReloadMode::Restart: return "restart";
    }
    return "?";
}

std::string ModMetadata::entry_for(const GameVersion& game) const {
    if (entry_by_game.empty()) {
        return entry;
    }
    for (const auto& [range, path] : entry_by_game) {
        if (range.matches(game)) return path;
    }
    return {};
}

MetadataResult parse_metadata(const json::Value& root, ModMetadata& out) {
    MetadataResult result;
    if (!root.is_object()) {
        result.errors.push_back("tome.mod.json must contain a JSON object");
        return result;
    }
    Reader r(root, result);
    ModMetadata m;

    if (const json::Value* schema = r.get("schema", json::Value::Type::Number, true)) {
        double n = schema->as_number();
        if (n != std::floor(n) || n < 1) {
            r.error("\"schema\" must be a positive integer");
        } else if (n > kMetadataSchema) {
            char text[32];  // n may not fit in an int
            std::snprintf(text, sizeof(text), "%.17g", n);
            r.error("\"schema\" " + std::string(text) +
                    " is newer than this Tome supports (" + std::to_string(kMetadataSchema) + "); update Tome");
        } else {
            m.schema = static_cast<int>(n);
        }
    }

    if (r.string("id", m.id, true)) {
        if (!is_valid_mod_id(m.id)) {
            r.error("\"id\" \"" + m.id + "\" is invalid: use 2-64 characters, a lowercase letter "
                    "first, then lowercase letters, digits, '-' or '_'");
        } else if (m.id == "tome") {
            r.error("\"id\" \"tome\" is reserved");
        }
    }

    std::string text;
    if (r.string("version", text, true)) {
        if (!parse_semver(text, m.version)) {
            r.error("\"version\" \"" + text + "\" is not a semantic version (e.g. 1.0.0)");
        } else if (text.size() > kMaxVersionLength) {
            // The multiplayer handshake sends id, version and hash in one chat line.
            r.error("\"version\" is longer than " + std::to_string(kMaxVersionLength) + " characters");
        }
    }

    if (!r.string("name", m.name, false)) m.name = m.id;
    r.string("description", m.description, false);
    r.string_list("authors", m.authors);
    r.string("license", m.license, false);
    if (r.string("icon", m.icon, false) && !is_safe_relative_path(m.icon)) {
        r.error("\"icon\" must be a path inside the mod folder");
    }

    std::string err;
    if (r.string("game", text, true) && !parse_game_range(text, m.game, err)) {
        r.error("\"game\": " + err);
    }
    if (r.string("tome", text, true) && !parse_semver_range(text, m.tome, err)) {
        r.error("\"tome\": " + err);
    }

    r.dependency_map("depends", m.depends);
    r.dependency_map("recommends", m.recommends);
    r.dependency_map("conflicts", m.conflicts);
    r.dependency_map("breaks", m.breaks);
    for (const Dependency& dep : m.depends) {
        if (dep.id == m.id) r.error("a mod can't depend on itself");
    }

    if (const json::Value* entry = root.find("entry")) {
        if (entry->is_string()) {
            m.entry = entry->as_string();
            if (!is_safe_relative_path(m.entry)) r.error("\"entry\" must be a path inside the mod folder");
        } else if (entry->is_object()) {
            for (const auto& [range_text, path] : entry->members()) {
                GameVersionRange range;
                if (!parse_game_range(range_text, range, err)) {
                    r.error("\"entry\" key: " + err);
                } else if (!path.is_string() || !is_safe_relative_path(path.as_string())) {
                    r.error("\"entry." + range_text + "\" must be a path inside the mod folder");
                } else {
                    m.entry_by_game.emplace_back(std::move(range), path.as_string());
                }
            }
        } else {
            r.error("\"entry\" must be a string or an object mapping game versions to paths");
        }
    }
    if (const json::Value* wasm = root.find("wasm")) {
        if (!wasm->is_string()) {
            r.error("\"wasm\" must be a path inside the mod folder");
        } else {
            m.wasm = wasm->as_string();
            if (!is_safe_relative_path(m.wasm)) r.error("\"wasm\" must be a path inside the mod folder");
            else if (m.wasm.size() < 5 || m.wasm.compare(m.wasm.size() - 5, 5, ".wasm") != 0)
                r.error("\"wasm\" must name a .wasm file");
            if (!m.entry.empty() || !m.entry_by_game.empty())
                r.error("a mod has \"entry\" (a DLL) or \"wasm\" (a WebAssembly module), not both");
        }
    }

    if (r.string("assets", m.assets, false) && !is_safe_relative_path(m.assets)) {
        r.error("\"assets\" must be a path inside the mod folder");
    }
    r.string_list("requires", m.requires_);
    r.string_list("natives", m.natives);
    for (const std::string& decl_text : m.natives) {
        jass::NativeDecl decl;
        std::string native_err;
        if (!jass::parse_native(decl_text, decl, native_err)) {
            r.error("\"natives\" entry \"" + decl_text + "\": " + native_err);
        }
    }
    r.string_list("provides", m.provides);
    for (const std::string& id : m.provides) {
        if (!is_valid_mod_id(id)) r.error("\"provides\" entry \"" + id + "\" is not a valid id");
    }

    if (r.string("sync", text, true)) {
        if (text == "client") m.sync = SyncMode::Client;
        else if (text == "required") m.sync = SyncMode::Required;
        else r.error("\"sync\" must be \"client\" or \"required\"");
    }
    if (r.string("reload", text, false)) {
        if (text == "live") m.reload = ReloadMode::Live;
        else if (text == "map") m.reload = ReloadMode::Map;
        else if (text == "restart") m.reload = ReloadMode::Restart;
        else r.error("\"reload\" must be \"live\", \"map\" or \"restart\"");
    }

    if (const json::Value* load = r.get("load", json::Value::Type::Object, false)) {
        Reader lr(*load, result);
        lr.string_list("after", m.load_after);
        lr.string_list("before", m.load_before);
    }

    if (r.string("config", m.config, false) && !is_safe_relative_path(m.config)) {
        r.error("\"config\" must be a path inside the mod folder");
    }
    if (const json::Value* keybinds = root.find("keybinds")) {
        std::vector<std::string> errors;
        if (!keys::parse_declared(*keybinds, m.keybinds, errors)) {
            for (std::string& e : errors) r.error(std::move(e));
        }
    }

    for (const auto& member : root.members()) {
        if (!is_known_field(member.first)) {
            r.warning("unknown field \"" + member.first + "\" (ignored)");
        }
    }

    result.ok = result.errors.empty();
    if (result.ok) out = std::move(m);
    return result;
}

} // namespace tome
