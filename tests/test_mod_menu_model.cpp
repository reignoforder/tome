#include "core/mod_menu_model.h"
#include "test.h"

using namespace tome;
using namespace tome::menu;

namespace {

ModStatus mod(const char* id, bool running) {
    ModStatus m;
    m.folder = id;
    m.id = id;
    m.name = std::string("Mod ") + id;
    m.version = "1.0.0";
    m.user_enabled = running;
    m.running = running;
    m.state = running ? "active" : "disabled";
    m.sync = "client";
    m.reload = "live";
    return m;
}

Data data_with(int count) {
    Data d;
    for (int i = 0; i < count; ++i) d.mods.push_back(mod(("m" + std::to_string(i)).c_str(), i % 2 == 0));
    d.tome = {"0.1.0", "1.27b (1.27.1.7085)", "C:\\Games\\WarCraft III\\mods", ""};
    return d;
}

bool contains(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

ModStatus with_settings() {
    ModStatus m = mod("cfg", true);
    json::Value schema;
    json::parse(R"({"properties": {"on": {"type": "boolean", "default": true},
                                     "n": {"type": "integer", "minimum": 0, "maximum": 3},
                                     "text": {"type": "string"},
                                     "limit": {"type": "integer", "minimum": 30, "maximum": 1000,
                                               "default": 250, "input": "text",
                                               "description": "A long description that goes on and on and on, far past what three lines of the menu's small text can hold, so the menu cuts it short with an ellipsis at the end"}}})",
                schema);
    std::vector<std::string> errors, warnings;
    settings::parse_schema(schema, m.settings, errors, warnings);
    m.settings_values = settings::load_values(m.settings, json::Value(), warnings);
    return m;
}

} // namespace

TEST(menu_list_pages) {
    Data d = data_with(5);
    State s;
    View v = build(s, d, 2);
    CHECK_EQ(v.title, std::string("Mods"));
    CHECK(contains(v.summary, "5 installed, 3 active"));
    CHECK_EQ(v.rows.size(), size_t(2));
    CHECK(contains(v.rows[0].name, "Mod m0"));
    CHECK_EQ(v.rows[0].button, std::string("Disable"));
    CHECK_EQ(v.rows[1].button, std::string("Enable"));
    CHECK_EQ(v.rows[0].small, std::string("Details"));
    CHECK_EQ(v.footer[0], std::string("Previous"));
    CHECK_EQ(v.footer[3], std::string("Done"));

    click_footer(s, d, 2, 1);  // Next
    CHECK_EQ(s.list_first, 2);
    click_footer(s, d, 2, 1);
    CHECK_EQ(build(s, d, 2).rows.size(), size_t(1));  // the last page has one
    click_footer(s, d, 2, 1);
    CHECK_EQ(s.list_first, 0);  // wraps
    click_footer(s, d, 2, 0);
    CHECK_EQ(s.list_first, 4);

    Action a = click_row(s, d, 2, 0, false);
    CHECK(a.kind == Action::SetEnabled);
    CHECK_EQ(a.mod, std::string("m4"));
    CHECK(!a.flag);
    CHECK(click_footer(s, d, 2, 3).kind == Action::Close);

    Data empty = data_with(0);
    View e = build(State{}, empty, 2);
    CHECK_EQ(e.rows.size(), size_t(1));
    CHECK(e.rows[0].button.empty());
    CHECK(e.footer[0].empty());  // one page: no Previous/Next
}

TEST(menu_details_show_problems) {
    Data d = data_with(1);
    ModStatus& m = d.mods[0];
    m.running = false;
    m.state = "error";
    m.reason = "crashed in native Boom: access violation reading 0x00000000 at crash.dll+0x1234";
    m.crash_calls = "crash.dll+0x1300 <- game.dll+0x2000";
    m.warnings = {"unknown field \"x\" (ignored)"};
    m.description = "Does things.";
    m.authors = {"A", "B"};
    keys::Binding b;
    b.mod = "m0";
    b.label = "Toggle";
    keys::parse("Ctrl+H", b.key);
    d.keybinds.push_back(b);

    State s;
    click_row(s, d, 4, 0, true);  // Details
    CHECK(s.page == Page::Details);
    View v = build(s, d, 4);
    CHECK(v.rows.empty());
    CHECK(contains(v.body, "Does things."));
    CHECK(contains(v.body, "A, B"));
    CHECK(contains(v.body, "access violation reading 0x00000000 at crash.dll+0x1234"));
    CHECK(contains(v.body, "called from (likely): crash.dll+0x1300"));
    CHECK(contains(v.body, "unknown field"));
    CHECK(contains(v.body, "Toggle (Ctrl+H)"));
    CHECK_EQ(v.footer[0], std::string("Disable"));  // user_enabled
    CHECK(v.footer[1].empty());                      // no settings
    click_footer(s, d, 4, 3);
    CHECK(s.page == Page::List);

    // A folder without a usable tome.mod.json: details, no toggle.
    ModStatus broken;
    broken.folder = "broken";
    broken.name = "broken";
    broken.state = "error";
    broken.reason = "tome.mod.json: line 1";
    broken.errors = {"tome.mod.json: line 1, column 2: bad"};
    d.mods.push_back(broken);
    View list = build(s, d, 4);
    CHECK(list.rows[1].button.empty());
    click_row(s, d, 4, 1, true);
    View details = build(s, d, 4);
    CHECK(contains(details.body, "column 2: bad"));
    CHECK(details.footer[0].empty());
}

