#pragma once

// Validator for BLP1 texture files, the format Warcraft III's own loader
// reads (docs/known-bugs.md, "BLP texture loader"). The game trusts the mip
// offsets and sizes and, for JPEG content, the JPEG-header size, and reads
// those spans out of the file buffer without checking them against the file's
// length: a crafted texture (in a downloaded map, or a mod's assets) makes it
// read past the end of the buffer. The blp-guard patch refuses a file this
// rejects before the game decodes it.
//
// The decisions here are portable and parse the file bytes directly, so they
// don't depend on the game's in-memory layout; loader/src/patches/blp_guard.cpp
// does the hooking.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tome::blp {

// The fixed BLP1 header: magic, type fields, then mipOffsets[16] and
// mipSizes[16]. The palette (paletted content) or JPEG-header size (JPEG
// content) follows at kHeaderSize.
constexpr size_t kHeaderSize = 0x9C;      // 156
constexpr size_t kMaxMipLevels = 16;      // the header has 16 mip slots
constexpr size_t kPaletteSize = 0x400;    // 256 * 4 (BGRA), paletted content
// CBLPFile::Open copies this many bytes from the file start into the object
// (0x127 dwords); the JPEG header is read from that copy, so it must fit.
constexpr size_t kHeaderCopySize = 0x49C; // 1180

// Whether the game would read outside `file` while decoding it as a BLP.
// `file` is the whole texture as the game loaded it. Returns a short reason
// (a static string) when it's unsafe to decode, or nullptr when it's safe.
// A file that isn't a BLP1 at all is safe here: the game checks the magic
// and rejects it on its own, so this only judges files it would decode.
const char* reject_reason(std::string_view file);

inline bool is_unsafe(std::string_view file) { return reject_reason(file) != nullptr; }

// The number of mip levels the game decodes for a texture of these
// dimensions: floor(log2(max(w, h))) + 1, i.e. down to 1x1. Exposed for the
// tests.
unsigned mip_levels(uint32_t width, uint32_t height);

// The bytes the game's paletted decoder reads for mip `level`: one palette
// index per pixel, then 1, 4 or 8 bits of alpha per pixel (other alpha_bits
// values read none). It ignores the header's mipSizes. Exposed for the tests.
uint64_t paletted_mip_bytes(uint32_t width, uint32_t height, uint32_t alpha_bits, unsigned level);

} // namespace tome::blp
