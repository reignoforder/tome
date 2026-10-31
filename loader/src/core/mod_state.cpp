#include "mod_state.h"

#include "signature.h"

#include <algorithm>

namespace tome::mod_state {
namespace {

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equal_ignoring_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

bool is_hex_hash(const std::string& text) {
    if (text.size() != 64) return false;
    return std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string string_member(const json::Value& object, std::string_view key) {
    const json::Value* value = object.find(key);
    return value && value->is_string() ? value->as_string() : std::string();
}

} // namespace

State parse(const json::Value& root, int& schema) {
    State state;
    const json::Value* number = root.find("schema");
    schema = 0;
    if (number && number->is_number()) {
        const double n = number->as_number();
        schema = n >= kSchema ? kSchema : n >= 1 ? static_cast<int>(n) : 0;
    }
    const json::Value* mods = root.find("mods");
    if (!mods || !mods->is_object()) return state;
    for (const auto& [id, value] : mods->members()) {
        if (!value.is_object()) continue;
        Entry entry;
        const json::Value* enabled = value.find("enabled");
        if (enabled && enabled->is_bool()) entry.enabled = enabled->as_bool();
        // A malformed hash allows nothing: the mod asks again.
        std::string hash = string_member(value, "approved");
        if (is_hex_hash(hash)) {
            entry.approved.hash = hash;
            entry.approved.folder = string_member(value, "approved_from");
            // A malformed or weak key: as if it wasn't signed (updates ask).
            std::string key = string_member(value, "approved_key");
            signature::Key decoded;
            if (signature::decode_key(key, decoded) && !signature::small_order(decoded)) entry.approved.key = key;
        }
        state[id] = std::move(entry);
    }
    return state;
}

json::Value serialize(const State& state) {
    json::Value root = json::Value::object();
    root.set("schema", json::Value::number(kSchema));
    json::Value mods = json::Value::object();
    for (const auto& [id, entry] : state) {
        json::Value value = json::Value::object();
        value.set("enabled", json::Value::boolean(entry.enabled));
        if (!entry.approved.hash.empty()) {
            value.set("approved", json::Value::string(entry.approved.hash));
            value.set("approved_from", json::Value::string(entry.approved.folder));
            if (!entry.approved.key.empty()) value.set("approved_key", json::Value::string(entry.approved.key));
        }
        mods.set(id, std::move(value));
    }
    root.set("mods", std::move(mods));
    return root;
}

Need need(const Entry* entry, bool has_code, bool package, const std::string& folder, const std::string& code_hash,
          const std::string& signer) {
    if (!has_code) return Need::None;
    if (!entry || entry->approved.hash.empty()) return Need::New;
    if (!package && entry->approved.folder == folder) return Need::None;
    if (code_hash.empty()) return Need::Changed;  // unreadable: never allowed
    if (code_hash == entry->approved.hash) return Need::None;
    if (!entry->approved.key.empty()) return signer == entry->approved.key ? Need::None : Need::SignerChanged;
    return Need::Changed;
}

const char* describe(Need need, CodeKind kind) {
    switch (need) {
    case Need::None: return "";
    case Need::New:
        switch (kind) {
        case CodeKind::Native: return "runs native code; turn it on to allow it";
        case CodeKind::Scripts: return "replaces game scripts; turn it on to allow it";
        case CodeKind::Wasm: return "runs sandboxed code; turn it on to allow it";
        default: return "adds game files; turn it on to allow them";
        }
    case Need::Changed:
        return kind == CodeKind::Native || kind == CodeKind::Scripts || kind == CodeKind::Wasm
                   ? "its code changed since you allowed it; turn it on to allow the new code"
                   : "its files changed since you allowed them; turn it on to allow the new files";
    case Need::SignerChanged:
        return "it's no longer signed by the author's key you allowed it with; turn it on only if you trust the new "
               "files anyway";
    }
    return "";
}

const char* kind_name(CodeKind kind) {
    switch (kind) {
    case CodeKind::None: return "";
    case CodeKind::Assets: return "assets";
    case CodeKind::Wasm: return "wasm";
    case CodeKind::Scripts: return "scripts";
    case CodeKind::Native: return "native";
    }
    return "";
}

bool is_script(std::string_view path) {
    size_t name = path.find_last_of("/\\");
    path.remove_prefix(name == std::string_view::npos ? 0 : name + 1);
    size_t dot = path.rfind('.');
    if (dot == std::string_view::npos) return false;
    std::string_view ext = path.substr(dot);
    return equal_ignoring_case(ext, ".j") || equal_ignoring_case(ext, ".ai") || equal_ignoring_case(ext, ".pld");
}

bool is_code_file(std::string_view path) {
    return !equal_ignoring_case(path, "tome.mod.json") && !equal_ignoring_case(path, signature::kFile);
}

std::string manifest(std::vector<CodeFile> files) {
    std::sort(files.begin(), files.end(), [](const CodeFile& a, const CodeFile& b) { return a.path < b.path; });
    std::string text;
    for (const CodeFile& f : files) text += f.sha256 + "  " + f.path + "\n";
    return text;
}

bool debug_approves(std::string_view list, std::string_view id) {
    while (!list.empty()) {
        size_t comma = list.find(',');
        std::string_view item = list.substr(0, comma);
        while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
        while (!item.empty() && item.back() == ' ') item.remove_suffix(1);
        if (item == "*" || (!item.empty() && item == id)) return true;
        if (comma == std::string_view::npos) break;
        list.remove_prefix(comma + 1);
    }
    return false;
}

} // namespace tome::mod_state
