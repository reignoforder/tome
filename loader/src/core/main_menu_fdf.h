// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Tome's mod menu as an FDF file (UI\FrameDef\Glue\TomeModMenu.fdf). Kept
// free of Windows APIs so the text it produces can be unit-tested.
//
// It defines the menu twice, with the same parts (core/mod_menu_model.h):
//   Glue  on the main menu: a "Mods" button under Credits, and a panel in
//         the chained panel model, built from the glue templates
//   Esc   in games: a "Mods" button for the Esc menu's main panel, and a
//         panel that takes the Help panel's place, built from the Esc menu's
//         templates

#include "edition.h"

#include <string>
#include <string_view>

namespace tome::ui {

enum class MenuStyle { Glue, Esc };

constexpr const char* kModMenuFdfPath = "UI\\FrameDef\\Glue\\TomeModMenu.fdf";

// Top-level frames Tome creates.
constexpr const char* kModsButtonFrame = "TomeModsBackdrop";    // main menu; contains "TomeModsButton"
constexpr const char* kEscModsButtonFrame = "TomeEscModsButton";  // in the Esc menu's main panel

// Rows per page.
int menu_rows(MenuStyle style);

// Where rows go, in the panel frame's coordinates (y down from its top):
// row r's name is at (left, top - r * step), `name_width` wide.
struct RowGeometry {
    double left, top, step, name_width;
};
RowGeometry row_geometry(MenuStyle style);

// Mod icons on the list: a square left of the name, which moves right by
// kIconSize + kIconGap.
constexpr double kIconSize = 0.03;
constexpr double kIconGap = 0.005;

// An FDF file defining `definition`, a BACKDROP of kIconSize showing
// `texture` (an archive path of a .blp or .tga). A mod's icon gets one,
// named after the icon file's contents, since a name can't be redefined.
std::string icon_fdf(const std::string& definition, const std::string& texture);

// Names of the panel's frames:
//   menu_frame(style, "Panel")   the top-level frame to create
//   menu_frame(style, "Content") everything but the chains (glue)
//   "Title", "Summary", "Body"   text frames
std::string menu_frame(MenuStyle style, const char* part);
// A row's "Name", "Info", "Small", "SmallText", "Button", "ButtonText",
// "Edit" (a text box) or "EditText".
std::string menu_row_frame(MenuStyle style, int row, const char* part);
// Footer button `index` (0-3), or its text frame.
std::string menu_footer_frame(MenuStyle style, int index, bool text = false);

// The chained panel behind the glue panel is the model the game's own glue
// screens show: the one war3skins.txt names for the skin key
// GlueSpriteLayerTopLeft_V<edition>, looked up the way the glue manager does
// (Skin::GetString), so a skin that overrides it changes Tome's panel too.
constexpr const char* kChainsSkinKey = "GlueSpriteLayerTopLeft";

// The key for `edition`: kChainsSkinKey + "_V0" (Reign of Chaos) or "_V1"
// (The Frozen Throne), the game's "%s_V%d" with Game.expansion; the plain key
// (war3.mpq's) for Unknown.
std::string chains_skin_key(Edition edition);

// The model to put in the FDF for a skin value: war3skins.txt names .mdl
// files, which the game loads as .mdx, so a .mdl extension becomes .mdx.
// Empty for null, empty or anything that isn't printable ASCII or holds a
// quote (it goes into an FDF string; a map's CustomSkin can set it).
std::string skin_model_path(const char* value);

// The model when the skin has no usable entry: TopLeftPanel.mdx in Reign of
// Chaos, TopLeftPanel-Expansion.mdx in The Frozen Throne (also for Unknown).
const char* default_chains_model(Edition edition);

// The complete FDF file with `chains_model` behind the glue panel, with CRLF
// line endings like the game's own files. Only the chains model differs
// between editions; everything else inherits the game's templates, which
// each edition loads from its own archives.
std::string mod_menu_fdf(std::string_view chains_model);

} // namespace tome::ui
