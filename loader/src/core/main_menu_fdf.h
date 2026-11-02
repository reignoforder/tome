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

#include <string>

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

// The complete FDF file, with CRLF line endings like the game's own files.
std::string mod_menu_fdf();

} // namespace tome::ui
