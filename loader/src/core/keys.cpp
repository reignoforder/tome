#include "keys.h"

#include <cctype>
#include <cstdio>

namespace tome::keys {
namespace {

// Windows virtual-key codes (winuser.h), so this file needs no Windows headers.
constexpr uint8_t kVkBack = 0x08, kVkTab = 0x09, kVkReturn = 0x0D, kVkShift = 0x10, kVkControl = 0x11,
                  kVkMenu = 0x12, kVkPause = 0x13, kVkEscape = 0x1B, kVkSpace = 0x20, kVkPrior = 0x21,
                  kVkNext = 0x22, kVkEnd = 0x23, kVkHome = 0x24, kVkLeft = 0x25, kVkUp = 0x26, kVkRight = 0x27,
                  kVkDown = 0x28, kVkSnapshot = 0x2C, kVkInsert = 0x2D, kVkDelete = 0x2E, kVkNumpad0 = 0x60,
                  kVkF1 = 0x70, kVkLShift = 0xA0, kVkRMenu = 0xA5;

struct Named {
    const char* name;
    uint8_t vk;
};

const Named kNamed[] = {
    {"Backspace", kVkBack}, {"Tab", kVkTab}, {"Enter", kVkReturn}, {"Pause", kVkPause}, {"Esc", kVkEscape},
    {"Space", kVkSpace}, {"PageUp", kVkPrior}, {"PageDown", kVkNext}, {"End", kVkEnd}, {"Home", kVkHome},
    {"Left", kVkLeft}, {"Up", kVkUp}, {"Right", kVkRight}, {"Down", kVkDown}, {"PrintScreen", kVkSnapshot},
    {"Insert", kVkInsert}, {"Delete", kVkDelete},
    {"NumMultiply", 0x6A}, {"NumAdd", 0x6B}, {"NumSubtract", 0x6D}, {"NumDecimal", 0x6E}, {"NumDivide", 0x6F},
    {";", 0xBA}, {"=", 0xBB}, {",", 0xBC}, {"-", 0xBD}, {".", 0xBE}, {"/", 0xBF}, {"`", 0xC0}, {"[", 0xDB},
    {"\\", 0xDC}, {"]", 0xDD}, {"'", 0xDE},
};

// Other spellings parse() accepts.
const Named kAliases[] = {
    {"Escape", kVkEscape}, {"Return", kVkReturn}, {"PgUp", kVkPrior}, {"PgDn", kVkNext}, {"Del", kVkDelete},
    {"Ins", kVkInsert}, {"Grave", 0xC0}, {"Minus", 0xBD}, {"Equals", 0xBB}, {"Plus", 0xBB}, {"Comma", 0xBC},
    {"Period", 0xBE}, {"Slash", 0xBF}, {"Backslash", 0xDC}, {"Semicolon", 0xBA}, {"Quote", 0xDE},
    {"LeftBracket", 0xDB}, {"RightBracket", 0xDD},
};

bool equal_ignoring_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return s;
}

uint8_t parse_key_name(std::string_view name) {
    if (name.size() == 1) {
        char c = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return static_cast<uint8_t>(c);
    }
    // F1-F24: at most two digits (checked first, so a long run of digits can't overflow n).
    if (name.size() >= 2 && name.size() <= 3 && (name[0] == 'F' || name[0] == 'f')) {
        int n = 0;
        bool digits = true;
        for (char c : name.substr(1)) {
            if (c < '0' || c > '9') digits = false;
            else n = n * 10 + (c - '0');
        }
        if (digits && n >= 1 && n <= 24) return static_cast<uint8_t>(kVkF1 + n - 1);
    }
    if (name.size() == 4 && equal_ignoring_case(name.substr(0, 3), "Num") && name[3] >= '0' && name[3] <= '9') {
        return static_cast<uint8_t>(kVkNumpad0 + (name[3] - '0'));
    }
    for (const Named& n : kNamed) {
        if (equal_ignoring_case(name, n.name)) return n.vk;
    }
    for (const Named& n : kAliases) {
        if (equal_ignoring_case(name, n.name)) return n.vk;
    }
    return 0;
}

bool valid_id(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok) return false;
    }
    return true;
}

} // namespace

bool is_modifier(uint8_t vk) {
    return vk == kVkShift || vk == kVkControl || vk == kVkMenu || (vk >= kVkLShift && vk <= kVkRMenu);
}

