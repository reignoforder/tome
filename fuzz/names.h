// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What every name zip::safe_name accepts must satisfy: it stays inside the
// folder it's unpacked into. Traps (a crash libFuzzer reports) otherwise.

#include "core/path_guard.h"
#include "core/zip.h"

#include <string_view>

inline void check_safe_name(std::string_view name) {
    if (name.empty() || name.front() == '/' || name.find('\\') != std::string_view::npos ||
        name.find(':') != std::string_view::npos || name.find('\0') != std::string_view::npos) {
        __builtin_trap();
    }
    std::string_view body = name.back() == '/' ? name.substr(0, name.size() - 1) : name;
    for (size_t i = 0; i <= body.size();) {
        size_t end = body.find('/', i);
        if (end == std::string_view::npos) end = body.size();
        std::string_view part = body.substr(i, end - i);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ') __builtin_trap();
        i = end + 1;
    }
    if (tome::path_guard::is_unsafe(body)) __builtin_trap();
    tome::zip::fold_name(name);
}
