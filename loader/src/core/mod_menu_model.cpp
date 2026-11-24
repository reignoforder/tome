// SPDX-License-Identifier: GPL-3.0-or-later
#include "mod_menu_model.h"

#include <algorithm>
#include <cstdio>

namespace tome::menu {
namespace {

const char* const kGray = "|cff999999";
const char* const kGreen = "|cff7fff7f";
const char* const kRed = "|cffff6060";
const char* const kYellow = "|cffffcc00";
const char* const kWhite = "|cffffffff";
const char* const kEnd = "|r";

std::string colored(const char* color, const std::string& text) {
    return color + text + kEnd;
}

std::string ellipsize(const std::string& text, size_t max) {
    if (text.size() <= max) return text;
    size_t end = max - 3;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;  // UTF-8 boundary
    return text.substr(0, end) + "...";
}

std::string first_line(const std::string& text) {
    return text.substr(0, text.find('\n'));
}

int page_count(size_t items, int rows) {
    return items == 0 ? 1 : static_cast<int>((items + rows - 1) / rows);
}

// The Settings page gives each setting two rows, so its description has
// room for a few lines under the name. A description is cut to about that.
constexpr int kSettingRows = 2;
constexpr size_t kSettingInfoMax = 150;

} // namespace

bool is_settings_page(Page page) {
    return page == Page::Settings || page == Page::TomeSettings;
}

namespace {

// Items (mods, settings, keybinds) on a page of `rows` rows.
int per_page(const State& state, int rows) {
    return is_settings_page(state.page) ? std::max(1, rows / kSettingRows) : rows;
}

// The item shown on row `row` of the page (from its first one), or -1 for
// a row that only gives the one above it room.
int item_on_row(const State& state, int row) {
    if (!is_settings_page(state.page)) return row;
    return row % kSettingRows == 0 ? row / kSettingRows : -1;
}

const ModStatus* find_mod(const Data& data, const std::string& folder) {
    if (folder.empty()) return nullptr;
    for (const ModStatus& m : data.mods) {
        if (m.folder == folder) return &m;
    }
    return nullptr;
}

// What a settings page shows: the mod's settings (Settings) or Tome's own
// (TomeSettings, mod null). Null schema and values on other pages and for a
// mod that's gone.
struct SettingsSource {
    const settings::Schema* schema = nullptr;
    const json::Value* values = nullptr;
    const ModStatus* mod = nullptr;
};

SettingsSource settings_source(const State& state, const Data& data) {
    if (state.page == Page::TomeSettings) return {&data.tome.settings, &data.tome.settings_values, nullptr};
    if (state.page != Page::Settings) return {};
    const ModStatus* m = find_mod(data, state.mod);
    return m ? SettingsSource{&m->settings, &m->settings_values, m} : SettingsSource{};
}

// The action that gives setting `key` of `source` (schema set) a new value.
Action set_setting(const SettingsSource& source, const std::string& key, json::Value value) {
    Action action;
    if (source.mod) {
        action.kind = Action::SetSetting;
        action.mod = source.mod->id;
    } else {
        action.kind = Action::SetTomeSetting;
    }
    action.key = key;
    action.value = std::move(value);
    return action;
}

// A mod whose tome.mod.json couldn't be used has nothing to turn on.
bool can_toggle(const ModStatus& m) {
    return !m.id.empty() && m.errors.empty();
}

std::string display_name(const ModStatus& m) {
    return m.name.empty() ? m.folder : m.name;
}

std::string status_line(const ModStatus& m) {
    std::string info;
    if (m.running) {
        info = colored(kGreen, "Active");
    } else if (m.state == "disabled") {
        info = colored("|cffa0a0a0", m.reason.empty() ? "Disabled" : "Disabled (" + m.reason + ")");
    } else if (m.state == "waiting") {
        info = colored(kYellow, "Waiting (" + m.reason + ")");
    } else if (m.state == "approval") {
        const bool changed = m.approval == "changed";
        if (m.approval == "signer") info = colored(kRed, "Signer changed: not allowed yet");
        else if (m.code_kind == "assets") info = colored(kYellow, changed ? "Files changed: not allowed yet" : "New files: not allowed yet");
        else info = colored(kYellow, changed ? "Code changed: not allowed yet" : "New code: not allowed yet");
    } else {
        info = colored(kRed, ellipsize(first_line(m.reason.empty() ? m.state : m.reason), 60));
    }
    if (!m.assets_pending.empty() && (m.running || m.state == "disabled")) {
        info += "  " + colored(kYellow, "(" + m.assets_pending + ")");
    }
    if (m.running && !m.locked.empty()) info += "  " + colored(kYellow, "(" + m.locked + ")");
    return info;
}

// A row with text only, for an empty list.
Row message_row(const char* name, const char* info) {
    Row r;
    r.name = name;
    r.info = info;
    return r;
}

std::string join(const std::vector<std::string>& items, const char* separator) {
    std::string out;
    for (const std::string& item : items) out += (out.empty() ? "" : separator) + item;
    return out;
}

std::string details_body(const ModStatus& m, const Data& data) {
    std::string body;
    auto line = [&](const std::string& text) { body += text + "\n"; };
    if (!m.description.empty()) {
        line(m.description);
        line("");
    }
    if (!m.authors.empty()) line(colored(kGray, "Authors: ") + join(m.authors, ", "));
    line(colored(kGray, "Id: ") + (m.id.empty() ? "?" : m.id) + colored(kGray, m.package ? "    Package: " : "    Folder: ") +
         "tome\\mods\\" + m.folder);
    if (m.sync == "required") line(colored(kGray, "Sync: ") + "required, every player needs it");
    else if (m.sync == "client") line(colored(kGray, "Sync: ") + "client, only you");
    if (m.reload == "live") line(colored(kGray, "Turning it on or off: ") + "applies at once");
    else if (m.reload == "map") line(colored(kGray, "Turning it on or off: ") + "applies at the next map");
    else if (m.reload == "restart") line(colored(kGray, "Turning it on or off: ") + "needs a restart");
    line(colored(kGray, "Status: ") + status_line(m));
    if (!m.code_hash.empty()) {
        line(colored(kGray, m.code_kind == "assets" ? "Files SHA-256: " : "Code SHA-256: ") + m.code_hash);
        line(colored(kGray, "Signed by: ") + (m.signer.empty() ? "not signed" : m.signer));
    }
    if (m.state == "approval") {
        line("");
        line(colored(kYellow, m.code_kind == "scripts"
                                  ? "This mod replaces game scripts, which run in every game. Enable it only if you "
                                    "trust where it came from."
                              : m.code_kind == "wasm"
                                  ? "This mod runs WebAssembly code in a sandbox, which can only use what Tome gives "
                                    "it, and the game reads its files with loaders that trust them. Enable it only if "
                                    "you trust where it came from."
                              : m.code_kind == "assets"
                                  ? "This mod adds game files (models, textures, sounds, UI), which the game's own "
                                    "loaders trust. Enable it only if you trust where it came from."
                                  : "This mod runs native code with full access to your computer. Enable it only if "
                                    "you trust where it came from."));
        if (m.approval == "signer") {
            line(colored(kRed, m.signer.empty()
                                   ? "The version you allowed was signed by its author; this one isn't signed at all."
                                   : "The version you allowed was signed by another key: this may not be from the "
                                     "same author."));
        }
    }

    if (m.running && m.profile.calls > 0) {
        const std::string time = profile::describe(m.profile);
        line(colored(kGray, "Time: ") + (m.profile.average_ms > profile::kSlowMs ? colored(kYellow, time) : time));
    }

    std::vector<std::string> keybinds;
    for (const keys::Binding& b : data.keybinds) {
        if (b.mod == m.id) keybinds.push_back(b.label + " (" + keys::format(b.key) + ")");
    }
    if (!keybinds.empty()) line(colored(kGray, "Keybinds: ") + join(keybinds, ", "));

    std::vector<std::string> problems;
    if (m.state == "error" && !m.reason.empty()) {
        if (m.errors.empty()) problems.push_back(colored(kRed, m.reason));
        for (const std::string& e : m.errors) problems.push_back(colored(kRed, e));
        if (!m.crash_calls.empty()) problems.push_back(colored(kRed, "  called from (likely): " + m.crash_calls));
    }
    if (!m.settings_error.empty()) problems.push_back(colored(kRed, "Settings: " + m.settings_error));
    for (const std::string& w : m.warnings) problems.push_back(colored(kYellow, w));
    if (!m.assets_pending.empty()) problems.push_back(colored(kYellow, m.assets_pending));
    if (!problems.empty()) {
        line("");
        line(colored(kWhite, "Problems"));
        for (const std::string& p : problems) line(p);
    }
    if (!body.empty()) body.pop_back();
    return body;
}

void build_list(const State& state, const Data& data, int rows, View& view) {
    size_t active = 0;
    for (const ModStatus& m : data.mods) active += m.running;
    view.title = "Mods";
    view.summary = std::to_string(data.mods.size()) + " installed, " + std::to_string(active) + " active";
    if (!data.tome.safe_mode.empty()) view.summary += "    " + colored(kYellow, "Safe mode");
    for (int row = 0; row < rows; ++row) {
        size_t index = static_cast<size_t>(state.list_first + row);
        if (index >= data.mods.size()) break;
        const ModStatus& m = data.mods[index];
        Row r;
        r.name = ellipsize(display_name(m), 40);
        if (!m.version.empty()) r.name += "  " + colored(kGray, m.version);
        r.info = status_line(m);
        if (!m.icon.empty()) r.icon = m.folder;
        r.small = "Details";
        if (can_toggle(m)) r.button = m.user_enabled ? "Disable" : "Enable";
        view.rows.push_back(std::move(r));
    }
    if (data.mods.empty()) {
        view.rows.push_back(message_row("No mods installed", "Put mods in the \"tome\\mods\" folder next to war3.exe."));
    }
    if (page_count(data.mods.size(), rows) > 1) {
        view.footer[0] = "Previous";
        view.footer[1] = "Next";
    }
    view.footer[2] = "Tome";
    view.footer[3] = "Done";
}

// A settings page's rows and its Previous and Next; the caller adds the rest.
void build_settings(const State& state, const SettingsSource& source, int rows, View& view) {
    view.title = "Settings";
    const std::vector<settings::Setting>& all = source.schema->settings;
    for (int item = 0; item < per_page(state, rows); ++item) {
        size_t index = static_cast<size_t>(state.first + item);
        if (index >= all.size()) break;
        const settings::Setting& s = all[index];
        const json::Value* found = source.values->find(s.key);
        const json::Value& value = found ? *found : s.fallback;
        std::string shown = settings::display(s, value);
        Row r;
        r.info = ellipsize(s.description, kSettingInfoMax);
        switch (s.type) {
        case settings::Type::Bool:
            r.name = s.title;
            r.button = shown;
            break;
        case settings::Type::Integer:
        case settings::Type::Number:
            if (s.text_input) {
                r.name = s.title;
                r.editable = true;
                r.edit = shown;
                if (r.info.empty()) r.info = colored(kGray, "Enter saves it");
            } else {
                r.name = s.title + ": " + colored(kWhite, shown);
                r.small = "-";
                r.button = "+";
            }
            break;
        case settings::Type::String:
            if (s.choices.empty()) {
                r.name = s.title;
                r.editable = true;
                r.edit = value.is_string() ? value.as_string() : std::string();
                if (r.info.empty()) r.info = colored(kGray, "Enter saves it");
            } else {
                r.name = s.title;
                r.button = shown;
            }
            break;
        }
        view.rows.push_back(std::move(r));
        for (int spare = 1; spare < kSettingRows; ++spare) view.rows.push_back(Row());
    }
    if (page_count(all.size(), per_page(state, rows)) > 1) {
        view.footer[0] = "Previous";
        view.footer[1] = "Next";
    }
}

void build_keybinds(const State& state, const Data& data, int rows, View& view) {
    size_t conflicts = 0;
    for (const keys::Binding& b : data.keybinds) conflicts += !b.conflict.empty();
    view.title = "Keybinds";
    view.summary = std::to_string(data.keybinds.size()) + (data.keybinds.size() == 1 ? " keybind" : " keybinds");
    if (conflicts) view.summary += ", " + colored(kYellow, std::to_string(conflicts) + " with conflicts");
    for (int row = 0; row < rows; ++row) {
        int index = state.first + row;
        if (index >= static_cast<int>(data.keybinds.size())) break;
        const keys::Binding& b = data.keybinds[static_cast<size_t>(index)];
        Row r;
        r.name = ellipsize(b.label, 40);
        r.info = colored(kGray, ellipsize(b.mod_name.empty() ? b.mod : b.mod_name, 30));
        if (!b.conflict.empty()) r.info += "  " + colored(kYellow, ellipsize(b.conflict, 40));
        r.button = keys::format(b.key);
        if (index == state.capture) {
            r.button = "...";
            r.info = colored(kWhite, "Press a key. Esc cancels, Backspace clears.");
        }
        if (!(b.key == b.fallback)) r.small = "Reset";
        view.rows.push_back(std::move(r));
    }
    if (data.keybinds.empty()) {
        view.rows.push_back(message_row("No keybinds", "Mods list theirs in tome.mod.json \"keybinds\"."));
    }
    if (page_count(data.keybinds.size(), rows) > 1) {
        view.footer[0] = "Previous";
        view.footer[1] = "Next";
    }
    view.footer[2] = "Defaults";
    view.footer[3] = "Back";
}

void build_tome(const Data& data, View& view) {
    view.title = "Tome";
    view.summary = "Version " + data.tome.version;
    std::string body;
    body += colored(kGray, "Tome: ") + data.tome.version + "\n";
    body += colored(kGray, "Game: ") + data.tome.game_build + "\n";
    body += colored(kGray, "Mods folder: ") + data.tome.mods_dir + "\n";
    if (!data.tome.updates.empty()) body += colored(kGray, "Updates: ") + data.tome.updates + "\n";
    body += "\n";
    if (data.tome.safe_mode.empty()) {
        body += colored(kGray, "Safe mode: ") + "off\n";
        body += "Safe Mode stops every mod for the rest of this session; you can still turn them on one at a "
                "time. Your choices in the mod list are kept.";
    } else {
        body += colored(kGray, "Safe mode: ") + colored(kYellow, "on (" + data.tome.safe_mode + ")") + "\n";
        body += "Mods only run if you turn them on during this session. Normal Mode starts the mods you had on.";
    }
    view.body = body;
    view.footer[0] = data.tome.safe_mode.empty() ? "Safe Mode" : "Normal Mode";
    view.footer[1] = "Settings";
    view.footer[2] = "Mods Folder";
    view.footer[3] = "Back";
}

int& first_of(State& state) {
    return state.page == Page::List ? state.list_first : state.first;
}

size_t item_count(const State& state, const Data& data) {
    switch (state.page) {
    case Page::List: return data.mods.size();
    case Page::Settings:
    case Page::TomeSettings: {
        SettingsSource source = settings_source(state, data);
        return source.schema ? source.schema->settings.size() : 0;
    }
    case Page::Keybinds: return data.keybinds.size();
    default: return 0;
    }
}

void turn_page(State& state, const Data& data, int rows, int direction) {
    const int items = per_page(state, rows);
    int pages = page_count(item_count(state, data), items);
    int page = first_of(state) / items;
    page = (page + (direction > 0 ? 1 : pages - 1)) % pages;
    first_of(state) = page * items;
    state.capture = -1;
}

void go(State& state, Page page) {
    state.page = page;
    state.first = 0;
    state.capture = -1;
}

} // namespace

View build(const State& state, const Data& data, int rows) {
    View view;
    const ModStatus* m = find_mod(data, state.mod);
    switch (state.page) {
    case Page::List: build_list(state, data, rows, view); break;
    case Page::Details:
        if (!m) break;
        view.title = ellipsize(display_name(*m), 40);
        view.summary = m->version;
        view.body = details_body(*m, data);
        if (can_toggle(*m)) view.footer[0] = m->user_enabled ? "Disable" : "Enable";
        if (!m->settings.settings.empty()) view.footer[1] = "Settings";
        view.footer[3] = "Back";
        break;
    case Page::Settings:
        if (!m) break;
        build_settings(state, settings_source(state, data), rows, view);
        view.summary = ellipsize(display_name(*m), 40);
        view.footer[2] = "Defaults";
        view.footer[3] = "Back";
        break;
    case Page::TomeSettings:
        build_settings(state, settings_source(state, data), rows, view);
        view.summary = "Tome";
        view.footer[2] = "Keybinds";
        view.footer[3] = "Back";
        break;
    case Page::Tome: build_tome(data, view); break;
    case Page::Keybinds: build_keybinds(state, data, rows, view); break;
    }
    return view;
}

Action click_row(State& state, const Data& data, int rows, int row, bool small) {
    Action action;
    if (row < 0 || row >= rows || item_on_row(state, row) < 0) return action;
    int index = first_of(state) + item_on_row(state, row);
    switch (state.page) {
    case Page::List: {
        if (index >= static_cast<int>(data.mods.size())) break;
        const ModStatus& m = data.mods[static_cast<size_t>(index)];
        if (small) {
            state.mod = m.folder;
            go(state, Page::Details);
        } else if (can_toggle(m)) {
            action.kind = Action::SetEnabled;
            action.mod = m.id;
            action.flag = !m.user_enabled;
        }
        break;
    }
    case Page::Settings:
    case Page::TomeSettings: {
        SettingsSource source = settings_source(state, data);
        if (!source.schema || index >= static_cast<int>(source.schema->settings.size())) break;
        const settings::Setting& s = source.schema->settings[static_cast<size_t>(index)];
        if (s.type == settings::Type::String && s.choices.empty()) break;
        if (s.text_input) break;
        const json::Value* found = source.values->find(s.key);
        action = set_setting(source, s.key, settings::step(s, found ? *found : s.fallback, small ? -1 : 1));
        break;
    }
    case Page::Keybinds: {
        if (index >= static_cast<int>(data.keybinds.size())) break;
        const keys::Binding& b = data.keybinds[static_cast<size_t>(index)];
        if (small) {
            state.capture = -1;
            action.kind = Action::SetKey;
            action.mod = b.mod;
            action.key = b.id;
            action.key_value = b.fallback;
        } else {
            state.capture = state.capture == index ? -1 : index;
            state.capture_mod = b.mod;
            state.capture_id = b.id;
        }
        break;
    }
    default: break;
    }
    return action;
}

Action edit_row(State& state, const Data& data, int rows, int row, const std::string& text) {
    Action action;
    if (!is_settings_page(state.page) || row < 0 || row >= rows || item_on_row(state, row) < 0) return action;
    SettingsSource source = settings_source(state, data);
    size_t index = static_cast<size_t>(state.first + item_on_row(state, row));
    if (!source.schema || index >= source.schema->settings.size()) return action;
    const settings::Setting& s = source.schema->settings[index];
    const json::Value* found = source.values->find(s.key);
    json::Value value;
    if (s.text_input) {
        // Untouched: the box still shows the value as display() wrote it,
        // possibly rounded, and saving that would replace the real value.
        if (text == settings::display(s, found ? *found : s.fallback)) return action;
        // Not a number: nothing changes, and the box shows the value again.
        if (!settings::parse_typed_number(text, value)) return action;
        json::Value coerced;
        std::string error;
        if (settings::coerce(s, value, coerced, error) && found && *found == coerced) return action;
    } else if (s.type == settings::Type::String && s.choices.empty()) {
        if (found && found->is_string() && found->as_string() == text) return action;
        value = json::Value::string(text);
    } else {
        return action;
    }
    return set_setting(source, s.key, std::move(value));
}

Action back(State& state) {
    Action action;
    if (state.capture >= 0) {
        state.capture = -1;
        return action;
    }
    switch (state.page) {
    case Page::List: action.kind = Action::Close; break;
    case Page::Details: go(state, Page::List); break;
    case Page::Settings: go(state, Page::Details); break;
    case Page::Tome: go(state, Page::List); break;
    case Page::TomeSettings: go(state, Page::Tome); break;
    case Page::Keybinds: go(state, Page::TomeSettings); break;
    }
    return action;
}

Action click_footer(State& state, const Data& data, int rows, int button) {
    Action action;
    const ModStatus* m = find_mod(data, state.mod);
    switch (state.page) {
    case Page::List:
        if (button == 0 || button == 1) turn_page(state, data, rows, button == 0 ? -1 : 1);
        else if (button == 2) go(state, Page::Tome);
        else if (button == 3) action.kind = Action::Close;
        break;
    case Page::Details:
        if (button == 0 && m && can_toggle(*m)) {
            action.kind = Action::SetEnabled;
            action.mod = m->id;
            action.flag = !m->user_enabled;
        } else if (button == 1 && m && !m->settings.settings.empty()) {
            go(state, Page::Settings);
        } else if (button == 3) {
            go(state, Page::List);
        }
        break;
    case Page::Settings:
        if (button == 0 || button == 1) turn_page(state, data, rows, button == 0 ? -1 : 1);
        else if (button == 2 && m) {
            action.kind = Action::ResetSettings;
            action.mod = m->id;
        } else if (button == 3) {
            go(state, Page::Details);
        }
        break;
    case Page::Tome:
        if (button == 0) {
            action.kind = Action::SetSafeMode;
            action.flag = data.tome.safe_mode.empty();
        } else if (button == 1) {
            go(state, Page::TomeSettings);
        } else if (button == 2) {
            action.kind = Action::OpenModsFolder;
        } else if (button == 3) {
            go(state, Page::List);
        }
        break;
    case Page::TomeSettings:
        if (button == 0 || button == 1) turn_page(state, data, rows, button == 0 ? -1 : 1);
        else if (button == 2) go(state, Page::Keybinds);
        else if (button == 3) go(state, Page::Tome);
        break;
    case Page::Keybinds:
        if (button == 0 || button == 1) turn_page(state, data, rows, button == 0 ? -1 : 1);
        else if (button == 2) {
            state.capture = -1;
            action.kind = Action::ResetKeys;
        } else if (button == 3) {
            go(state, Page::TomeSettings);
        }
        break;
    }
    return action;
}

bool key_pressed(State& state, const Data& data, keys::Key key, Action& action) {
    if (state.page != Page::Keybinds || state.capture < 0) return false;
    if (keys::is_modifier(key.vk)) return true;  // wait for the key itself
    state.capture = -1;
    // The binding that was clicked, wherever it is in the list now.
    bool found = false;
    for (const keys::Binding& b : data.keybinds) found |= b.mod == state.capture_mod && b.id == state.capture_id;
    if (!found) return true;
    constexpr uint8_t kVkEscape = 0x1B, kVkBack = 0x08;
    if (key.vk == kVkEscape && key.mods == 0) return true;
    if (keys::key_name(key.vk).empty()) return true;  // a key Tome can't name can't be saved either
    action.kind = Action::SetKey;
    action.mod = state.capture_mod;
    action.key = state.capture_id;
    action.key_value = key.vk == kVkBack && key.mods == 0 ? keys::Key{} : key;
    return true;
}

void clamp(State& state, const Data& data, int rows) {
    if ((state.page == Page::Details || state.page == Page::Settings) && !find_mod(data, state.mod)) {
        go(state, Page::List);
    }
    if (state.page == Page::Settings) {
        const ModStatus* m = find_mod(data, state.mod);
        if (m && m->settings.settings.empty()) go(state, Page::Details);
    }
    if (state.capture >= 0) {
        // Follow the binding if the list changed while it waits; drop it if it's gone.
        int index = -1;
        for (size_t i = 0; i < data.keybinds.size(); ++i) {
            const keys::Binding& b = data.keybinds[i];
            if (b.mod == state.capture_mod && b.id == state.capture_id) index = static_cast<int>(i);
        }
        state.capture = index;
    }
    const int items = per_page(state, rows);
    int pages = page_count(item_count(state, data), items);
    if (state.list_first >= page_count(data.mods.size(), rows) * rows) state.list_first = 0;
    if (state.page != Page::List && first_of(state) >= pages * items) first_of(state) = (pages - 1) * items;
}

std::vector<std::string> newly_restart_pending(const std::vector<ModStatus>& before,
                                               const std::vector<ModStatus>& after) {
    std::vector<std::string> names;
    for (const ModStatus& m : after) {
        if (!m.restart_pending) continue;
        bool was = false;
        for (const ModStatus& b : before) was |= b.folder == m.folder && b.restart_pending;
        if (!was) names.push_back(m.name.empty() ? m.folder : m.name);
    }
    return names;
}

popup::Spec restart_prompt(const std::vector<std::string>& names, bool in_game) {
    popup::Spec spec;
    spec.title = "Restart Required";
    std::string list;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i > 0) list += i + 1 == names.size() ? " and " : ", ";
        list += names[i];
    }
    spec.text = list + (names.size() == 1 ? " applies its" : " apply their") +
                " changes after Warcraft III restarts. Restart now?";
    if (in_game) spec.text += "\n\nRestarting leaves the game in progress.";
    spec.options = {"Restart Now", "Later"};
    spec.default_option = kRestartNow;
    spec.cancel_option = kRestartLater;
    return spec;
}

} // namespace tome::menu
