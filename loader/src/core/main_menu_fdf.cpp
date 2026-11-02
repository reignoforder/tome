// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_menu_fdf.h"

#include "mod_menu_model.h"

#include <cstdio>

namespace tome::ui {
namespace {

// Where each style puts things, in the coordinates of its panel frame.
//
// Glue: the Mods button takes the upper slot of the main menu's bottom box
// (where Options puts OK), and the panel's content sits where Options puts
// the Gameplay settings, inside the same chained panel model.
//
// Esc: the panel takes the Help panel's place, so the Esc menu's backdrop
// has the Help panel's size (0.432 x 0.384).
struct Layout {
    const char* prefix;
    const char* button_template;
    const char* button_text_template;
    const char* title_template;
    const char* name_template;
    const char* info_template;
    const char* info_font;  // FrameFont line for info and body text, or ""
    const char* edit_template;
    const char* edit_text_template;
    int rows;
    double title_x, title_y;
    double summary_x, summary_y, summary_width;  // summary_x: its right edge
    double content_left, row_top, row_step;
    double name_width;
    double small_left, small_width;
    double button_left, button_width, button_height;
    double footer_y;            // the footer buttons' top edge, from the panel's top
    double footer_left[menu::kFooterButtons];
    double footer_width;
    double body_width, body_height;
};

const Layout kGlue = {
    "TomeMods", "StandardSmallButtonTemplate", "StandardButtonTextTemplate", "StandardTitleTextTemplate",
    "StandardValueTextTemplate", "StandardSmallTextTemplate", "",
    "StandardEditBoxTemplate", "StandardEditBoxTextTemplate",
    8,
    0.025, -0.085,
    0.40, -0.087, 0.23,
    0.035, -0.125, 0.042,
    0.205,
    0.245, 0.065,
    0.315, 0.085, 0.026,
    -0.125 - 8 * 0.042 - 0.012,
    {0.035, 0.125, 0.215, 0.315},
    0.085,
    0.365, 0.34,
};

const Layout kEsc = {
    "TomeEscMods", "EscMenuButtonTemplate", "EscMenuButtonTextTemplate", "EscMenuTitleTextTemplate",
    "EscMenuLabelTextTemplate", "EscMenuLabelTextTemplate", "FrameFont \"EscMenuTextFont\", 0.0095, \"\",",
    "EscMenuEditBoxTemplate", "EscMenuEditBoxTextTemplate",
    6,
    0.03, -0.03,
    0.402, -0.032, 0.2,
    0.03, -0.06, 0.04,
    0.215,
    0.25, 0.06,
    0.315, 0.087, 0.03,
    -0.384 + 0.03 + 0.03,
    {0.03, 0.124, 0.218, 0.312},
    0.09,
    0.372, 0.26,
};

const Layout& layout(MenuStyle style) {
    return style == MenuStyle::Glue ? kGlue : kEsc;
}

std::string num(double value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.4f", value);
    return buf;
}

std::string at_panel(const Layout& l, double x, double y, const char* point = "TOPLEFT") {
    return std::string("SetPoint ") + point + ", \"" + l.prefix + "Panel\", TOPLEFT, " + num(x) + ", " + num(y) +
           ",";
}

std::string button(const Layout& l, const std::string& name, double width, const std::string& anchor,
                   const std::string& indent) {
    std::string s;
    s += indent + "Frame \"GLUETEXTBUTTON\" \"" + name + "\" INHERITS WITHCHILDREN \"" + l.button_template + "\" {\n";
    s += indent + "    Width " + num(width) + ",\n";
    s += indent + "    Height " + num(l.button_height) + ",\n";
    s += indent + "    " + anchor + "\n";
    s += indent + "    ButtonText \"" + name + "Text\",\n";
    s += indent + "    Frame \"TEXT\" \"" + name + "Text\" INHERITS \"" + l.button_text_template + "\" {\n";
    s += indent + "        Text \" \",\n";
    s += indent + "    }\n";
    s += indent + "}\n";
    return s;
}

std::string text(const std::string& name, const char* base, const std::string& body, const std::string& indent) {
    std::string s = indent + "Frame \"TEXT\" \"" + name + "\" INHERITS \"" + base + "\" {\n";
    s += body;
    s += indent + "    Text \" \",\n";
    s += indent + "}\n";
    return s;
}

// Title, summary, body, rows and footer: the parts every page fills.
std::string content(MenuStyle style, const std::string& indent) {
    const Layout& l = layout(style);
    const std::string in = indent + "    ";
    std::string s;
    s += text(menu_frame(style, "Title"), l.title_template,
              in + at_panel(l, l.title_x, l.title_y) + "\n" + in + "FontColor 1.0 1.0 1.0 1.0,\n", indent);
    s += text(menu_frame(style, "Summary"), l.info_template,
              in + at_panel(l, l.summary_x, l.summary_y, "TOPRIGHT") + "\n" + in + "Width " + num(l.summary_width) +
                  ",\n" + in + "FontJustificationH JUSTIFYRIGHT,\n",
              indent);
    std::string body_font = *l.info_font ? in + l.info_font + "\n" : "";
    s += text(menu_frame(style, "Body"), l.info_template,
              in + at_panel(l, l.content_left, l.row_top) + "\n" + in + "Width " + num(l.body_width) + ",\n" + in +
                  "Height " + num(l.body_height) + ",\n" + body_font + in + "FontJustificationH JUSTIFYLEFT,\n" + in +
                  "FontJustificationV JUSTIFYTOP,\n",
              indent);
    for (int row = 0; row < l.rows; ++row) {
        double y = l.row_top - row * l.row_step;
        std::string name = menu_row_frame(style, row, "Name");
        s += text(name, l.name_template,
                  in + at_panel(l, l.content_left, y) + "\n" + in + "Width " + num(l.name_width) + ",\n" + in +
                      "FontJustificationH JUSTIFYLEFT,\n",
                  indent);
        s += text(menu_row_frame(style, row, "Info"), l.info_template,
                  in + "SetPoint TOPLEFT, \"" + name + "\", BOTTOMLEFT, 0.0, -0.002,\n" + in + "Width " +
                      num(l.name_width) + ",\n" + body_font + in + "FontJustificationH JUSTIFYLEFT,\n",
                  indent);
        s += button(l, menu_row_frame(style, row, "Small"), l.small_width, at_panel(l, l.small_left, y + 0.002),
                    indent);
        s += button(l, menu_row_frame(style, row, "Button"), l.button_width, at_panel(l, l.button_left, y + 0.002),
                    indent);
        // A text setting's box takes the place of both buttons.
        std::string edit = menu_row_frame(style, row, "Edit");
        s += indent + "Frame \"EDITBOX\" \"" + edit + "\" INHERITS WITHCHILDREN \"" + l.edit_template + "\" {\n";
        s += in + "Width " + num(l.button_left + l.button_width - l.small_left) + ",\n";
        s += in + "Height " + num(l.button_height + 0.004) + ",\n";
        s += in + at_panel(l, l.small_left, y + 0.004) + "\n";
        s += in + "EditTextFrame \"" + edit + "Text\",\n";
        s += in + "Frame \"TEXT\" \"" + edit + "Text\" INHERITS \"" + l.edit_text_template + "\" {\n";
        s += in + "}\n";
        s += indent + "}\n";
    }
    for (int i = 0; i < menu::kFooterButtons; ++i) {
        s += button(l, menu_footer_frame(style, i), l.footer_width, at_panel(l, l.footer_left[i], l.footer_y),
                    indent);
    }
    return s;
}

std::string frames_text() {
    std::string s = R"(// Tome's mod menu. Generated by loader/src/core/main_menu_fdf.cpp.
// These are top-level definitions; Tome creates them inside the main menu
// and the Esc menu at runtime, so the game's own files are never modified.

IncludeFile "UI\FrameDef\Glue\StandardTemplates.fdf",
IncludeFile "UI\FrameDef\UI\EscMenuTemplates.fdf",

// The upper slot of the bottom box, like OK on the Options screen. Tome moves
// the Quit button into the lower slot (Cancel's position) at runtime.
Frame "BACKDROP" "TomeModsBackdrop" INHERITS "StandardMenuButtonBaseBackdrop" {
    Width 0.24,
    SetPoint BOTTOMRIGHT, "MainMenuFrame", BOTTOMRIGHT, -0.015, 0.074,

    Frame "GLUETEXTBUTTON" "TomeModsButton" INHERITS WITHCHILDREN "StandardButtonTemplate" {
        Width 0.168,
        SetPoint TOPRIGHT, "TomeModsBackdrop", TOPRIGHT, -0.012, -0.0165,
        ControlShortcutKey "M",

        ButtonText "TomeModsButtonText",
        Frame "TEXT" "TomeModsButtonText" INHERITS "StandardButtonTextTemplate" {
            Text "Mods",
        }
    }
}

// Covers the screen like the game's own menus; the chained panel model and
// the content are positioned in screen coordinates inside it.
Frame "FRAME" "TomeModsPanel" {
    SetPoint TOPLEFT, "MainMenuFrame", TOPLEFT, 0.0, 0.0,
    SetPoint BOTTOMRIGHT, "MainMenuFrame", BOTTOMRIGHT, 0.0, 0.0,

    // The same chained panel the Options screen lowers on the left.
    Frame "SPRITE" "TomeModsChains" {
        SetAllPoints,
        BackgroundArt "UI\Glues\SpriteLayers\Expansion\TopLeftPanel-Expansion.mdx",
    }

    // Hidden while the panel is moving.
    Frame "FRAME" "TomeModsContent" {
        SetAllPoints,

)";
    s += content(MenuStyle::Glue, "        ");
    s += R"(    }
}

// The Esc menu's Mods button. Tome narrows Options to make room and anchors
// this one beside it, like Help and Tips share the row below.
Frame "GLUETEXTBUTTON" "TomeEscModsButton" INHERITS WITHCHILDREN "EscMenuButtonTemplate" {
    Width 0.11,

    ButtonText "TomeEscModsButtonText",
    Frame "TEXT" "TomeEscModsButtonText" INHERITS "EscMenuButtonTextTemplate" {
        Text "Mods",
    }
}

// Created in the Esc menu's main panel, shown in place of the Help panel.
Frame "FRAME" "TomeEscModsPanel" {
    SetAllPoints,

)";
    s += content(MenuStyle::Esc, "    ");
    s += "}\n";
    return s;
}

} // namespace

int menu_rows(MenuStyle style) {
    return layout(style).rows;
}

RowGeometry row_geometry(MenuStyle style) {
    const Layout& l = layout(style);
    return {l.content_left, l.row_top, l.row_step, l.name_width};
}

std::string icon_fdf(const std::string& definition, const std::string& texture) {
    std::string text = "Frame \"BACKDROP\" \"" + definition + "\" {\r\n";
    text += "    Width " + num(kIconSize) + ",\r\n    Height " + num(kIconSize) + ",\r\n";
    text += "    BackdropBackground \"" + texture + "\",\r\n";
    text += "    BackdropBlendAll,\r\n}\r\n";
    return text;
}

std::string menu_frame(MenuStyle style, const char* part) {
    return std::string(layout(style).prefix) + part;
}

std::string menu_row_frame(MenuStyle style, int row, const char* part) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%sRow%d%s", layout(style).prefix, row, part);
    return buf;
}

std::string menu_footer_frame(MenuStyle style, int index, bool text) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%sFooter%d%s", layout(style).prefix, index, text ? "Text" : "");
    return buf;
}

std::string mod_menu_fdf() {
    std::string text = frames_text();
    std::string crlf;  // the game's FDF files use CRLF line endings
    crlf.reserve(text.size() + text.size() / 16);
    for (char c : text) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    return crlf;
}

} // namespace tome::ui
