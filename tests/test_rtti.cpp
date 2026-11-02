// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/embedded_mappings.h"
#include "core/json.h"
#include "core/mappings.h"
#include "core/pe.h"
#include "core/rtti.h"
#include "test.h"

#include <cstdio>
#include <cstring>

using namespace tome;

namespace {

// A fake address space: `bytes` mapped at `base`.
struct Space {
    uint32_t base = 0x1000;
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x400, 0);

    void put32(uint32_t address, uint32_t value) { std::memcpy(&bytes[address - base], &value, 4); }
    void put_string(uint32_t address, const char* text) { std::memcpy(&bytes[address - base], text, std::strlen(text) + 1); }

    rtti::Memory memory() const {
        return {[](const void* ctx, uint32_t address, void* out, uint32_t size) {
                    auto self = static_cast<const Space*>(ctx);
                    if (address < self->base || address - self->base + size > self->bytes.size()) return false;
                    std::memcpy(out, &self->bytes[address - self->base], size);
                    return true;
                },
                this};
    }
};

// CButton derives from CControl, which derives from CFrame.
Space button_space() {
    Space s;
    const uint32_t types[] = {0x1100, 0x1140, 0x1180};
    s.put_string(types[0] + 8, ".?AVCButton@@");
    s.put_string(types[1] + 8, ".?AVCControl@@");
    s.put_string(types[2] + 8, ".?AVCFrame@@");
    const uint32_t descriptors = 0x1200, bases = 0x1240, hierarchy = 0x1260, locator = 0x1280, vtable = 0x12a4;
    for (uint32_t i = 0; i < 3; ++i) {
        s.put32(descriptors + i * 8, types[i]);
        s.put32(bases + i * 4, descriptors + i * 8);
    }
    s.put32(hierarchy + 8, 3);
    s.put32(hierarchy + 12, bases);
    s.put32(locator + 12, types[0]);
    s.put32(locator + 16, hierarchy);
    s.put32(vtable - 4, locator);
    return s;
}

constexpr uint32_t kButtonVtable = 0x12a4;

bool read_file(const char* path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return true;
}

} // namespace

TEST(rtti_reads_class_and_bases) {
    Space s = button_space();
    rtti::Memory m = s.memory();
    CHECK_EQ(rtti::class_name(m, kButtonVtable), std::string("CButton"));
    CHECK(rtti::derives_from(m, kButtonVtable, "CButton"));
    CHECK(rtti::derives_from(m, kButtonVtable, "CControl"));
    CHECK(rtti::derives_from(m, kButtonVtable, "CFrame"));
    CHECK(!rtti::derives_from(m, kButtonVtable, "CTextFrame"));
    CHECK(!rtti::derives_from(m, kButtonVtable, "CFram"));
}

TEST(rtti_rejects_unreadable_or_odd_data) {
    Space s = button_space();
    rtti::Memory m = s.memory();
    CHECK_EQ(rtti::class_name(m, 0x9000), std::string());       // outside the space
    CHECK(!rtti::derives_from(m, 0x9000, "CFrame"));
    CHECK_EQ(rtti::class_name(m, 0x1010), std::string());       // no locator there
    s.put32(0x1260 + 8, 100000);                                 // absurd base count
    CHECK(!rtti::derives_from(s.memory(), kButtonVtable, "CFrame"));
    s = button_space();
    s.put_string(0x1100 + 8, "CButton");                         // not a decorated name
    CHECK_EQ(rtti::class_name(s.memory(), kButtonVtable), std::string());
}

// The real game.dll, laid out at its preferred base (RTTI pointers are
// absolute, and the unrelocated file holds them for that base).
TEST(rtti_against_local_game) {
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
    for (size_t i = 0; i < mappings::kEmbeddedFileCount; ++i) {
        mappings::Build b;
        json::Value root;
        if (!json::parse(mappings::kEmbeddedFiles[i].text, root).ok || !mappings::parse_build(root, b).ok) continue;
        if (b.pe_timestamp != image.timestamp || b.size_of_image != image.size_of_image) continue;
        const mappings::Symbol* vtable = b.find("CMainMenu.vtable");
        if (!vtable) continue;
        struct Ctx {
            const pe::Image* image;
            uint32_t base;
        } ctx{&image, 0x6F000000};
        rtti::Memory m{[](const void* c, uint32_t address, void* out, uint32_t size) {
                           auto ctx = static_cast<const Ctx*>(c);
                           uint32_t rva = address - ctx->base;
                           if (address < ctx->base || rva + size > ctx->image->bytes.size()) return false;
                           std::memcpy(out, &ctx->image->bytes[rva], size);
                           return true;
                       },
                       &ctx};
        uint32_t va = ctx.base + vtable->value;
        CHECK_EQ(rtti::class_name(m, va), std::string("CMainMenu"));
        CHECK(rtti::derives_from(m, va, "CFrame"));
        CHECK(!rtti::derives_from(m, va, "CTextFrame"));
        return;
    }
    std::printf("  skipped: the local game.dll isn't a mapped build\n");
}
