#include "core/mod_menu_model.h"
#include "core/popup.h"
#include "test.h"

#include <algorithm>

using namespace tome;
using namespace tome::popup;

namespace {

Spec spec_with(int options, const char* label = "Option") {
    Spec s;
    s.title = "Title";
    s.text = "Some text.";
    for (int i = 0; i < options; ++i) s.options.push_back(std::string(label) + " " + std::to_string(i + 1));
    return s;
}

keys::Key key(uint8_t vk, uint8_t mods = 0) {
    keys::Key k;
    k.vk = vk;
    k.mods = mods;
    return k;
}

} // namespace

TEST(popup_validate) {
    Spec s;
    CHECK(!validate(s).empty());
    s.options = {"OK"};
    CHECK(validate(s).empty());
    s.default_option = 1;
    CHECK(!validate(s).empty());
    s.default_option = 0;
    s.cancel_option = -2;
    CHECK(!validate(s).empty());
    s.cancel_option = -1;
    CHECK(validate(s).empty());
}

TEST(popup_estimate_lines) {
    CHECK_EQ(estimate_lines("", 10), 0);
    CHECK_EQ(estimate_lines("short", 10), 1);
    CHECK_EQ(estimate_lines("one two three", 10), 2);  // "one two" / "three"
    CHECK_EQ(estimate_lines("a\nb\n\nc", 10), 4);
    CHECK_EQ(estimate_lines("abcdefghijklmnopqrstuvwxy", 10), 3);  // a long word takes several
    CHECK_EQ(estimate_lines("|cffff0000red|r words", 9), 1);       // color codes take no room
    CHECK_EQ(estimate_lines("a|nb", 10), 2);
    CHECK_EQ(estimate_lines("\xC3\xA9\xC3\xA9\xC3\xA9", 3), 1);    // code points, not bytes
}

TEST(popup_short_options_share_a_row) {
    for (Style style : {Style::Glue, Style::Esc}) {
        Spec s = spec_with(2, "Go");
        Layout l = layout(s, style, 0);
        CHECK_EQ(l.pages, 1);
        CHECK_EQ(l.buttons.size(), size_t(2));
        CHECK_EQ(l.buttons[0].top, l.buttons[1].top);
        CHECK(l.buttons[0].left + l.buttons[0].width < l.buttons[1].left);
        CHECK(l.buttons[1].left + l.buttons[1].width <= l.width);
        CHECK(l.buttons[0].top + l.button_height < l.height);
        CHECK(l.text_top > l.title_top);
        CHECK(l.buttons[0].top >= l.text_top + l.text_height);
    }
}

TEST(popup_many_options_page_in_a_column) {
    Spec s = spec_with(15);
    CHECK_EQ(page_count(s, Style::Glue), 3);
    Layout first = layout(s, Style::Glue, 0);
    CHECK_EQ(first.page, 0);
    // A full page and Next.
    CHECK_EQ(first.buttons.size(), size_t(kColumnPage + 1));
    CHECK_EQ(first.buttons.back().option, kNext);
    for (int i = 0; i < kColumnPage; ++i) CHECK_EQ(first.buttons[i].option, i);
    Layout middle = layout(s, Style::Glue, 1);
    CHECK_EQ(middle.buttons.size(), size_t(kColumnPage + 2));
    CHECK_EQ(middle.buttons[0].option, kColumnPage);
    Layout last = layout(s, Style::Glue, 99);  // clamped
    CHECK_EQ(last.page, 2);
    CHECK_EQ(last.buttons.size(), size_t(15 - 2 * kColumnPage + 1));
    CHECK_EQ(last.buttons.back().option, kPrevious);
    // Every page has the same size.
    CHECK_EQ(first.height, last.height);
    // Nothing overlaps in a column.
    for (size_t i = 1; i < kColumnPage; ++i) {
        CHECK(first.buttons[i].top >= first.buttons[i - 1].top + first.button_height);
    }
}

TEST(popup_long_labels_go_in_a_column) {
    Spec s = spec_with(2, "A label far too long to share a row with another one");
    Layout l = layout(s, Style::Glue, 0);
    CHECK_EQ(l.pages, 1);
    CHECK(l.buttons[1].top > l.buttons[0].top);
    CHECK_EQ(l.buttons[0].width, l.content_width);
    // Four short ones don't fit a row either, but fit one page.
    Layout four = layout(spec_with(4, "x"), Style::Esc, 0);
    CHECK_EQ(four.buttons.size(), size_t(4));
    CHECK(four.buttons[3].top > four.buttons[0].top);
}

