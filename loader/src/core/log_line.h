// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The layout of a tome.log line, shared by the logger (log.cpp) and its
// reader (/tome logs, log_view.cpp):
//   "[12:34:56.789] [ 1234] [WARN ] message\r\n"

#include <cstddef>
#include <cstdio>

namespace tome::log {

enum class Level { Debug, Info, Warn, Error };

// The level as it appears in a line: always five characters.
inline const char* level_name(Level level) {
    switch (level) {
    case Level::Debug: return "DEBUG";
    case Level::Info:  return "INFO ";
    case Level::Warn:  return "WARN ";
    case Level::Error: return "ERROR";
    }
    return "?????";
}

// One line as it goes into the file, into `line` (at least 3 bytes), ending
// in CRLF: cut short if needed, but still one line. Its length, or -1.
inline int format_line(char* line, size_t size, unsigned hour, unsigned minute, unsigned second,
                       unsigned millisecond, unsigned long thread, Level level, const char* message) {
    int len = snprintf(line, size, "[%02u:%02u:%02u.%03u] [%5lu] [%s] %s\r\n", hour, minute,
                       second, millisecond, thread, level_name(level), message);
    if (len < 0) return -1;
    if (static_cast<size_t>(len) >= size) {
        len = static_cast<int>(size) - 1;
        line[len - 2] = '\r';
        line[len - 1] = '\n';
    }
    return len;
}

} // namespace tome::log