std::string key_name(uint8_t vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, static_cast<char>(vk));
    if (vk >= kVkF1 && vk < kVkF1 + 24) return "F" + std::to_string(vk - kVkF1 + 1);
    if (vk >= kVkNumpad0 && vk <= kVkNumpad0 + 9) return "Num" + std::to_string(vk - kVkNumpad0);
    for (const Named& n : kNamed) {
        if (n.vk == vk) return n.name;
    }
    return {};
}

bool parse(std::string_view text, Key& out) {
    text = trim(text);
    if (text.empty() || equal_ignoring_case(text, "None")) {
        out = {};
        return true;
    }
    Key key;
    // Modifiers, each followed by '+', then the key ("Ctrl+Plus" for '=').
    size_t start = 0;
    for (;;) {
        size_t plus = text.find('+', start);
        std::string_view part = trim(text.substr(start, plus == std::string_view::npos ? std::string_view::npos
                                                                                       : plus - start));
        bool last = plus == std::string_view::npos;
        if (!last) {
            if (equal_ignoring_case(part, "Ctrl") || equal_ignoring_case(part, "Control")) key.mods |= kCtrl;
            else if (equal_ignoring_case(part, "Shift")) key.mods |= kShift;
            else if (equal_ignoring_case(part, "Alt")) key.mods |= kAlt;
            else return false;
            start = plus + 1;
            continue;
        }
        key.vk = parse_key_name(part);
        if (!key.vk) return false;
        break;
    }
    out = key;
    return true;
}

std::string format(Key key) {
    if (!key.bound()) return "None";
    std::string out;
    if (key.mods & kCtrl) out += "Ctrl+";
    if (key.mods & kShift) out += "Shift+";
    if (key.mods & kAlt) out += "Alt+";
    std::string name = key_name(key.vk);
    if (name.empty()) {
        char buf[8];
        snprintf(buf, sizeof(buf), "0x%02X", key.vk);
        name = buf;
    }
    return out + name;
}

CustomKeys parse_custom_keys(std::string_view text) {
    CustomKeys out;
    std::string section;
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);  // UTF-8 BOM
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        std::string_view line = text.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
        pos = end == std::string_view::npos ? text.size() : end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\t')) line.remove_suffix(1);
        line = trim(line);
        if (line.empty() || line.substr(0, 2) == "//") continue;
        if (line.front() == '[' && line.back() == ']') {
            section = std::string(line);
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        std::string_view name = trim(line.substr(0, eq));
        if (name.size() < 6 || !equal_ignoring_case(name.substr(name.size() - 6), "hotkey")) continue;
        std::string_view values = line.substr(eq + 1);
        size_t start = 0;
        for (;;) {
            size_t comma = values.find(',', start);
            std::string_view value =
                trim(values.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
            uint8_t vk = 0;
            if (value.size() == 1) {
                char c = static_cast<char>(std::toupper(static_cast<unsigned char>(value[0])));
                if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) vk = static_cast<uint8_t>(c);
            } else if (!value.empty() && value.size() <= 3) {
                int n = 0;
                bool digits = true;
                for (char c : value) {
                    if (c < '0' || c > '9') digits = false;
                    else n = n * 10 + (c - '0');
                }
                if (digits && n > 0 && n < 256) vk = static_cast<uint8_t>(n);  // a key code; 512 means none
            }
            if (vk && !out.keys.count(vk)) out.keys[vk] = section;
            if (comma == std::string_view::npos) break;
            start = comma + 1;
        }
    }
    return out;
}

