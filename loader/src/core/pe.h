#pragma once

// Lays out a PE32 file the way the Windows loader maps it, without applying
// relocations: for checking a game.dll on disk (tests, tools) the same way
// Tome checks the loaded module.

#include <cstdint>
#include <string>
#include <vector>

namespace tome::pe {

struct Image {
    uint32_t timestamp = 0;      // FileHeader.TimeDateStamp
    uint32_t size_of_image = 0;  // OptionalHeader.SizeOfImage
    std::vector<uint8_t> bytes;  // size_of_image bytes, sections at their RVAs
};

bool map_image(const std::vector<uint8_t>& file, Image& out, std::string& error);

} // namespace tome::pe
