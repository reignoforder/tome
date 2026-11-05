// SPDX-License-Identifier: GPL-3.0-or-later
#include "log_view.h"
#include "log_line.h"

#include <tome/utf8.h>

#include <algorithm>
#include <cstdint>

namespace tome::log_view {
namespace {

using log::Level;

struct Entry {
    std::string_view time;  // "12:34:56", or empty if the line had no prefix
    Level level = Level::Info;
    // From the message to the end of its last continuation line: a view of
    // the log text, made a string only for the lines shown.
    std::string_view message;
};

// "[12:34:56.789] [ 1234] [WARN ] message" (log_line.h's format_line).
bool parse_line(std::string_view line, Entry& out) {
    if (line.size() < 15 || line[0] != '[' || line[13] != ']' || line[14] != ' ') return false;
    std::string_view time = line.substr(1, 8);
    line.remove_prefix(15);
    if (line.empty() || line[0] != '[') return false;
    size_t close = line.find("] ");
    if (close == std::string_view::npos) return false;
    line.remove_prefix(close + 2);
    if (line.size() < 8 || line[0] != '[' || line[6] != ']') return false;
    std::string_view level = line.substr(1, 5);
    bool known = false;
    for (Level l : {Level::Debug, Level::Info, Level::Warn, Level::Error}) {
        if (level == log::level_name(l)) {
            out.level = l;
            known = true;
        }
    }
    if (!known) return false;
    out.time = time;
    out.message = line.substr(line[7] == ' ' ? 8 : 7);
    return true;
}

// The message's lines joined with spaces.
std::string join(std::string_view message) {
    std::string out;
    out.reserve(message.size());
    while (true) {
        size_t end = message.find('\n');
        std::string_view line = message.substr(0, end);
        while (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        out += line;
        if (end == std::string_view::npos) return out;
        out += ' ';
        message.remove_prefix(end + 1);
    }
}

bool passes(Level level, chat::LogFilter filter) {
    switch (filter) {
    case chat::LogFilter::All:   return true;
    case chat::LogFilter::Warn:  return level >= Level::Warn;
    case chat::LogFilter::Error: return level == Level::Error;
    }
    return true;
}

const char* level_text(Level level) {
    switch (level) {
    case Level::Debug: return "|cff808080DEBUG|r";
    case Level::Info:  return "|cff80c0ffINFO|r";
    case Level::Warn:  return "|cffffa000WARN|r";
    case Level::Error: return "|cffff4040ERROR|r";
    }
    return "?";
}

// '|' starts the game's color codes; "||" shows one. Control characters
// (tabs, a stray CR) become spaces, invalid UTF-8 (mods log any bytes) U+FFFD,
// as in every string Tome hands the game.
std::string escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        uint32_t cp;
        i += tome_utf8_decode(text.data() + i, text.size() - i, &cp);
        if (cp == '|') {
            out += "||";
        } else if (cp < 0x20) {
            out += ' ';
        } else {
            char buf[4];
            out.append(buf, tome_utf8_encode(cp, buf));
        }
    }
    return out;
}

// Cuts at kMaxMessage bytes, not inside a UTF-8 sequence.
std::string shorten(std::string text) {
    if (text.size() <= kMaxMessage) return text;
    size_t end = kMaxMessage;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    text.resize(end);
    return text + "...";
}

} // namespace

std::vector<std::string> format(std::string_view log, chat::LogFilter filter, size_t count) {
    std::vector<Entry> entries;
    bool first = true;
    while (!log.empty()) {
        size_t end = log.find('\n');
        std::string_view line = log.substr(0, end);
        log.remove_prefix(end == std::string_view::npos ? log.size() : end + 1);
        while (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        Entry entry;
        if (parse_line(line, entry)) {
            entries.push_back(entry);
        } else if (!entries.empty()) {
            // A continuation line: the message's view grows over it.
            std::string_view& message = entries.back().message;
            message = std::string_view(message.data(), line.data() + line.size() - message.data());
        } else if (!first || line.empty()) {
            // A continuation of an entry before the text starts: dropped.
        } else if (log.empty()) {
            entry.message = line;  // the whole text, one unprefixed line
            entries.push_back(entry);
        }
        first = false;
    }

    std::vector<std::string> out;
    for (auto it = entries.rbegin(); it != entries.rend() && out.size() < count; ++it) {
        if (!passes(it->level, filter)) continue;
        std::string line;
        if (!it->time.empty()) line = std::string(it->time) + " " + level_text(it->level) + " ";
        line += escape(shorten(join(it->message)));
        out.push_back(std::move(line));
    }
    std::reverse(out.begin(), out.end());
    return out;
}

} // namespace tome::log_view