std::string game_use(Key key, const CustomKeys* custom) {
    if (!key.bound()) return {};
    const uint8_t vk = key.vk;
    const uint8_t mods = key.mods;
    if (mods == 0 && custom) {
        auto it = custom->keys.find(vk);
        if (it != custom->keys.end()) return "CustomKeys.txt " + it->second;
    }
    if (vk >= '0' && vk <= '9' && (mods == 0 || mods == kCtrl || mods == kShift)) return "control groups";
    if (mods == 0) {
        if (vk >= kVkF1 && vk <= kVkF1 + 4) return "hero selection";
        if (vk == kVkF1 + 8) return "the quest log";       // F9
        if (vk == kVkF1 + 9) return "the menu";            // F10
        if (vk == kVkF1 + 10) return "the allies dialog";  // F11
        if (vk == kVkF1 + 11) return "the chat log";       // F12
        switch (vk) {
        case kVkEscape: return "the menu and cancelling";
        case kVkReturn: return "chat";
        case kVkSpace: return "jumping to the last event";
        case kVkBack: return "jumping to the town hall";
        case kVkTab: return "subgroups";
        case kVkInsert: case kVkDelete: case kVkPrior: case kVkNext: return "the camera";
        case kVkLeft: case kVkUp: case kVkRight: case kVkDown: return "scrolling";
        case kVkSnapshot: return "screenshots";
        default: break;
        }
        if (vk >= 'A' && vk <= 'Z') return "command card hotkeys";
    }
    if (mods == kAlt) {
        switch (vk) {
        case 'A': return "ally colors";
        case 'Q': return "quitting the game";
        case kVkF1 + 3: return "closing the window";  // Alt+F4
        default: break;
        }
    }
    if (mods == kCtrl && vk == 'S') return "turning sound off";
    if (mods == kCtrl && vk == 'M') return "turning music off";
    return {};
}

bool parse_declared(const json::Value& list, std::vector<Declared>& out, std::vector<std::string>& errors) {
    size_t errors_before = errors.size();
    if (!list.is_array()) {
        errors.push_back("\"keybinds\" must be a list");
        return false;
    }
    std::vector<Declared> result;
    for (size_t i = 0; i < list.items().size(); ++i) {
        const json::Value& item = list.items()[i];
        std::string where = "\"keybinds\" entry " + std::to_string(i + 1);
        if (!item.is_object()) {
            errors.push_back(where + " must be an object");
            continue;
        }
        Declared d;
        const json::Value* id = item.find("id");
        if (!id || !id->is_string() || !valid_id(id->as_string())) {
            errors.push_back(where + ": \"id\" must be 1-64 lowercase letters, digits, '-', '_' or '.'");
            continue;
        }
        d.id = id->as_string();
        where = "keybind \"" + d.id + "\"";
        for (const Declared& other : result) {
            if (other.id == d.id) errors.push_back(where + " is listed twice");
        }
        const json::Value* label = item.find("label");
        if (label && !label->is_string()) errors.push_back(where + ": \"label\" must be a string");
        d.label = label && label->is_string() ? label->as_string() : d.id;
        const json::Value* def = item.find("default");
        if (def && (!def->is_string() || !parse(def->as_string(), d.key))) {
            errors.push_back(where + ": \"default\" isn't a key like \"Ctrl+H\"");
        }
        result.push_back(std::move(d));
    }
    if (errors.size() != errors_before) return false;
    out = std::move(result);
    return true;
}

void apply_overrides(std::vector<Binding>& bindings, const json::Value& stored) {
    for (Binding& b : bindings) {
        b.key = b.fallback;
        const json::Value* mod = stored.is_object() ? stored.find(b.mod) : nullptr;
        const json::Value* value = mod && mod->is_object() ? mod->find(b.id) : nullptr;
        Key key;
        if (value && value->is_string() && parse(value->as_string(), key)) b.key = key;
    }
}

json::Value overrides(const std::vector<Binding>& bindings, const json::Value& previous) {
    json::Value root = json::Value::object();
    if (previous.is_object()) {
        for (const auto& [mod, entries] : previous.members()) {
            bool installed = false;
            for (const Binding& b : bindings) installed |= b.mod == mod;
            if (!installed && entries.is_object()) root.set(mod, entries);
        }
    }
    for (const Binding& b : bindings) {
        if (b.key == b.fallback) continue;
        json::Value entries = root.find(b.mod) ? *root.find(b.mod) : json::Value::object();
        entries.set(b.id, json::Value::string(format(b.key)));
        root.set(b.mod, std::move(entries));
    }
    return root;
}

void find_conflicts(std::vector<Binding>& bindings, const CustomKeys* custom) {
    for (size_t i = 0; i < bindings.size(); ++i) {
        Binding& b = bindings[i];
        b.conflict.clear();
        if (!b.key.bound()) continue;
        for (size_t j = 0; j < bindings.size(); ++j) {
            const Binding& other = bindings[j];
            if (i == j || !(other.key == b.key)) continue;
            b.conflict = "also " + (other.mod_name.empty() ? other.mod : other.mod_name) + ": " + other.label;
            break;
        }
        if (b.conflict.empty()) {
            std::string use = game_use(b.key, custom);
            if (!use.empty()) b.conflict = "game: " + use;
        }
    }
}

} // namespace tome::keys
