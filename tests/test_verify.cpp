// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/embedded_mappings.h"
#include "core/mappings.h"
#include "core/pe.h"
#include "core/verify.h"
#include "test.h"

#include <cstdio>
#include <cstring>

using namespace tome;

namespace {

bool parse_build_text(const std::string& text, mappings::Build& out) {
    json::Value root;
    if (!json::parse(text, root).ok) return false;
    auto r = mappings::parse_build(root, out);
    for (const std::string& e : r.errors) test::fail(__FILE__, __LINE__, e);
    return r.ok;
}

// A 0x100-byte image: a function at 0x10, a call to it at 0x40, and an
// import directory (descriptors at 0x88) for KERNEL32.dll with slots at 0xD8
// for Sleep and GetTickCount, as the loader leaves it: the slots hold
// addresses, the lookup table (0xC8) the names.
std::vector<uint8_t> small_image() {
    std::vector<uint8_t> image(0x100, 0xCC);
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&image[at], &v, 4); };
    put32(0x3C, 0x00);        // "NT headers" at 0: the import directory's entry is at 0x80
    put32(0x18 + 104, 0x88);  // import directory
    std::memset(&image[0x88], 0, 0x28);  // one descriptor and the terminating one
    put32(0x88, 0xC8);        // OriginalFirstThunk
    put32(0x88 + 12, 0xE8);   // Name
    put32(0x88 + 16, 0xD8);   // FirstThunk
    std::memcpy(&image[0xB0], "\0\0Sleep", 8);
    std::memcpy(&image[0xB8], "\0\0GetTickCount", 15);
    put32(0xC8, 0xB0);
    put32(0xCC, 0xB8);
    put32(0xD0, 0);
    put32(0xD8, 0x7b801234);  // what the loader wrote
    put32(0xDC, 0x7b805678);
    std::memcpy(&image[0xE8], "KERNEL32.dll", 13);
    const uint8_t fn[] = {0x55, 0x8b, 0xec, 0xa1, 0x78, 0x56, 0x34, 0x12};
    std::memcpy(&image[0x10], fn, sizeof(fn));
    const uint8_t call[] = {0xE8, 0xCB, 0xFF, 0xFF, 0xFF};  // 0x45 + (-0x35) = 0x10
    std::memcpy(&image[0x40], call, sizeof(call));
    return image;
}

const char* kSmallBuild = R"j({"schema": 2, "name": "t", "file_version": "1", "pe_timestamp": 1,
    "size_of_image": "0x100",
    "sha256": "d51e5680243fc90e19c9d6074f7fac433c466d3cf5f46e2364291725574d8236",
    "symbols": {
        "Fn": {"kind": "function", "rva": "0x10", "conv": "cdecl", "sig": "void(void)",
               "bytes": "55 8b ec a1 ?? ?? ?? ??"},
        "Fn.call.here": {"kind": "call", "rva": "0x40", "target": "Fn"},
        "Import.GetTickCount": {"kind": "import", "rva": "0xdc", "conv": "stdcall", "sig": "uint32_t(void)",
                                "target": "kernel32.dll!GetTickCount"}
    }})j";

bool contains(const std::vector<std::string>& errors, const char* text) {
    for (const std::string& e : errors) {
        if (e.find(text) != std::string::npos) return true;
    }
    return false;
}

bool read_file(const char* path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(size > 0 ? size_t(size) : 0);
    bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

} // namespace

TEST(verify_accepts_matching_code) {
    mappings::Build b;
    CHECK(parse_build_text(kSmallBuild, b));
    std::vector<uint8_t> image = small_image();
    CHECK(mappings::verify_code(b, image.data(), image.size()).empty());
    // Wildcards cover relocated bytes.
    image[0x14] = 0x00;
    CHECK(mappings::verify_code(b, image.data(), image.size()).empty());
}

TEST(verify_names_each_mismatch) {
    mappings::Build b;
    CHECK(parse_build_text(kSmallBuild, b));
    std::vector<uint8_t> image = small_image();
    image[0x11] = 0x90;
    auto errors = mappings::verify_code(b, image.data(), image.size());
    CHECK_EQ(errors.size(), size_t(1));
    CHECK(contains(errors, "\"Fn\" at 0x00000010: byte 1 is 0x90, expected 0x8b"));

    image = small_image();
    image[0x41] = 0xCC;  // the call now goes elsewhere
    errors = mappings::verify_code(b, image.data(), image.size());
    CHECK(contains(errors, "\"Fn.call.here\" at 0x00000040: calls 0x00000011, expected \"Fn\" at 0x00000010"));

    image[0x40] = 0xE9;  // someone else's jmp hook
    errors = mappings::verify_code(b, image.data(), image.size());
    CHECK(contains(errors, "\"Fn.call.here\" at 0x00000040: not a call (byte 0xe9, expected 0xe8)"));

    errors = mappings::verify_code(b, image.data(), 0x12);
    CHECK(contains(errors, "\"Fn\" at 0x00000010: outside the loaded image"));
    CHECK(contains(errors, "\"Fn.call.here\" at 0x00000040: outside the loaded image"));
}