TEST(menu_settings_page) {
    Data d = data_with(0);
    d.mods.push_back(with_settings());
    State s;
    click_row(s, d, 8, 0, true);
    CHECK_EQ(build(s, d, 8).footer[1], std::string("Settings"));
    click_footer(s, d, 8, 1);
    CHECK(s.page == Page::Settings);
    // Two rows per setting: the second leaves room for the description.
    View v = build(s, d, 8);
    CHECK_EQ(v.rows.size(), size_t(8));
    CHECK_EQ(v.rows[0].button, std::string("On"));
    CHECK(v.rows[1].name.empty() && v.rows[1].button.empty() && !v.rows[1].editable);
    CHECK_EQ(v.rows[2].small, std::string("-"));
    CHECK_EQ(v.rows[2].button, std::string("+"));
    CHECK(v.rows[4].button.empty());  // free text: a text box
    CHECK(v.rows[4].editable);
    CHECK(!v.rows[0].editable);
    CHECK(edit_row(s, d, 8, 4, "").kind == Action::None);  // unchanged
    Action typed = edit_row(s, d, 8, 4, "hi");
    CHECK(typed.kind == Action::SetSetting);
    CHECK_EQ(typed.key, std::string("text"));
    CHECK_EQ(typed.value.as_string(), std::string("hi"));
    CHECK(edit_row(s, d, 8, 0, "x").kind == Action::None);  // not a text setting
    CHECK(edit_row(s, d, 8, 5, "x").kind == Action::None);  // a spare row

    // "input": "text": a number typed into a box.
    CHECK(v.rows[6].editable && v.rows[6].button.empty() && v.rows[6].small.empty());
    CHECK_EQ(v.rows[6].edit, std::string("250"));
    CHECK(v.rows[6].info.size() <= 150 && contains(v.rows[6].info, "..."));
    CHECK(edit_row(s, d, 8, 6, "250").kind == Action::None);   // unchanged
    CHECK(edit_row(s, d, 8, 6, "fast").kind == Action::None);  // not a number
    typed = edit_row(s, d, 8, 6, " 144 ");
    CHECK(typed.kind == Action::SetSetting && typed.key == "limit" && typed.value.as_number() == 144.0);
    typed = edit_row(s, d, 8, 6, "5000");  // the manager clamps it
    CHECK(typed.kind == Action::SetSetting && typed.value.as_number() == 5000.0);
    CHECK(click_row(s, d, 8, 6, false).kind == Action::None);  // no buttons

    Action a = click_row(s, d, 8, 0, false);
    CHECK(a.kind == Action::SetSetting);
    CHECK_EQ(a.mod, std::string("cfg"));
    CHECK_EQ(a.key, std::string("on"));
    CHECK(!a.value.as_bool());
    a = click_row(s, d, 8, 2, false);
    CHECK_EQ(a.value.as_number(), 1.0);
    a = click_row(s, d, 8, 2, true);
    CHECK_EQ(a.value.as_number(), 0.0);  // clamped at the minimum
    CHECK(click_row(s, d, 8, 1, false).kind == Action::None);  // a spare row
    CHECK(click_row(s, d, 8, 4, false).kind == Action::None);
    CHECK(click_footer(s, d, 8, 2).kind == Action::ResetSettings);

    // Four rows: two settings per page.
    v = build(s, d, 4);
    CHECK_EQ(v.rows.size(), size_t(4));
    CHECK_EQ(v.footer[1], std::string("Next"));
    click_footer(s, d, 4, 1);
    CHECK_EQ(s.first, 2);
    v = build(s, d, 4);
    CHECK(v.rows[0].editable && v.rows[2].editable);  // text, then limit
    CHECK_EQ(click_row(s, d, 4, 2, false).kind, Action::None);
    click_footer(s, d, 4, 3);
    CHECK(s.page == Page::Details);
}