TEST(popup_without_title_or_text) {
    Spec s;
    s.options = {"OK"};
    Layout l = layout(s, Style::Glue, 0);
    CHECK_EQ(l.text_height, 0.0);
    Layout full = layout(spec_with(1), Style::Glue, 0);
    CHECK(l.height < full.height);
    // Long text grows the popup, up to kMaxTextLines.
    Spec long_text = spec_with(1);
    long_text.text = std::string(5000, 'x');
    Spec longer = long_text;
    longer.text += std::string(5000, 'y');
    CHECK(layout(long_text, Style::Glue, 0).height > full.height);
    CHECK_EQ(layout(long_text, Style::Glue, 0).height, layout(longer, Style::Glue, 0).height);
}

TEST(popup_keys) {
    Spec s = spec_with(2);
    s.default_option = 0;
    s.cancel_option = 1;
    KeyResult r = key_pressed(s, Style::Glue, 0, key(0x0D));
    CHECK_EQ(r.kind, KeyResult::Pick);
    CHECK_EQ(r.option, 0);
    r = key_pressed(s, Style::Glue, 0, key(0x1B));
    CHECK_EQ(r.kind, KeyResult::Pick);
    CHECK_EQ(r.option, 1);
    CHECK_EQ(key_pressed(s, Style::Glue, 0, key('A')).kind, KeyResult::None);
    CHECK_EQ(key_pressed(s, Style::Glue, 0, key(0x73, keys::kAlt)).kind, KeyResult::Pass);  // Alt+F4
    CHECK_EQ(key_pressed(s, Style::Glue, 0, key(0x0D, keys::kCtrl)).kind, KeyResult::None);
    // Without default and cancel options, Enter and Esc do nothing.
    Spec plain = spec_with(2);
    CHECK_EQ(key_pressed(plain, Style::Glue, 0, key(0x0D)).kind, KeyResult::None);
    CHECK_EQ(key_pressed(plain, Style::Glue, 0, key(0x1B)).kind, KeyResult::None);
    // Arrows turn pages.
    Spec many = spec_with(13);
    CHECK_EQ(key_pressed(many, Style::Glue, 0, key(0x25)).kind, KeyResult::None);
    r = key_pressed(many, Style::Glue, 0, key(0x27));
    CHECK_EQ(r.kind, KeyResult::Page);
    CHECK_EQ(r.page, 1);
    CHECK_EQ(key_pressed(many, Style::Glue, 2, key(0x27)).kind, KeyResult::None);
}

TEST(popup_fdf_defines_both_styles) {
    std::string fdf = popup_fdf();
    CHECK_EQ(std::count(fdf.begin(), fdf.end(), '{'), std::count(fdf.begin(), fdf.end(), '}'));
    CHECK(fdf.find("\r\n") != std::string::npos);
    CHECK_EQ(frame_name(Style::Glue, "Popup"), std::string("TomePopup"));
    CHECK_EQ(frame_name(Style::Esc, "ButtonText"), std::string("TomeEscPopupButtonText"));
    for (Style style : {Style::Glue, Style::Esc}) {
        for (const char* part : {"Popup", "Backdrop", "Title", "Text", "Button", "ButtonText"}) {
            CHECK(fdf.find("\"" + frame_name(style, part) + "\"") != std::string::npos);
        }
        CHECK(fdf.find("ButtonText \"" + frame_name(style, "ButtonText") + "\"") != std::string::npos);
    }
}

TEST(menu_restart_prompt) {
    ModStatus a, b;
    a.folder = "a";
    a.name = "Alpha";
    b.folder = "b";
    b.name = "Beta";
    std::vector<ModStatus> before = {a, b};
    CHECK(menu::newly_restart_pending(before, before).empty());
    std::vector<ModStatus> after = before;
    after[0].restart_pending = true;
    std::vector<std::string> names = menu::newly_restart_pending(before, after);
    CHECK_EQ(names.size(), size_t(1));
    CHECK_EQ(names[0], std::string("Alpha"));
    // Already waiting before: no new prompt.
    CHECK(menu::newly_restart_pending(after, after).empty());

    Spec s = menu::restart_prompt({"Alpha", "Beta"}, false);
    CHECK(validate(s).empty());
    CHECK_EQ(s.options[menu::kRestartNow], std::string("Restart Now"));
    CHECK_EQ(s.options[menu::kRestartLater], std::string("Later"));
    CHECK_EQ(s.cancel_option, menu::kRestartLater);
    CHECK(s.text.find("Alpha and Beta apply their") != std::string::npos);
    CHECK(s.text.find("leaves the game") == std::string::npos);
    CHECK(menu::restart_prompt({"Alpha"}, true).text.find("leaves the game") != std::string::npos);
}
