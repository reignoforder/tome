#include "core/blp.h"
#include "test.h"

#include <cstdint>
#include <string>

using namespace tome;

namespace {

void put_u32(std::string& buf, size_t at, uint32_t value) {
    if (buf.size() < at + 4) buf.resize(at + 4);
    buf[at + 0] = char(value & 0xff);
    buf[at + 1] = char(value >> 8 & 0xff);
    buf[at + 2] = char(value >> 16 & 0xff);
    buf[at + 3] = char(value >> 24 & 0xff);
}

// A minimal well-formed JPEG BLP1: one 8x8 mip right after an empty JPEG
// header, everything in bounds. `total` lets a test pad or truncate it.
std::string jpeg_blp(uint32_t width = 8, uint32_t height = 8, uint32_t jpeg_header_size = 0,
                     uint32_t mip0_offset = blp::kHeaderSize + 4, uint32_t mip0_size = 16, size_t total = 0) {
    std::string b(blp::kHeaderSize + 4, '\0');
    put_u32(b, 0x00, 0x31504c42);  // "BLP1"
    put_u32(b, 0x04, 0);           // JPEG
    put_u32(b, 0x0C, width);
    put_u32(b, 0x10, height);
    put_u32(b, 0x18, 0);  // no extra mipmaps: one level
    put_u32(b, blp::kHeaderSize, jpeg_header_size);
    put_u32(b, 0x1C, mip0_offset);  // mipOffsets[0]
    put_u32(b, 0x5C, mip0_size);    // mipSizes[0]
    // `total` sets the file's length exactly (a test places mips past it on
    // purpose); without it, grow just enough to hold the single mip.
    if (total) {
        b.resize(total, '\0');
    } else if (mip0_size && uint64_t(mip0_offset) + mip0_size > b.size()) {
        b.resize(mip0_offset + mip0_size, '\0');
    }
    return b;
}

std::string paletted_blp() {
    std::string b(blp::kHeaderSize + blp::kPaletteSize, '\0');
    put_u32(b, 0x00, 0x31504c42);
    put_u32(b, 0x04, 1);  // paletted
    put_u32(b, 0x0C, 8);
    put_u32(b, 0x10, 8);
    put_u32(b, 0x18, 0);
    put_u32(b, 0x1C, uint32_t(blp::kHeaderSize + blp::kPaletteSize));  // mip0 after the palette
    put_u32(b, 0x5C, 64);  // 8x8 indices, no alpha
    b.resize(blp::kHeaderSize + blp::kPaletteSize + 64, '\0');
    return b;
}

} // namespace

TEST(blp_accepts_valid) {
    CHECK(!blp::is_unsafe(jpeg_blp()));
    CHECK(!blp::is_unsafe(paletted_blp()));
    // A real JPEG header and larger dimensions.
    CHECK(!blp::is_unsafe(jpeg_blp(256, 128, 600, blp::kHeaderSize + 4 + 600, 4096)));
}

TEST(blp_ignores_non_blp) {
    // The game's own magic check handles these; the validator passes them.
    CHECK(!blp::is_unsafe(""));
    CHECK(!blp::is_unsafe("not a blp at all, just bytes"));
    std::string png = "\x89PNG\r\n";
    CHECK(!blp::is_unsafe(png));
}

TEST(blp_rejects_truncated_header) {
    std::string b = jpeg_blp();
    b.resize(blp::kHeaderSize - 1);
    put_u32(b, 0, 0x31504c42);  // still claims BLP1
    CHECK(blp::is_unsafe(b));
}

TEST(blp_rejects_jpeg_header_past_eof) {
    // jpeg_header_size larger than the bytes that follow the fixed header.
    std::string b = jpeg_blp(8, 8, 0x7fffffff, blp::kHeaderSize + 4, 0, blp::kHeaderSize + 8);
    CHECK(blp::is_unsafe(b));
    // Exactly at the end is fine; one past is not.
    CHECK(!blp::is_unsafe(jpeg_blp(8, 8, 4, blp::kHeaderSize + 4, 0, blp::kHeaderSize + 8)));
    CHECK(blp::is_unsafe(jpeg_blp(8, 8, 5, blp::kHeaderSize + 4, 0, blp::kHeaderSize + 8)));
}

TEST(blp_rejects_mip_past_eof) {
    CHECK(blp::is_unsafe(jpeg_blp(8, 8, 0, blp::kHeaderSize + 4, 0x7fffffff, blp::kHeaderSize + 8)));
    // Offset + size overflowing 32 bits must not wrap past the check.
    CHECK(blp::is_unsafe(jpeg_blp(8, 8, 0, 0xfffffff0, 0x20, blp::kHeaderSize + 8)));
}

