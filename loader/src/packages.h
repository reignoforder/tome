#pragma once

// .tome packages (core/package.h): unpacking into tome/packages and removing
// what nothing uses any more. Called by the mod manager under its lock.

#include <cstdint>
#include <set>
#include <string>

#include "paths.h"

namespace tome::packages {

struct Unpacked {
    std::wstring dir;   // the unpacked folder; empty on failure
    std::string error;  // why it couldn't be unpacked
};

// Unpacks `file` (a .tome in tome/mods) into tome/packages/<stem>.<hash>/,
// or finds it there from an earlier unpack of the same contents. Files are
// created only where none exists, so two entries the reader let through as
// different names that are one file on this volume fail the unpack.
Unpacked unpack(const Paths& paths, const std::wstring& file);

// Deletes every unpacked folder in tome/packages (and every unfinished one)
// that isn't in `keep`. A folder still in use (a helper DLL loaded from it)
// stays until a later call.
void remove_unused(const Paths& paths, const std::set<std::wstring>& keep);

} // namespace tome::packages
