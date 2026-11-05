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
    std::string fdf = mod_menu_fdf(default_chains_model(tome::Edition::FrozenThrone));
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

// The chains model is whatever the caller found in the skin; only that differs.
TEST(fdf_chains_model_goes_into_the_sprite) {
    std::string fdf = mod_menu_fdf("UI\\Custom\\Chains.mdx");
    CHECK(fdf.find("BackgroundArt \"UI\\Custom\\Chains.mdx\",") != std::string::npos);
    CHECK_EQ(brace_balance(fdf), 0L);
}

// The skin key the glue manager uses: "%s_V%d" with Game.expansion.
TEST(fdf_chains_skin_key) {
    using tome::Edition;
    CHECK_EQ(chains_skin_key(Edition::ReignOfChaos), std::string("GlueSpriteLayerTopLeft_V0"));
    CHECK_EQ(chains_skin_key(Edition::FrozenThrone), std::string("GlueSpriteLayerTopLeft_V1"));
    CHECK_EQ(chains_skin_key(Edition::Unknown), std::string("GlueSpriteLayerTopLeft"));
}

// war3skins.txt's values (War3Patch.mpq: _V0 and _V1) name .mdl files.
TEST(fdf_skin_model_path) {
    CHECK_EQ(skin_model_path("UI\\Glues\\SpriteLayers\\TopLeftPanel.mdl"),
             std::string("UI\\Glues\\SpriteLayers\\TopLeftPanel.mdx"));
    CHECK_EQ(skin_model_path("UI\\Glues\\SpriteLayers\\Expansion\\TopLeftPanel-Expansion.MDL"),
             std::string("UI\\Glues\\SpriteLayers\\Expansion\\TopLeftPanel-Expansion.mdx"));
    CHECK_EQ(skin_model_path("UI\\Custom\\Chains.mdx"), std::string("UI\\Custom\\Chains.mdx"));
    CHECK_EQ(skin_model_path("UI\\Dotted.dir\\Chains"), std::string("UI\\Dotted.dir\\Chains"));
    // Nothing that could break out of the FDF string.
    CHECK(skin_model_path(nullptr).empty());
    CHECK(skin_model_path("").empty());
    CHECK(skin_model_path("a.mdx\",\n}\nFrame \"x").empty());
    CHECK(skin_model_path("a\r\nb.mdl").empty());
    CHECK(skin_model_path("\xC3\xA9.mdl").empty());
}

// The fallbacks when the skin has no usable entry.
TEST(fdf_default_chains_model) {
    using tome::Edition;
    CHECK_EQ(std::string(default_chains_model(Edition::ReignOfChaos)),
             std::string("UI\\Glues\\SpriteLayers\\TopLeftPanel.mdx"));
    CHECK_EQ(std::string(default_chains_model(Edition::FrozenThrone)),
             std::string("UI\\Glues\\SpriteLayers\\Expansion\\TopLeftPanel-Expansion.mdx"));
    CHECK_EQ(std::string(default_chains_model(Edition::Unknown)),
             std::string(default_chains_model(Edition::FrozenThrone)));
}

TEST(fdf_icon_definition) {
    std::string fdf = icon_fdf("TomeIcon1234", "UI\\Tome\\Icons\\a.blp");
    CHECK_EQ(brace_balance(fdf), 0L);
    CHECK(fdf.find("Frame \"BACKDROP\" \"TomeIcon1234\" {") != std::string::npos);
    CHECK(fdf.find("BackdropBackground \"UI\\Tome\\Icons\\a.blp\",") != std::string::npos);
}

TEST(fdf_file_uses_crlf) {
    std::string fdf = mod_menu_fdf(default_chains_model(tome::Edition::FrozenThrone));
    bool bare_lf = false;
    for (size_t i = 0; i < fdf.size(); ++i) {
        if (fdf[i] == '\n' && (i == 0 || fdf[i - 1] != '\r')) bare_lf = true;
    }
    CHECK(!bare_lf);
    CHECK(fdf.find("\r\r") == std::string::npos);
}
