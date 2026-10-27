#pragma once

// Keybinds (docs/research.md §8.3): key names like "Ctrl+Shift+H", the
// bindings mods declare in tome.mod.json "keybinds", the player's changes
// (tome/keybinds.json) and conflicts between them and with the game.

#include "json.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace tome::keys {

enum Modifier : uint8_t { kCtrl = 1, kShift = 2, kAlt = 4 };

// A key with modifiers. `vk` is a Windows virtual-key code; 0 means unbound.
struct Key {
    uint8_t mods = 0;
    uint8_t vk = 0;

    bool bound() const { return vk != 0; }
    bool operator==(const Key&) const = default;
};

// "Ctrl+H", "Alt+Shift+F5", "Num7", "PageUp", ... Modifiers in any order,
// names ignore case. "" and "None" are unbound. False for anything else
// (including a modifier on its own).
bool parse(std::string_view text, Key& out);

// The canonical name: modifiers as Ctrl, Shift, Alt, then the key. "None"
// for an unbound key.
std::string format(Key key);

// The key of a virtual-key code, or "" if Tome has no name for it (those
// can't be bound).
std::string key_name(uint8_t vk);

// Ctrl, Shift, Alt and their left/right variants: never a key on their own.
bool is_modifier(uint8_t vk);

// The command card hotkeys a player set in CustomKeys.txt (next to
// war3.exe): each key with the first section ("[CmdMove]", "[AHbz]") that
// uses it in a Hotkey, Unhotkey or Researchhotkey line.
struct CustomKeys {
    std::map<uint8_t, std::string> keys;
};
CustomKeys parse_custom_keys(std::string_view text);

// What the game uses this key for, as far as Tome knows (control groups,
// the F-key menus, camera keys, command card hotkeys, with `custom`'s
// sections for the keys it lists), or "" if nothing.
std::string game_use(Key key, const CustomKeys* custom = nullptr);

// One binding a mod declared.
struct Declared {
    std::string id;     // unique within the mod
    std::string label;
    Key key;            // the default
};

// Parses tome.mod.json "keybinds". False with errors for a bad list.
bool parse_declared(const json::Value& list, std::vector<Declared>& out, std::vector<std::string>& errors);

struct Binding {
    std::string mod;       // mod id
    std::string mod_name;
    std::string id;
    std::string label;
    Key fallback;          // what the mod declared
    Key key;               // what's in use: the player's choice, else the fallback
    std::string conflict;  // filled by find_conflicts; "" if none
};

// Applies the player's choices from tome/keybinds.json:
// { "<mod id>": { "<keybind id>": "Ctrl+J" } }. Unknown or unparsable
// entries are ignored.
void apply_overrides(std::vector<Binding>& bindings, const json::Value& stored);

// tome/keybinds.json for `bindings`: only keys that differ from their
// default. Entries in `previous` for mods not in `bindings` (not installed
// right now) are kept.
json::Value overrides(const std::vector<Binding>& bindings, const json::Value& previous);

// Fills each binding's `conflict`: another binding with the same key
// ("also Mod: Label"), or the game's use of the key ("game: control groups").
void find_conflicts(std::vector<Binding>& bindings, const CustomKeys* custom = nullptr);

} // namespace tome::keys
