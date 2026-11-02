// SPDX-License-Identifier: GPL-3.0-or-later
#include "chat.h"

namespace tome::chat {
namespace {

bool is_space(char c) {
    return c == ' ' || c == '\t';
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
    while (!text.empty() && is_space(text.back())) text.remove_suffix(1);
    return text;
}

// The first word of `text` (trimmed), and the rest after it.
std::string_view next_word(std::string_view& text) {
    text = trim(text);
    size_t end = 0;
    while (end < text.size() && !is_space(text[end])) ++end;
    std::string_view word = text.substr(0, end);
    text = trim(text.substr(end));
    return word;
}

} // namespace

std::string lowercase(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool valid_name(std::string_view name) {
    if (name.empty() || name.size() > 32) return false;
    for (char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool parse(std::string_view line, Command& out) {
    if (line.empty() || line.front() != '/') return false;
    line.remove_prefix(1);
    size_t end = 0;
    while (end < line.size() && !is_space(line[end])) ++end;
    std::string_view name = line.substr(0, end);
    if (!valid_name(name)) return false;
    out.name = lowercase(name);
    out.args = std::string(trim(line.substr(end)));
    return true;
}

bool reserved(std::string_view name) {
    // The game's local commands in 1.27b (the table CGameUI::HandleSlashCommand
    // compares against); a mod's command would hide them.
    static const char* const kNames[] = {"tome",  "mapname", "squelch", "unsquelch", "ignore",
                                         "unignore", "fps", "showminimapsignals", "hideminimapsignals"};
    std::string lower = lowercase(name);
    for (const char* n : kNames) {
        if (lower == n) return true;
    }
    return false;
}

TomeCommand parse_tome(std::string_view args) {
    TomeCommand out;
    std::string action = lowercase(next_word(args));
    std::string_view id = next_word(args);
    bool extra = !args.empty();

    if (action.empty() || action == "help") {
        out.action = TomeAction::Help;
    } else if (action == "list") {
        out.action = TomeAction::List;
    } else if (action == "enable" || action == "disable") {
        if (id.empty()) {
            out.error = "usage: /tome " + action + " <mod id>";
            return out;
        }
        out.action = action == "enable" ? TomeAction::Enable : TomeAction::Disable;
        out.id = std::string(id);
    } else if (action == "reload") {
        out.action = TomeAction::Reload;
        out.id = std::string(id);
    } else {
        out.error = "unknown command \"" + action + "\"; try /tome help";
        return out;
    }
    if (extra || (!id.empty() && (out.action == TomeAction::Help || out.action == TomeAction::List))) {
        out = TomeCommand{};
        out.error = "too many arguments; try /tome help";
    }
    return out;
}

} // namespace tome::chat
