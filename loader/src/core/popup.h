#pragma once

// Popups: a title, a message and any number of buttons, one popup at a time
// on whatever screen is showing (the mod API's `popup`, and Tome's own
// "restart required"). This part is the layout and the FDF definitions,
// kept free of Windows APIs so it can be unit-tested; popup.cpp makes the
// frames and runs the queue.
//
// Up to three options with short labels share one row, like the game's
// Yes/No dialogs; more go in a column, kColumnPage to a page, with Previous
// and Next under them.

#include "keys.h"

#include <string>
#include <string_view>
#include <vector>

namespace tome::popup {

// Glue on the menu screens, Esc (the Esc menu's look) in games.
enum class Style { Glue, Esc };

constexpr const char* kFdfPath = "UI\\FrameDef\\Tome\\TomePopup.fdf";
constexpr int kColumnPage = 6;
constexpr int kMaxTextLines = 16;  // longer messages are cut off

struct Spec {
    std::string title;  // "" for none
    std::string text;   // "" for none; '\n' starts a new line
    std::vector<std::string> options;
    int default_option = -1;  // Enter picks it; -1: Enter does nothing
    int cancel_option = -1;   // Esc picks it; -1: Esc does nothing
};

// Why the spec can't be shown ("" if it can): no options, or an index out of range.
std::string validate(const Spec& spec);

// Lines `text` takes when wrapped at `chars_per_line` code points, breaking
// at spaces where it can, with the game's color codes left out and |n as a
// line break. 0 for "".
int estimate_lines(std::string_view text, int chars_per_line);

// Buttons that aren't options.
constexpr int kPrevious = -2;
constexpr int kNext = -3;

// Everything in the popup's coordinates: x right and y down from its top
// left corner, in screen units (the screen is 0.8 x 0.6).
struct Layout {
    double width = 0, height = 0;
    double content_left = 0, content_width = 0;
    double title_top = 0;  // the title's top edge; no title if the spec has none
    double text_top = 0, text_height = 0;
    struct Button {
        int option;  // index into Spec::options, or kPrevious / kNext
        double left, top, width;
    };
    std::vector<Button> buttons;
    double button_height = 0;
    int page = 0, pages = 1;
};

int page_count(const Spec& spec, Style style);

// The layout of `page` (clamped to the pages there are).
Layout layout(const Spec& spec, Style style, int page);

// What a key does while a popup shows.
struct KeyResult {
    enum Kind {
        None,   // nothing; the popup still takes the key from the game
        Pick,   // option
        Page,   // page: show this page instead
        Pass,   // let the game have it (Alt combinations: Alt+F4, Alt+Tab)
    };
    Kind kind = None;
    int option = -1;
    int page = 0;
};
KeyResult key_pressed(const Spec& spec, Style style, int page, keys::Key key);

// Names of the frames: "Popup" (the top-level frame, covering the screen so
// clicks don't reach what's behind), "Backdrop", "Title", "Text",
// "Button" (created once per button) and "ButtonText".
std::string frame_name(Style style, const char* part);

// The FDF file defining both styles, with CRLF line endings.
std::string popup_fdf();

} // namespace tome::popup