TEST(menu_keybinds_capture) {
    Data d = data_with(1);
    keys::Binding b;
    b.mod = "m0";
    b.mod_name = "Mod m0";
    b.id = "toggle";
    b.label = "Toggle";
    keys::parse("Ctrl+H", b.fallback);
    b.key = b.fallback;
    d.keybinds.push_back(b);

    State s;
    click_footer(s, d, 4, 2);  // Tome
    CHECK(s.page == Page::Tome);
    View tome = build(s, d, 4);
    CHECK(contains(tome.body, "1.27b"));
    CHECK_EQ(tome.footer[0], std::string("Safe Mode"));
    Action safe = click_footer(s, d, 4, 0);
    CHECK(safe.kind == Action::SetSafeMode);
    CHECK(safe.flag);
    click_footer(s, d, 4, 1);  // Keybinds
    CHECK(s.page == Page::Keybinds);
    CHECK_EQ(build(s, d, 4).rows[0].button, std::string("Ctrl+H"));
    CHECK(build(s, d, 4).rows[0].small.empty());  // the default: nothing to reset

    Action a;
    CHECK(!key_pressed(s, d, keys::Key{0, 'J'}, a));  // not capturing
    click_row(s, d, 4, 0, false);
    CHECK_EQ(s.capture, 0);
    CHECK_EQ(build(s, d, 4).rows[0].button, std::string("..."));
    CHECK(key_pressed(s, d, keys::Key{0, 0x11}, a));  // Ctrl alone: keep waiting
    CHECK_EQ(s.capture, 0);
    CHECK(key_pressed(s, d, keys::Key{keys::kCtrl, 'J'}, a));
    CHECK(a.kind == Action::SetKey);
    CHECK_EQ(keys::format(a.key_value), std::string("Ctrl+J"));
    CHECK_EQ(s.capture, -1);

    click_row(s, d, 4, 0, false);
    Action none;
    CHECK(key_pressed(s, d, keys::Key{0, 0x1B}, none));  // Esc cancels
    CHECK(none.kind == Action::None);
    click_row(s, d, 4, 0, false);
    Action clear;
    CHECK(key_pressed(s, d, keys::Key{0, 0x08}, clear));  // Backspace clears
    CHECK(!clear.key_value.bound());

    d.keybinds[0].key = keys::Key{keys::kCtrl, 'J'};
    CHECK_EQ(build(s, d, 4).rows[0].small, std::string("Reset"));
    Action reset = click_row(s, d, 4, 0, true);
    CHECK(reset.kind == Action::SetKey);
    CHECK(reset.key_value == d.keybinds[0].fallback);
    CHECK_EQ(reset.mod, std::string("m0"));
    CHECK_EQ(reset.key, std::string("toggle"));

    // A binding is added in front while one waits for a key: the key still goes to the clicked one.
    click_row(s, d, 4, 0, false);
    keys::Binding other = d.keybinds[0];
    other.mod = "a-new-mod";
    d.keybinds.insert(d.keybinds.begin(), other);
    clamp(s, d, 4);
    CHECK_EQ(s.capture, 1);
    Action moved;
    CHECK(key_pressed(s, d, keys::Key{keys::kCtrl, 'K'}, moved));
    CHECK_EQ(moved.mod, std::string("m0"));
    d.keybinds.erase(d.keybinds.begin());
    CHECK(click_footer(s, d, 4, 2).kind == Action::ResetKeys);
    click_footer(s, d, 4, 3);
    CHECK(s.page == Page::Tome);
}

TEST(menu_back) {
    Data d = data_with(2);
    d.mods[0].icon = "icon.blp";
    State s;
    CHECK_EQ(build(s, d, 4).rows[0].icon, std::string("m0"));
    CHECK(build(s, d, 4).rows[1].icon.empty());
    click_row(s, d, 4, 0, true);
    CHECK(back(s).kind == Action::None);
    CHECK(s.page == Page::List);
    CHECK(back(s).kind == Action::Close);
    click_footer(s, d, 4, 2);  // Tome
    click_footer(s, d, 4, 1);  // Keybinds
    s.capture = 0;
    back(s);
    CHECK(s.page == Page::Keybinds);  // the first Esc only cancels the capture
    back(s);
    CHECK(s.page == Page::Tome);
}

TEST(menu_clamp_after_changes) {
    Data d = data_with(5);
    State s;
    s.list_first = 4;
    click_row(s, d, 2, 0, true);
    CHECK(s.page == Page::Details);
    d.mods.resize(2);  // the mod was removed
    clamp(s, d, 2);
    CHECK(s.page == Page::List);
    CHECK_EQ(s.list_first, 0);
}
