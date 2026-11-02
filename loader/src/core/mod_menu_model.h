// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the mod menu shows and what its buttons do, apart from the frames
// that show it (mod_menu.cpp), so it can be unit-tested. The main menu and
// the in-game Esc menu use the same pages with their own frames.
//
// Each page fills the same generic layout: a title and a summary line,
// either rows (a name, an info line under it, a small button and a button
// on the right) or a block of text, and four footer buttons. An empty label
// hides that button.
//
//   List      every mod: Details, Enable/Disable; footer Previous, Next, Tome, Done
//   Details   one mod's metadata and problems; footer Enable/Disable, Settings, Back
//   Settings  generated from the mod's config.schema.json, two rows per setting (the second
//             leaves room for the description); footer Previous, Next, Defaults, Back
//   Tome      versions, mods folder, safe mode; footer Safe Mode, Settings, Mods Folder, Back
//   TomeSettings  Tome's own settings (core/tome_settings.h), laid out like a mod's;
//             footer Previous, Next, Keybinds, Back
//   Keybinds  every mod's keybinds with conflicts; footer Previous, Next, Defaults, Back

#include "json.h"
#include "keys.h"
#include "mod_status.h"
#include "popup.h"

#include <string>
#include <vector>

namespace tome::menu {

enum class Page { List, Details, Settings, Tome, TomeSettings, Keybinds };

constexpr int kFooterButtons = 4;

struct Row {
    std::string name;
    std::string info;
    std::string small;   // label of the small button; "" hides it
    std::string button;  // label of the button on the right; "" hides it
    bool editable = false;  // a text box (with `edit` in it) instead of the buttons
    std::string edit;
    std::string icon;    // List: the mod's folder if it has an icon, else ""
};

struct View {
    std::string title;
    std::string summary;
    std::string body;  // Details and Tome: text instead of rows
    std::vector<Row> rows;
    std::string footer[kFooterButtons];
};

struct TomeInfo {
    std::string version;     // Tome's
    std::string game_build;  // e.g. "1.27b (1.27.1.7085)"
    std::string mods_dir;
    std::string safe_mode;   // why safe mode is on; "" if it isn't
    settings::Schema settings;    // Tome's own (core/tome_settings.h)
    json::Value settings_values;  // ...and their values
};

struct Data {
    std::vector<ModStatus> mods;
    std::vector<keys::Binding> keybinds;  // with conflicts filled in
    TomeInfo tome;
};

struct State {
    Page page = Page::List;
    int list_first = 0;  // first mod shown on the list
    int first = 0;       // first setting or keybind shown
    std::string mod;     // Details and Settings: the mod's folder (ids may be missing)
    int capture = -1;    // Keybinds: the row (index into Data::keybinds) waiting for a key, or -1
    // ...and that binding's mod and id: the list may change while it waits.
    std::string capture_mod, capture_id;
};

// What a click asks the caller to do, besides the navigation build() shows.
struct Action {
    enum Kind {
        None,
        Close,          // close the menu
        SetEnabled,     // mod, flag
        SetSetting,     // mod, key, value
        ResetSettings,  // mod
        SetKey,         // mod, key (the keybind's id), key_value
        ResetKeys,      // every keybind back to its default
        SetSafeMode,    // flag
        SetTomeSetting, // key, value: one of Tome's own settings
        OpenModsFolder,
    };
    Kind kind = None;
    std::string mod;
    std::string key;
    json::Value value;
    bool flag = false;
    keys::Key key_value;
};

View build(const State& state, const Data& data, int rows);

// Settings or TomeSettings: a page of settings, with text boxes.
bool is_settings_page(Page page);

// The text box of row `row` was confirmed (Enter, or leaving the page)
// with `text`: a free-text setting, or a number typed in ("input": "text").
// Action::None if the value didn't change or the text isn't a number. A mod's
// setting gives SetSetting, one of Tome's own SetTomeSetting.
Action edit_row(State& state, const Data& data, int rows, int row, const std::string& text);

// Esc: back to the page this one came from; Action::Close on the list.
// While a keybind waits for a key, only cancels that.
Action back(State& state);

// A click on row `row` (0-based on the page): its small button or its
// main button.
Action click_row(State& state, const Data& data, int rows, int row, bool small);
Action click_footer(State& state, const Data& data, int rows, int button);

// While a keybind waits for a key: Esc cancels, Backspace unbinds, other
// keys (not modifiers on their own) become the binding. True if the menu
// took the key.
bool key_pressed(State& state, const Data& data, keys::Key key, Action& action);

// After a toggle: the names of the mods whose changes wait for a restart
// now and didn't before.
std::vector<std::string> newly_restart_pending(const std::vector<ModStatus>& before,
                                               const std::vector<ModStatus>& after);

// The popup offering that restart: option kRestartNow or kRestartLater.
// `in_game` says that restarting leaves the game.
constexpr int kRestartNow = 0, kRestartLater = 1;
popup::Spec restart_prompt(const std::vector<std::string>& names, bool in_game);

// After the data changed: keeps the state pointing at something that exists
// (a removed mod goes back to the list, pages stay in range).
void clamp(State& state, const Data& data, int rows);

} // namespace tome::menu