TEST(verify_checks_import_slots) {
    mappings::Build b;
    CHECK(parse_build_text(kSmallBuild, b));
    std::vector<uint8_t> image = small_image();
    image[0xBA] = 'S';  // the slot now imports SetTickCount
    auto errors = mappings::verify_code(b, image.data(), image.size());
    CHECK_EQ(errors.size(), size_t(1));
    CHECK(contains(errors, "\"Import.GetTickCount\" at 0x000000dc: imports KERNEL32.dll!SetTickCount, "
                           "expected kernel32.dll!GetTickCount"));

    // One slot off: that's Sleep's.
    std::string text = kSmallBuild;
    text.replace(text.find("\"0xdc\""), 6, "\"0xd8\"");
    CHECK(parse_build_text(text, b));
    errors = mappings::verify_code(b, small_image().data(), image.size());
    CHECK(contains(errors, "imports KERNEL32.dll!Sleep, expected kernel32.dll!GetTickCount"));

    // Past the slots.
    text.replace(text.find("\"0xd8\""), 6, "\"0xe0\"");
    CHECK(parse_build_text(text, b));
    errors = mappings::verify_code(b, small_image().data(), image.size());
    CHECK(contains(errors, "\"Import.GetTickCount\" at 0x000000e0: not an import slot"));
}

TEST(pe_map_image_places_sections) {
    // Headers at 0, one section: 0x10 raw bytes at file 0x200, mapped at RVA 0x1000.
    std::vector<uint8_t> file(0x210, 0);
    auto put16 = [&](size_t at, uint16_t v) { std::memcpy(&file[at], &v, 2); };
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&file[at], &v, 4); };
    put16(0, 0x5A4D);
    put32(0x3C, 0x80);
    put32(0x80, 0x00004550);
    put16(0x84, 0x14C);
    put16(0x86, 1);
    put32(0x88, 0x584a499e);
    put16(0x94, 0xE0);
    put32(0x98 + 56, 0x2000);  // SizeOfImage
    put32(0x98 + 60, 0x200);   // SizeOfHeaders
    const size_t section = 0x98 + 0xE0;
    put32(section + 8, 0x20);     // VirtualSize
    put32(section + 12, 0x1000);  // VirtualAddress
    put32(section + 16, 0x10);    // SizeOfRawData
    put32(section + 20, 0x200);   // PointerToRawData
    for (int i = 0; i < 0x10; ++i) file[0x200 + i] = uint8_t(0xA0 + i);

    pe::Image image;
    std::string error;
    CHECK(pe::map_image(file, image, error));
    CHECK_EQ(image.timestamp, 0x584a499eu);
    CHECK_EQ(image.size_of_image, 0x2000u);
    CHECK_EQ(image.bytes.size(), size_t(0x2000));
    CHECK_EQ(image.bytes[0x1000], uint8_t(0xA0));
    CHECK_EQ(image.bytes[0x100F], uint8_t(0xAF));
    CHECK_EQ(image.bytes[0x1010], uint8_t(0));

    put32(section + 20, 0x208);  // raw data now runs past the end of the file
    CHECK(!pe::map_image(file, image, error));
    file[0] = 'X';
    CHECK(!pe::map_image(file, image, error));
    CHECK_EQ(error, std::string("not a PE file"));
}

// The real mapping files against the local game install, if there is one
// (game files are never committed, so this is skipped elsewhere).
TEST(verify_embedded_mappings_against_local_game) {
    std::vector<uint8_t> file;
    if (!read_file(TOME_TEST_GAME_DIR "/game.dll", file)) {
        std::printf("  skipped: no %s/game.dll\n", TOME_TEST_GAME_DIR);
        return;
    }
    pe::Image image;
    std::string error;
    if (!pe::map_image(file, image, error)) {
        test::fail(__FILE__, __LINE__, "game.dll: " + error);
        return;
    }
    bool matched = false;
    for (size_t i = 0; i < mappings::kEmbeddedFileCount; ++i) {
        mappings::Build b;
        if (!parse_build_text(mappings::kEmbeddedFiles[i].text, b)) continue;
        if (b.pe_timestamp != image.timestamp || b.size_of_image != image.size_of_image) continue;
        matched = true;
        for (const std::string& e : mappings::verify_code(b, image.bytes.data(), image.bytes.size())) {
            test::fail(__FILE__, __LINE__, b.name + ": " + e);
        }
    }
    if (!matched) std::printf("  skipped: the local game.dll isn't a mapped build\n");
}

// mappings/blizzard.ax.json against the local install's blizzard.ax.
TEST(verify_blizzard_ax_mapping_against_local_game) {
    std::vector<uint8_t> file;
    if (!read_file(TOME_TEST_GAME_DIR "/blizzard.ax", file)) {
        std::printf("  skipped: no %s/blizzard.ax\n", TOME_TEST_GAME_DIR);
        return;
    }
    pe::Image image;
    std::string error;
    if (!pe::map_image(file, image, error)) {
        test::fail(__FILE__, __LINE__, "blizzard.ax: " + error);
        return;
    }
    bool matched = false;
    for (size_t i = 0; i < mappings::kEmbeddedFileCount; ++i) {
        mappings::Build b;
        if (!parse_build_text(mappings::kEmbeddedFiles[i].text, b) || b.module != "blizzard.ax") continue;
        if (b.pe_timestamp != image.timestamp || b.size_of_image != image.size_of_image) continue;
        matched = true;
        for (const std::string& e : mappings::verify_code(b, image.bytes.data(), image.bytes.size())) {
            test::fail(__FILE__, __LINE__, b.name + ": " + e);
        }
    }
    if (!matched) std::printf("  skipped: the local blizzard.ax isn't a mapped build\n");
}
