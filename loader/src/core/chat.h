// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Chat commands typed in the in-game chat box ("/tome list", or a mod's
// "/name args"): parsing and the names Tome accepts. The hook that sees the
// lines is in loader/src/events.cpp.

#include <string>
#include <string_view>

namespace tome::chat {

struct Command {
    std::string name;  // lowercase, without the '/'
    std::string args;  // the rest of the line, trimmed; may be empty
};

// "/Name  some args " -> {"name", "some args"}. False unless the line starts
// with '/' followed by a valid name and then a space or the end.
bool parse(std::string_view line, Command& out);

// 1 to 32 characters from a-z, 0-9, '_' and '-' (any case; names compare
// case-insensitively).
bool valid_name(std::string_view name);

std::string lowercase(std::string_view text);

// Names a mod can't register: Tome's own and the game's local commands
// (/mapname, /fps, ...), which the game handles itself.
bool reserved(std::string_view name);

// The /tome command's subcommands.
enum class TomeAction { Help, List, Enable, Disable, Reload, Invalid };

struct TomeCommand {
    TomeAction action = TomeAction::Invalid;
    std::string id;     // Enable, Disable: required; Reload: empty means every mod
    std::string error;  // Invalid: what's wrong, for the player
};

// Parses the arguments of "/tome": "", "help", "list", "enable <id>",
// "disable <id>", "reload [id]".
TomeCommand parse_tome(std::string_view args);

} // namespace tome::chat