TEST(blp_rejects_mip_into_header) {
    CHECK(blp::is_unsafe(jpeg_blp(8, 8, 0, 4, 16, 4096)));
}

TEST(blp_rejects_paletted_without_palette) {
    std::string b = paletted_blp();
    b.resize(blp::kHeaderSize + 4);  // no room for the 1024-byte palette
    CHECK(blp::is_unsafe(b));
}

TEST(blp_paletted_mips_sized_by_dimensions) {
    // The game's paletted decoder ignores mipSizes and reads w*h indices
    // (plus alpha) from the offset, so a small declared size doesn't help.
    std::string b = paletted_blp();
    put_u32(b, 0x5C, 1);
    CHECK(!blp::is_unsafe(b));  // the 64 bytes are still in the file
    put_u32(b, 0x08, 8);        // 8-bit alpha: 128 bytes, past the end
    CHECK(blp::is_unsafe(b));
    // A zero-size slot is still decoded: its offset must be in bounds too.
    b = paletted_blp();
    put_u32(b, 0x5C, 0);
    put_u32(b, 0x1C, 0x7fff0000);
    CHECK(blp::is_unsafe(b));
    CHECK_EQ(blp::paletted_mip_bytes(8, 8, 0, 0), uint64_t(64));
    CHECK_EQ(blp::paletted_mip_bytes(8, 8, 1, 0), uint64_t(72));
    CHECK_EQ(blp::paletted_mip_bytes(8, 8, 4, 1), uint64_t(24));
    CHECK_EQ(blp::paletted_mip_bytes(8, 8, 8, 5), uint64_t(2));
}

TEST(blp_jpeg_for_any_compression_but_one) {
    // The game decodes compression 2 (or any value but 1) as JPEG, so its
    // JPEG-header size is checked even though it isn't 0.
    std::string b = jpeg_blp(8, 8, 0x7fffffff, blp::kHeaderSize + 4, 0, blp::kHeaderSize + blp::kPaletteSize + 64);
    put_u32(b, 0x04, 2);
    CHECK(blp::is_unsafe(b));
}

TEST(blp_rejects_jpeg_header_past_header_copy) {
    // The JPEG header is read from the game's 1180-byte copy of the file
    // start, so one longer than what fits there is refused even in a big file.
    const uint32_t fits = uint32_t(blp::kHeaderCopySize - blp::kHeaderSize - 4);
    CHECK(!blp::is_unsafe(jpeg_blp(8, 8, fits, blp::kHeaderCopySize, 16)));
    CHECK(blp::is_unsafe(jpeg_blp(8, 8, fits + 1, blp::kHeaderCopySize + 1, 16)));
}

TEST(blp_rejects_bad_dimensions) {
    CHECK(blp::is_unsafe(jpeg_blp(0, 8)));
    CHECK(blp::is_unsafe(jpeg_blp(8, 0)));
    // 65536 wide implies 17 mip levels: more than the 16 slots.
    CHECK(blp::is_unsafe(jpeg_blp(65536, 1, 0, blp::kHeaderSize + 4, 16, 8192)));
    // 32768 implies exactly 16: allowed.
    CHECK(!blp::is_unsafe(jpeg_blp(32768, 1, 0, blp::kHeaderSize + 4, 16, 8192)));
}

TEST(blp_checks_every_used_mip) {
    // A full chain for 8x8 is 4 levels; the 4th mip offset is out of bounds.
    std::string b = jpeg_blp(8, 8, 0, blp::kHeaderSize + 4, 16, 4096);
    put_u32(b, 0x18, 1);               // has the full mip chain
    put_u32(b, 0x5C + 4 * 3, 16);      // mipSizes[3] used
    put_u32(b, 0x1C + 4 * 3, 0x7fff0000);  // mipOffsets[3] past the file
    CHECK(blp::is_unsafe(b));
    // The same file, but that mip marked unused (size 0): accepted.
    put_u32(b, 0x5C + 4 * 3, 0);
    CHECK(!blp::is_unsafe(b));
}

TEST(blp_mip_levels) {
    CHECK_EQ(blp::mip_levels(1, 1), 1u);
    CHECK_EQ(blp::mip_levels(8, 8), 4u);
    CHECK_EQ(blp::mip_levels(256, 1), 9u);
    CHECK_EQ(blp::mip_levels(512, 512), 10u);
    CHECK_EQ(blp::mip_levels(32768, 1), 16u);
    CHECK_EQ(blp::mip_levels(65536, 1), 17u);
}
