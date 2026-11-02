// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// .tome packages: a zip of a mod folder (tome.mod.json at the top), unpacked
// by Tome into tome/packages/<stem>.<hash>/ and loaded from there like a
// folder mod. The decisions here are portable; loader/src/packages.cpp does
// the file work.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "zip.h"

namespace tome::package {

constexpr std::string_view kExtension = ".tome";
// Packages larger than this aren't read at all.
constexpr uint64_t kMaxFileSize = 256ull << 20;

// The file name without ".tome" (ASCII case ignored), or "" if `file` isn't
// a package's name (or is only the extension).
std::string stem(std::string_view file);

// The folder a package unpacks into: "<stem>.<16 hex digits of hash>".
std::string unpacked_name(std::string_view stem, uint64_t hash);

// True if `name` (a folder in tome/packages) is one unpacked_name makes, or
// what's left of an unpack that didn't finish (".partial" at the end).
bool is_unpacked_name(std::string_view name);

// The name an unpack writes to before renaming it to unpacked_name's.
std::string partial_name(std::string_view unpacked, uint32_t process_id);

// False, with `error`, unless the archive has tome.mod.json at the top.
bool check_entries(const std::vector<zip::Entry>& entries, std::string& error);

} // namespace tome::package
