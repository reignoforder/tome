#pragma once

// The files in mappings/, compiled into tome.mix so it stays a single file.
// Generated at configure time by cmake/embed_mappings.cmake.

#include <cstddef>

namespace tome::mappings {

struct EmbeddedFile {
    const char* name;  // e.g. "1.27b.json"
    const char* text;
};

extern const EmbeddedFile kEmbeddedFiles[];
extern const size_t kEmbeddedFileCount;

} // namespace tome::mappings
