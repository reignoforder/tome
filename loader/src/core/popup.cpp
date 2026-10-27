#include "popup.h"

#include <tome/utf8.h>

#include <algorithm>
#include <cstdio>

namespace tome::popup {
namespace {

// Sizes for each style, from the game's templates: DialogWar3's backdrop and
// StandardButtonTemplate (0.031 high) on the menus, the Esc menu's backdrop
// and EscMenuButtonTemplate (0.035 high) in games.
struct Metrics {
    const char* prefix;
    double width;
    double pad_x, pad_top, pad_bottom;
    double title_height, gap;
    double line_height;
    int chars_per_line;    // of the message, at its font size
    double button_height, button_gap;
    double button_char;    // a button label's width per character
    double button_padding; // room a label leaves on either side, both together
};

const Metrics kGlue = {"TomePopup", 0.36, 0.035, 0.035, 0.03, 0.02, 0.012, 0.0165, 44, 0.031, 0.006, 0.0075, 0.03};
const Metrics kEsc = {"TomeEscPopup", 0.34, 0.03, 0.03, 0.028, 0.02, 0.012, 0.0145, 48, 0.035, 0.004, 0.0075, 0.03};

const Metrics& metrics(Style style) {
    return style == Style::Glue ? kGlue : kEsc;
}

size_t code_points(std::string_view s) {
    size_t i = 0, count = 0;
    while (i < s.size()) {
        uint32_t cp;
        i += tome_utf8_decode(s.data() + i, s.size() - i, &cp);
        ++count;
    }
    return count;
}

// The text as it shows: the game's color codes (|cAARRGGBB, |r) take no
// room, and |n is a line break.
std::string visible(std::string_view text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '|' && i + 1 < text.size()) {
            char c = text[i + 1];
            if ((c == 'c' || c == 'C') && i + 10 <= text.size()) {
                i += 9;
                continue;
            }
            if (c == 'r' || c == 'R') {
                ++i;
                continue;
            }
            if (c == 'n' || c == 'N') {
                out += '\n';
                ++i;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

// Whether the options fit side by side in one row.
bool one_row(const Spec& spec, const Metrics& m) {
    const size_t n = spec.options.size();
    if (n == 0 || n > 3) return false;
    const double width = (m.width - 2 * m.pad_x - (n - 1) * m.button_gap) / n;
    for (const std::string& label : spec.options) {
        if (code_points(visible(label)) * m.button_char + m.button_padding > width) return false;
    }
    return true;
}

std::string num(double value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.4f", value);
    return buf;
}

} // namespace

std::string validate(const Spec& spec) {
    const int n = static_cast<int>(spec.options.size());
    if (n == 0) return "a popup needs at least one option";
    if (spec.default_option < -1 || spec.default_option >= n) return "default_option is out of range";
    if (spec.cancel_option < -1 || spec.cancel_option >= n) return "cancel_option is out of range";
    return {};
}

int estimate_lines(std::string_view raw, int chars_per_line) {
    const std::string shown = visible(raw);
    std::string_view text = shown;
    if (text.empty()) return 0;
    if (chars_per_line < 1) chars_per_line = 1;
    int lines = 0;
    size_t start = 0;
    for (;;) {
        size_t end = text.find('\n', start);
        std::string_view paragraph = text.substr(start, end == std::string_view::npos ? end : end - start);
        // Greedy wrapping by words; a word longer than a line takes several.
        int line = 0;  // code points on the current line
        int count = 1;
        size_t i = 0;
        while (i < paragraph.size()) {
            size_t space = paragraph.find(' ', i);
            std::string_view word = paragraph.substr(i, space == std::string_view::npos ? space : space - i);
            int w = static_cast<int>(code_points(word));
            if (line > 0 && line + 1 + w > chars_per_line) {
                ++count;
                line = 0;
            }
            line += (line > 0 ? 1 : 0) + w;
            while (line > chars_per_line) {
                ++count;
                line -= chars_per_line;
            }
            if (space == std::string_view::npos) break;
            i = space + 1;
        }
        lines += count;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return lines;
}

int page_count(const Spec& spec, Style style) {
    const int n = static_cast<int>(spec.options.size());
    if (one_row(spec, metrics(style)) || n <= kColumnPage) return 1;
    return (n + kColumnPage - 1) / kColumnPage;
}

Layout layout(const Spec& spec, Style style, int page) {
    const Metrics& m = metrics(style);
    Layout l;
    l.width = m.width;
    l.content_left = m.pad_x;
    l.content_width = m.width - 2 * m.pad_x;
    l.button_height = m.button_height;
    l.pages = page_count(spec, style);
    l.page = std::clamp(page, 0, l.pages - 1);

    double y = m.pad_top;
    if (!spec.title.empty()) {
        l.title_top = y;
        y += m.title_height + m.gap;
    }
    int lines = std::min(estimate_lines(spec.text, m.chars_per_line), kMaxTextLines);
    if (lines > 0) {
        l.text_top = y;
        l.text_height = lines * m.line_height;
        y += l.text_height + m.gap;
    }

    const int n = static_cast<int>(spec.options.size());
    if (one_row(spec, m)) {
        const double width = (l.content_width - (n - 1) * m.button_gap) / n;
        for (int i = 0; i < n; ++i) {
            l.buttons.push_back({i, m.pad_x + i * (width + m.button_gap), y, width});
        }
        y += m.button_height;
    } else {
        // Every page keeps the first page's height, so the popup doesn't jump.
        const int first = l.page * kColumnPage;
        const int shown = l.pages > 1 ? kColumnPage : n;
        for (int i = 0; i < shown; ++i) {
            if (first + i < n) {
                l.buttons.push_back({first + i, m.pad_x, y + i * (m.button_height + m.button_gap), l.content_width});
            }
        }
        y += shown * m.button_height + (shown - 1) * m.button_gap;
        if (l.pages > 1) {
            y += m.gap;
            const double width = (l.content_width - m.button_gap) / 2;
            if (l.page > 0) l.buttons.push_back({kPrevious, m.pad_x, y, width});
            if (l.page + 1 < l.pages) l.buttons.push_back({kNext, m.pad_x + width + m.button_gap, y, width});
            y += m.button_height;
        }
    }
    l.height = y + m.pad_bottom;
    return l;
}

KeyResult key_pressed(const Spec& spec, Style style, int page, keys::Key key) {
    constexpr uint8_t kVkReturn = 0x0D, kVkEscape = 0x1B, kVkLeft = 0x25, kVkRight = 0x27;
    KeyResult r;
    if (key.mods & keys::kAlt) {
        r.kind = KeyResult::Pass;
        return r;
    }
    if (key.mods != 0) return r;
    const int pages = page_count(spec, style);
    if (key.vk == kVkReturn && spec.default_option >= 0) {
        r = {KeyResult::Pick, spec.default_option, 0};
    } else if (key.vk == kVkEscape && spec.cancel_option >= 0) {
        r = {KeyResult::Pick, spec.cancel_option, 0};
    } else if (key.vk == kVkLeft && page > 0) {
        r = {KeyResult::Page, -1, page - 1};
    } else if (key.vk == kVkRight && page + 1 < pages) {
        r = {KeyResult::Page, -1, page + 1};
    }
    return r;
}

std::string frame_name(Style style, const char* part) {
    return std::string(metrics(style).prefix) + (std::string_view(part) == "Popup" ? "" : part);
}

std::string popup_fdf() {
    std::string s = "// Tome's popups. Generated by loader/src/core/popup.cpp.\n\n"
                    "IncludeFile \"UI\\FrameDef\\Glue\\StandardTemplates.fdf\",\n"
                    "IncludeFile \"UI\\FrameDef\\UI\\EscMenuTemplates.fdf\",\n\n";
    struct Look {
        Style style;
        const char* backdrop;  // the backdrop's body
        const char* title_template;
        const char* text_template;
        const char* text_font;  // a FrameFont line, or ""
        const char* button_template;
        const char* button_text_template;
    };
    const Look looks[] = {
        {Style::Glue,
         "        BackdropTileBackground,\n"
         "        BackdropBackground \"UI\\Widgets\\BattleNet\\bnet-dialoguebox-background.blp\",\n"
         "        BackdropCornerFlags \"UL|UR|BL|BR|T|L|B|R\",\n"
         "        BackdropCornerSize 0.064,\n"
         "        BackdropBackgroundSize 0.128,\n"
         "        BackdropBackgroundInsets 0.01 0.01 0.01 0.01,\n"
         "        BackdropEdgeFile \"UI\\Widgets\\BattleNet\\bnet-dialoguebox-border.blp\",\n",
         "StandardTitleTextTemplate", "StandardValueTextTemplate", "", "StandardButtonTemplate",
         "StandardButtonTextTemplate"},
        {Style::Esc,
         "        DecorateFileNames,\n"
         "        BackdropTileBackground,\n"
         "        BackdropBackground \"EscMenuBackground\",\n"
         "        BackdropCornerFlags \"UL|UR|BL|BR|T|L|B|R\",\n"
         "        BackdropCornerSize 0.048,\n"
         "        BackdropBackgroundSize 0.256,\n"
         "        BackdropBackgroundInsets 0.01 0.01 0.01 0.01,\n"
         "        BackdropEdgeFile \"EscMenuBorder\",\n"
         "        BackdropBlendAll,\n",
         "EscMenuTitleTextTemplate", "EscMenuLabelTextTemplate", "        FrameFont \"EscMenuTextFont\", 0.011, \"\",\n",
         "EscMenuButtonTemplate", "EscMenuButtonTextTemplate"},
    };
    for (const Look& look : looks) {
        const Metrics& m = metrics(look.style);
        auto name = [&](const char* part) { return frame_name(look.style, part); };
        // A button without backdrops over the whole screen takes the clicks
        // meant for what's behind the popup. Tome anchors the rest at runtime.
        s += "Frame \"BUTTON\" \"" + name("Popup") + "\" {\n";
        s += "    SetAllPoints,\n\n";
        s += "    Frame \"BACKDROP\" \"" + name("Backdrop") + "\" {\n" + look.backdrop + "    }\n\n";
        s += "    Frame \"TEXT\" \"" + name("Title") + "\" INHERITS \"" + look.title_template + "\" {\n";
        s += "        FontColor 1.0 1.0 1.0 1.0,\n";
        s += "        FontJustificationH JUSTIFYCENTER,\n";
        s += "        Text \" \",\n";
        s += "    }\n\n";
        s += "    Frame \"TEXT\" \"" + name("Text") + "\" INHERITS \"" + look.text_template + "\" {\n";
        s += look.text_font;
        s += "        FontJustificationH JUSTIFYLEFT,\n";
        s += "        FontJustificationV JUSTIFYTOP,\n";
        s += "        Text \" \",\n";
        s += "    }\n";
        s += "}\n\n";
        // Created inside the popup, one per button, each with its own context.
        s += "Frame \"GLUETEXTBUTTON\" \"" + name("Button") + "\" INHERITS WITHCHILDREN \"" + look.button_template +
             "\" {\n";
        s += "    Height " + num(m.button_height) + ",\n";
        s += "    ButtonText \"" + name("ButtonText") + "\",\n";
        s += "    Frame \"TEXT\" \"" + name("ButtonText") + "\" INHERITS \"" + look.button_text_template + "\" {\n";
        s += "        Text \" \",\n";
        s += "    }\n";
        s += "}\n\n";
    }
    std::string crlf;
    crlf.reserve(s.size() + s.size() / 16);
    for (char c : s) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    return crlf;
}

} // namespace tome::popup
