// SPDX-License-Identifier: GPL-3.0-or-later
#include "command_line.h"

#include "text.h"

namespace tome::command_line {
std::vector<std::wstring> split(std::wstring_view line) {
    std::vector<std::wstring> args;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == L' ' || line[i] == L'\t')) ++i;
        if (i == line.size()) break;
        std::wstring arg;
        bool quoted = false;
        while (i < line.size() && (quoted || (line[i] != L' ' && line[i] != L'\t'))) {
            if (line[i] == L'\\') {
                size_t slashes = 0;
                while (i < line.size() && line[i] == L'\\') {
                    ++slashes;
                    ++i;
                }
                if (i < line.size() && line[i] == L'"') {
                    // 2n backslashes and a quote: n backslashes, then the quote
                    // toggles; 2n+1: n backslashes and a literal quote.
                    arg.append(slashes / 2, L'\\');
                    if (slashes % 2) {
                        arg += L'"';
                        ++i;
                    }
                } else {
                    arg.append(slashes, L'\\');
                }
                continue;
            }
            if (line[i] == L'"') {
                if (quoted && i + 1 < line.size() && line[i + 1] == L'"') {
                    arg += L'"';  // "" inside quotes
                    i += 2;
                    continue;
                }
                quoted = !quoted;
                ++i;
                continue;
            }
            arg += line[i++];
        }
        args.push_back(std::move(arg));
    }
    return args;
}

bool has_flag(std::wstring_view command_line, std::wstring_view flag) {
    std::vector<std::wstring> args = split(command_line);
    for (size_t i = 1; i < args.size(); ++i) {
        if (text::iequals(args[i], flag)) return true;
    }
    return false;
}

std::vector<std::string> list_values(std::wstring_view command_line, std::wstring_view option) {
    std::vector<std::string> out;
    std::vector<std::wstring> args = split(command_line);
    for (size_t i = 1; i < args.size(); ++i) {
        std::wstring_view arg = args[i];
        if (arg.size() <= option.size() || arg[option.size()] != L'=' ||
            !text::iequals(arg.substr(0, option.size()), option)) {
            continue;
        }
        std::string item;
        auto flush = [&] {
            size_t b = item.find_first_not_of(" \t"), e = item.find_last_not_of(" \t");
            if (b != std::string::npos) out.push_back(item.substr(b, e - b + 1));
            item.clear();
        };
        for (wchar_t c : arg.substr(option.size() + 1)) {
            if (c == L',') {
                flush();
            } else {
                item += c < 0x80 ? static_cast<char>(text::ascii_lower(c)) : '?';
            }
        }
        flush();
    }
    return out;
}

} // namespace tome::command_line
