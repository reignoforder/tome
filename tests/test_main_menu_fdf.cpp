// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/main_menu_fdf.h"
#include "core/mod_menu_model.h"
#include "test.h"

#include <algorithm>

using namespace tome::ui;

namespace {

long brace_balance(const std::string& s) {
    return static_cast<long>(std::count(s.begin(), s.end(), '{')) - static_cast<long>(std::count(s.begin(), s.end(), '}'));
}

bool defines(const std::string& fdf, const std::string& name) {
    return fdf.find("\"" + name + "\" INHERITS") != std::string::npos ||
           fdf.find("\"" + name + "\" {") != std::string::npos;
}

} // namespace

TEST(fdf_frame_names) {
    CHECK_EQ(menu_row_frame(MenuStyle::Glue, 0, "Name"), std::string("TomeModsRow0Name"));
    CHECK_EQ(menu_row_frame(MenuStyle::Esc, 5, "ButtonText"), std::string("TomeEscModsRow5ButtonText"));
    CHECK_EQ(menu_frame(MenuStyle::Glue, "Panel"), std::string("TomeModsPanel"));
    CHECK_EQ(menu_footer_frame(MenuStyle::Esc, 3, true), std::string("TomeEscModsFooter3Text"));
    CHECK_EQ(menu_rows(MenuStyle::Glue), 8);
    CHECK_EQ(menu_rows(MenuStyle::Esc), 6);
}

TEST(fdf_file_is_well_formed) {
    std::string fdf = mod_menu_fdf();
    CHECK_EQ(brace_balance(fdf), 0L);
    CHECK(fdf.find("IncludeFile \"UI\\FrameDef\\Glue\\StandardTemplates.fdf\",") != std::string::npos);
    CHECK(fdf.find("IncludeFile \"UI\\FrameDef\\UI\\EscMenuTemplates.fdf\",") != std::string::npos);
    CHECK(defines(fdf, kModsButtonFrame));
    CHECK(defines(fdf, kEscModsButtonFrame));
    CHECK(defines(fdf, "TomeModsButton"));
    CHECK(defines(fdf, "TomeModsChains"));
    for (MenuStyle style : {MenuStyle::Glue, MenuStyle::Esc}) {
        for (const char* part : {"Panel", "Title", "Summary", "Body"}) CHECK(defines(fdf, menu_frame(style, part)));
        for (int row = 0; row < menu_rows(style); ++row) {
            for (const char* part : {"Name", "Info", "Small", "SmallText", "Button", "ButtonText", "Edit", "EditText"}) {
                CHECK(defines(fdf, menu_row_frame(style, row, part)));
            }
            CHECK(fdf.find("ButtonText \"" + menu_row_frame(style, row, "ButtonText") + "\"") != std::string::npos);
        }
        CHECK(!defines(fdf, menu_row_frame(style, menu_rows(style), "Name")));
        for (int i = 0; i < tome::menu::kFooterButtons; ++i) {
            CHECK(defines(fdf, menu_footer_frame(style, i)));
            CHECK(defines(fdf, menu_footer_frame(style, i, true)));
        }
    }
}

TEST(fdf_icon_definition) {
    std::string fdf = icon_fdf("TomeIcon1234", "UI\\Tome\\Icons\\a.blp");
    CHECK_EQ(brace_balance(fdf), 0L);
    CHECK(fdf.find("Frame \"BACKDROP\" \"TomeIcon1234\" {") != std::string::npos);
    CHECK(fdf.find("BackdropBackground \"UI\\Tome\\Icons\\a.blp\",") != std::string::npos);
}

TEST(fdf_file_uses_crlf) {
    std::string fdf = mod_menu_fdf();
    bool bare_lf = false;
    for (size_t i = 0; i < fdf.size(); ++i) {
        if (fdf[i] == '\n' && (i == 0 || fdf[i - 1] != '\r')) bare_lf = true;
    }
    CHECK(!bare_lf);
    CHECK(fdf.find("\r\r") == std::string::npos);
}
