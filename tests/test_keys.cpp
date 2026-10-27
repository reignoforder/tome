#include "core/keys.h"
#include "test.h"

using namespace tome;
using namespace tome::keys;

namespace {

Key key(const char* text) {
    Key k;
    if (!parse(text, k)) test::fail(__FILE__, __LINE__, std::string("can't parse ") + text);
    return k;
}

Binding binding(const char* mod, const char* id, const char* fallback) {
    Binding b;
    b.mod = mod;
    b.mod_name = std::string(mod) + " name";
    b.id = id;
    b.label = std::string(id) + " label";
    b.fallback = key(fallback);
    b.key = b.fallback;
    return b;
}

} // namespace

TEST(keys_parse_and_format) {
    CHECK_EQ(format(key("Ctrl+H")), std::string("Ctrl+H"));
    CHECK_EQ(format(key("alt + shift + f5")), std::string("Shift+Alt+F5"));
    CHECK_EQ(format(key("control+Num7")), std::string("Ctrl+Num7"));
    CHECK_EQ(format(key("PgDn")), std::string("PageDown"));
    CHECK_EQ(format(key("Ctrl+Plus")), std::string("Ctrl+="));
    CHECK_EQ(format(key("F24")), std::string("F24"));
    CHECK_EQ(format(key("")), std::string("None"));
    CHECK_EQ(format(key("none")), std::string("None"));
    CHECK(!key("None").bound());
    CHECK_EQ(key("A").vk, uint8_t('A'));
    CHECK_EQ(key("ctrl+a").mods, uint8_t(kCtrl));

    Key k;
    CHECK(!parse("Ctrl", k));       // a modifier alone
    CHECK(!parse("Ctrl+", k));
    CHECK(!parse("Hyper+H", k));
    CHECK(!parse("F25", k));
    CHECK(!parse("Ctrl+H+J", k));
    CHECK(!parse("Num10", k));
}

TEST(keys_names_round_trip) {
    for (int vk = 1; vk < 256; ++vk) {
        std::string name = key_name(static_cast<uint8_t>(vk));
        if (name.empty()) continue;
        Key k;
        CHECK(parse(name, k));
        CHECK_EQ(int(k.vk), vk);
    }
    CHECK(is_modifier(0x10));
    CHECK(is_modifier(0xA3));
    CHECK(!is_modifier('A'));
}

TEST(keys_game_use) {
    CHECK(!game_use(key("1")).empty());
    CHECK(!game_use(key("Ctrl+1")).empty());
    CHECK(!game_use(key("F10")).empty());
    CHECK(!game_use(key("H")).empty());  // command card
    CHECK(!game_use(key("Alt+F4")).empty());
    CHECK(game_use(key("Ctrl+H")).empty());
    CHECK(game_use(key("Ctrl+F10")).empty());
    CHECK(game_use(key("None")).empty());
}

TEST(keys_custom_keys) {
    CustomKeys custom = parse_custom_keys("\xEF\xBB\xBF// comment\r\n[CmdMove]\r\nTip=|cffffcc00M|rove\r\nHotkey=M\r\n"
                                          "[AHbz]\nResearchhotkey=z\nHotkey=X,Y,512\nUnhotkey=27\n[Other]\nHotkey=M\n");
    CHECK_EQ(custom.keys.size(), size_t(5));  // M, Z, X, Y, Esc (27)
    CHECK_EQ(custom.keys['M'], std::string("[CmdMove]"));  // the first section that uses it
    CHECK_EQ(custom.keys['Z'], std::string("[AHbz]"));
    CHECK(custom.keys.count(0x1B));
    CHECK_EQ(game_use(key("X"), &custom), std::string("CustomKeys.txt [AHbz]"));
    CHECK_EQ(game_use(key("Q"), &custom), std::string("command card hotkeys"));  // not listed: the default
    CHECK(game_use(key("Ctrl+X"), &custom).empty());  // command card keys have no modifiers
}

TEST(keys_parse_declared) {
    json::Value list;
    CHECK(json::parse(R"([{"id": "toggle", "label": "Toggle bars", "default": "Ctrl+H"}, {"id": "other"}])", list)
              .ok);
    std::vector<Declared> out;
    std::vector<std::string> errors;
    CHECK(parse_declared(list, out, errors));
    CHECK_EQ(out.size(), size_t(2));
    CHECK_EQ(out[0].label, std::string("Toggle bars"));
    CHECK_EQ(format(out[0].key), std::string("Ctrl+H"));
    CHECK(!out[1].key.bound());  // no default: unbound until the player picks one
}

TEST(keys_overrides_and_conflicts) {
    std::vector<Binding> list = {binding("a", "one", "Ctrl+H"), binding("a", "two", "Ctrl+J"),
                                 binding("b", "three", "Ctrl+K")};
    json::Value stored;
    CHECK(json::parse(R"({"b": {"three": "Ctrl+H", "gone": "Ctrl+X"}, "removed": {"x": "Alt+Q"},
                          "a": {"two": "not a key"}})",
                      stored)
              .ok);
    apply_overrides(list, stored);
    CHECK_EQ(format(list[0].key), std::string("Ctrl+H"));
    CHECK_EQ(format(list[1].key), std::string("Ctrl+J"));  // unparsable: the default
    CHECK_EQ(format(list[2].key), std::string("Ctrl+H"));

    find_conflicts(list);
    CHECK_EQ(list[0].conflict, std::string("also b name: three label"));
    CHECK_EQ(list[2].conflict, std::string("also a name: one label"));
    CHECK(list[1].conflict.empty());

    list[1].key = key("F10");
    find_conflicts(list);
    CHECK_EQ(list[1].conflict, std::string("game: the menu"));

    json::Value saved = overrides(list, stored);
    CHECK(saved.find("removed"));  // a mod that isn't installed keeps its entry
    CHECK_EQ(saved.find("a")->find("two")->as_string(), std::string("F10"));
    CHECK(!saved.find("a")->find("one"));  // the default isn't stored
    CHECK_EQ(saved.find("b")->find("three")->as_string(), std::string("Ctrl+H"));
}
