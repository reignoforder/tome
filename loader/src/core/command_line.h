// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The game's command line, split the way the C runtime splits argv.

#include <string>
#include <string_view>
#include <vector>

namespace tome::command_line {

// Splits at spaces and tabs outside double quotes; quotes group and are
// dropped, and backslashes before a quote escape it (CommandLineToArgvW's rules).
std::vector<std::wstring> split(std::wstring_view command_line);

// Whether any argument after the program's own path is `flag`, ignoring
// ASCII case: has_flag(L"war3.exe -window -TomeSafe", L"-tomesafe").
bool has_flag(std::wstring_view command_line, std::wstring_view flag);

// The values of every `<option>=a,b` argument after the program's own path
// (the option's name in any ASCII case; repeatable), split at commas,
// trimmed of spaces and tabs and lowered to ASCII lowercase; empty items are dropped
// and characters outside ASCII become '?'. For lists of names:
// list_values(L"war3.exe -TomeNoPatch=Widescreen,blp-guard", L"-tomenopatch")
// gives {"widescreen", "blp-guard"}.
std::vector<std::string> list_values(std::wstring_view command_line, std::wstring_view option);

} // namespace tome::command_line
