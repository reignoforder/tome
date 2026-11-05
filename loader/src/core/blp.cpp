// SPDX-License-Identifier: GPL-3.0-or-later
#include "blp.h"

#include <cstdint>

namespace tome::blp {
namespace {

// Little-endian u32 at byte offset `at`; the caller guarantees it's in range.
uint32_t u32(std::string_view file, size_t at) {
    auto b = reinterpret_cast<const unsigned char*>(file.data()) + at;
    return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
}

constexpr uint32_t kMagicBlp1 = 0x31504c42;  // "BLP1"
constexpr size_t kMipOffsets = 0x1C;         // u32[16]
constexpr size_t kMipSizes = 0x5C;           // u32[16]

} // namespace

uint64_t paletted_mip_bytes(uint32_t width, uint32_t height, uint32_t alpha_bits, unsigned level) {
    const uint64_t w = (width >> level) > 1 ? (width >> level) : 1;
    const uint64_t h = (height >> level) > 1 ? (height >> level) : 1;
    const uint64_t pixels = w * h;
    switch (alpha_bits) {
    case 1: return pixels + (pixels + 7) / 8;
    case 4: return pixels + (pixels + 1) / 2;
    case 8: return pixels * 2;
    default: return pixels;  // no alpha channel is read
    }
}

unsigned mip_levels(uint32_t width, uint32_t height) {
    uint32_t largest = width > height ? width : height;
    unsigned levels = 1;
    while (largest > 1) {
        largest >>= 1;
        ++levels;
    }
    return levels;
}

const char* reject_reason(std::string_view file) {
    // Not a BLP1: the game's own magic check rejects it, so nothing here to do.
    if (file.size() < 4 || u32(file, 0) != kMagicBlp1) return nullptr;

    // It claims to be a BLP1 but is too short to hold even the fixed header
    // (mip offset/size arrays): the game reads those arrays regardless.
    if (file.size() < kHeaderSize + 4) return "BLP1 shorter than its header";

    const uint32_t compression = u32(file, 0x04);  // 1: paletted, anything else: JPEG
    const uint32_t alpha_bits = u32(file, 0x08);
    const uint32_t width = u32(file, 0x0C);
    const uint32_t height = u32(file, 0x10);
    const uint32_t has_mipmaps = u32(file, 0x18);
    if (width == 0 || height == 0) return "BLP1 with a zero dimension";

    // The game indexes the 16-slot mip arrays by a count it derives from the
    // dimensions (down to 1x1), capped by the header's 16 slots. Dimensions
    // that imply more than 16 levels would read past the arrays.
    const unsigned levels = mip_levels(width, height);
    if (levels > kMaxMipLevels) return "BLP1 dimensions imply more than 16 mip levels";
    const unsigned mips = has_mipmaps == 0 ? 1 : levels;

    const uint64_t size = file.size();
    // The game's mip decoder takes the paletted path only for compression 1
    // and the JPEG path for every other value, so judge it the same way.
    const bool paletted = compression == 1;
    if (!paletted) {
        // JPEG: a shared JPEG header of jpeg_header_size bytes follows the
        // fixed header. The game copies it out of its in-object copy of the
        // header (kHeaderCopySize bytes from the file start), not out of the
        // file, so it must fit both the file and that copy.
        const uint32_t jpeg_header_size = u32(file, kHeaderSize);
        if (kHeaderSize + 4 + uint64_t(jpeg_header_size) > size) return "BLP1 JPEG header runs past the file";
        if (kHeaderSize + 4 + uint64_t(jpeg_header_size) > kHeaderCopySize) {
            return "BLP1 JPEG header longer than the game's header copy";
        }
    } else {
        // Paletted: a 256-entry palette follows the fixed header.
        if (kHeaderSize + kPaletteSize > size) return "BLP1 paletted but has no room for its palette";
    }

    // Each mip the game decodes must lie inside the file.
    for (unsigned i = 0; i < mips; ++i) {
        const uint32_t offset = u32(file, kMipOffsets + 4 * i);
        uint64_t length = u32(file, kMipSizes + 4 * i);
        if (paletted) {
            // The paletted decoder ignores mipSizes: it reads one index byte
            // per pixel of the mip, then the alpha bits the header declares.
            length = paletted_mip_bytes(width, height, alpha_bits, i);
        } else if (length == 0) {
            continue;  // the JPEG decoder copies nothing from the file for it
        }
        if (offset < kHeaderSize) return "BLP1 mip overlaps the header";
        if (uint64_t(offset) + length > size) return "BLP1 mip runs past the file";
    }
    return nullptr;
}

} // namespace tome::blp
