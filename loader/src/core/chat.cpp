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

// A line count for /tome logs: 1 to kLogsMax, digits only.
bool parse_count(std::string_view word, size_t& out) {
    if (word.empty() || word.size() > 3) return false;
    size_t n = 0;
    for (char c : word) {
        if (c < '0' || c > '9') return false;
        n = n * 10 + static_cast<size_t>(c - '0');
    }
    if (n < 1 || n > kLogsMax) return false;
    out = n;
    return true;
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
    } else if (action == "perf") {
        out.action = TomeAction::Perf;
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
    } else if (action == "logs" || action == "log") {
        out.action = TomeAction::Logs;
        bool have_filter = false, have_count = false;
        for (std::string_view word = id; !word.empty(); word = next_word(args)) {
            std::string lower = lowercase(word);
            if (!have_filter && (lower == "warn" || lower == "warning" || lower == "warnings" || lower == "error" || lower == "errors")) {
                out.filter = lower[0] == 'w' ? LogFilter::Warn : LogFilter::Error;
                have_filter = true;
            } else if (!have_count && parse_count(word, out.count)) {
                have_count = true;
            } else {
                out = TomeCommand{};
                out.error = "usage: /tome logs [warn|error] [1-" + std::to_string(kLogsMax) + "]";
                return out;
            }
        }
        return out;
    } else {
        out.error = "unknown command \"" + action + "\"; try /tome help";
        return out;
    }
    if (extra || (!id.empty() && (out.action == TomeAction::Help || out.action == TomeAction::List || out.action == TomeAction::Perf))) {
        out = TomeCommand{};
        out.error = "too many arguments; try /tome help";
    }
    return out;
}

} // namespace tome::chat
