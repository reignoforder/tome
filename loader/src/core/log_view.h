// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// /tome logs: the last lines of tome.log, made into chat lines.

#include "chat.h"

#include <string>
#include <string_view>
#include <vector>

namespace tome::log_view {

// Longest message shown; longer ones end in "...".
constexpr size_t kMaxMessage = 200;

// From the end of `log` (tome.log's text, or its tail; a cut first line is
// skipped unless it's the whole text), the last `count` entries that pass
// `filter`, oldest first, each one chat line: "12:34:56 WARN message", the
// level colored, '|' doubled so the game shows it as is, invalid UTF-8 made
// U+FFFD. A line without the "[time] [thread] [LEVEL] " prefix continues the
// entry before it.
std::vector<std::string> format(std::string_view log, chat::LogFilter filter, size_t count);

} // namespace tome::log_view
