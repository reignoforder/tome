// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/tome_settings.h"
#include "core/widescreen.h"
#include "test.h"

#include <cmath>
#include <string>
#include <vector>

using namespace tome::widescreen;

namespace {
bool near(float a, float b) { return std::fabs(a - b) < 1e-6f; }
} // namespace

TEST(widescreen_four_by_three_unchanged) {
    for (auto [w, h] : {std::pair{800, 600}, {1024, 768}, {1600, 1200}}) {
        Scale s = projection_scale(w, h);
        CHECK(s.x == 1.0f);
        CHECK(s.y == 1.0f);
    }
}

TEST(widescreen_wider_shows_more_at_the_sides) {
    Scale s = projection_scale(1920, 1080);  // 16:9
    CHECK(near(s.x, 0.75f));                 // (4/3) / (16/9)
    CHECK(s.y == 1.0f);
    s = projection_scale(1680, 1050);  // 16:10
    CHECK(near(s.x, (4.0f / 3.0f) / 1.6f));
    s = projection_scale(3440, 1440);  // 21:9
    CHECK(near(s.x, (4.0f / 3.0f) / (3440.0f / 1440.0f)));
}

TEST(widescreen_narrower_shows_more_at_the_top) {
    Scale s = projection_scale(1280, 1024);  // 5:4
    CHECK(s.x == 1.0f);
    CHECK(near(s.y, (5.0f / 4.0f) / (4.0f / 3.0f)));
}

TEST(widescreen_empty_area) {
    for (auto [w, h] : {std::pair{0, 0}, {1280, 0}, {0, 720}, {-1, 600}}) {
        Scale s = projection_scale(w, h);
        CHECK(s.x == 1.0f);
        CHECK(s.y == 1.0f);
    }
}

TEST(tome_settings_widescreen_defaults_on) {
    using namespace tome;
    const settings::Setting* s = tome_settings::schema().find(tome_settings::kWidescreen);
    CHECK(s != nullptr);
    CHECK(s->type == settings::Type::Bool);
    CHECK(tome_settings::get_bool(json::Value::object(), tome_settings::kWidescreen));
    json::Value values = json::Value::object();
    values.set(tome_settings::kWidescreen, json::Value::boolean(false));
    CHECK(!tome_settings::get_bool(values, tome_settings::kWidescreen));
    values.set(tome_settings::kWidescreen, json::Value::string("off"));  // wrong type: the default
    CHECK(tome_settings::get_bool(values, tome_settings::kWidescreen));
}

TEST(widescreen_ui_width) {
    using tome::widescreen::ui_width;
    CHECK_EQ(ui_width(800, 600), 0.8f);
    CHECK_EQ(ui_width(1024, 768), 0.8f);
    CHECK_EQ(ui_width(1280, 1024), 0.8f);  // narrower than 4:3 keeps the game's width
    CHECK_EQ(ui_width(0, 0), 0.8f);
    CHECK(std::fabs(ui_width(1920, 1080) - 0.6f * 16 / 9) < 1e-6f);
    CHECK(std::fabs(ui_width(2560, 1080) - 0.6f * 2560 / 1080) < 1e-6f);
}

namespace {
// The pieces of console_layout, by name, in order.
std::vector<tome::widescreen::Piece> named(const std::vector<tome::widescreen::Piece>& all, const char* name) {
    std::vector<tome::widescreen::Piece> out;
    for (const auto& p : all) {
        if (std::string(p.name) == name) out.push_back(p);
    }
    return out;
}
bool near_eq(float a, float b) {
    return std::fabs(a - b) < 1e-5f;
}
} // namespace

TEST(widescreen_console_at_4_3_is_the_games_bottom_row) {
    using namespace tome::widescreen;
    auto all = console_layout(0.8f);
    CHECK(named(all, "TomeWideConsoleWall").empty());
    CHECK(named(all, "TomeWideConsoleSlot").empty());
    // Tiles 1 to 4 at 0, 0.256, 0.512, 0.768, as in ConsoleUI.fdf.
    CHECK(near_eq(named(all, "TomeWideConsoleLeft")[0].x, 0.0f));
    CHECK(near_eq(named(all, "TomeWideConsoleLeft")[0].width + named(all, "TomeWideConsoleArch")[0].width, 0.256f));
    CHECK(near_eq(named(all, "TomeWideConsoleInfo")[0].x, 0.256f));
    CHECK(near_eq(named(all, "TomeWideConsoleInventory")[0].x, 0.512f));
    CHECK(near_eq(named(all, "TomeWideConsoleEdge")[0].x, 0.768f));
    CHECK(near_eq(named(all, "TomeWideConsoleEdge")[0].width, 0.032f));
    CHECK_EQ(console_layout(0.5f).size(), all.size());  // narrower counts as 0.8
}

TEST(widescreen_console_wide_has_no_gaps) {
    using namespace tome::widescreen;
    for (float wide : {0.9f, 1.0667f, 1.4222f, 2.1333f}) {
        auto all = console_layout(wide);
        // The bottom row, left to right, touches end to end from 0 to `wide`.
        float x = 0.0f;
        for (const auto& p : all) {
            if (p.top) continue;
            CHECK(near_eq(p.x, x));
            CHECK(p.width > 0.0f);
            x = p.x + p.width;
        }
        CHECK(near_eq(x, wide));
        // The middle moved right by half the extra width: it stays centered.
        const float e = middle_shift(wide);
        CHECK(near_eq(named(all, "TomeWideConsoleInfo")[0].x, 0.256f + e));
        // The top bar's filler spans the gap each side of the clock.
        auto slots = named(all, "TomeWideConsoleSlot");
        CHECK(!slots.empty());
        float filled = 0.0f;
        for (const auto& s : slots) filled += s.width;
        CHECK(near_eq(filled, 2 * e));
        CHECK(near_eq(slots.front().x, 0.343f));
        CHECK(near_eq(slots.back().x + slots.back().width, wide - 0.341f));
    }
}

TEST(widescreen_console_fdf_defines_every_piece) {
    using namespace tome::widescreen;
    const std::string fdf = console_fdf();
    for (const auto& p : console_layout(1.4f)) CHECK(fdf.find(std::string("\"") + p.name + "\"") != std::string::npos);
    CHECK(fdf.find("DecorateFileNames") != std::string::npos);
    CHECK(fdf.find("ConsoleTexture04") != std::string::npos);
}

// DDCToNDCRect, DDCToNDC and NDCToDDCWidth as on 1.27b, the 0.8 at 0x6faa9328
// (x) and the 0.6 at 0x6faa9324 (y).
TEST(widescreen_constant_operands) {
    const uint8_t rect[] = {
        0xf3, 0x0f, 0x10, 0x01, 0xf3, 0x0f, 0x5e, 0x05, 0x28, 0x93, 0xaa, 0x6f, 0xf3, 0x0f, 0x11, 0x02,
        0xf3, 0x0f, 0x10, 0x41, 0x08, 0xf3, 0x0f, 0x5e, 0x05, 0x28, 0x93, 0xaa, 0x6f, 0xf3, 0x0f, 0x11,
        0x42, 0x08, 0xf3, 0x0f, 0x10, 0x41, 0x04, 0xf3, 0x0f, 0x5e, 0x05, 0x24, 0x93, 0xaa, 0x6f, 0xf3,
        0x0f, 0x11, 0x42, 0x04, 0xc3, 0xcc,
    };
    std::vector<size_t> at;
    CHECK(constant_operands(rect, sizeof rect, 0x6faa9328, at));
    CHECK(at.size() == 2);
    CHECK(at.size() == 2 && at[0] == 8 && at[1] == 25);
    CHECK(constant_operands(rect, sizeof rect, 0x6faa9324, at));
    CHECK(at.size() == 1 && at[0] == 43);

    const uint8_t point[] = {
        0x55, 0x8b, 0xec, 0x85, 0xc9, 0x74, 0x11, 0xf3, 0x0f, 0x10, 0x45, 0x08, 0xf3, 0x0f, 0x5e, 0x05,
        0x28, 0x93, 0xaa, 0x6f, 0xf3, 0x0f, 0x11, 0x01, 0x85, 0xd2, 0x74, 0x11, 0xf3, 0x0f, 0x10, 0x45,
        0x0c, 0xf3, 0x0f, 0x5e, 0x05, 0x24, 0x93, 0xaa, 0x6f, 0xf3, 0x0f, 0x11, 0x02, 0x5d, 0xc2, 0x08, 0x00,
    };
    CHECK(constant_operands(point, sizeof point, 0x6faa9328, at));
    CHECK(at.size() == 1 && at[0] == 16);

    const uint8_t width[] = {0x55, 0x8b, 0xec, 0xd9, 0x45, 0x08, 0xd8, 0x0d, 0x28, 0x93, 0xaa, 0x6f, 0x5d, 0xc2, 0x04, 0x00};
    CHECK(constant_operands(width, sizeof width, 0x6faa9328, at));
    CHECK(at.size() == 1 && at[0] == 8);

    // 1.26a's x87 forms: the 0.8 is a double at 0x6f8768b0, read once by the
    // Rect forms (fld QWORD, then fmul st(1),st reused), by fmul/fdiv QWORD
    // in the others.
    const uint8_t rect_x87[] = {
        0xd9, 0x01, 0xdd, 0x05, 0xb0, 0x68, 0x87, 0x6f, 0xdc, 0xc9, 0xd9, 0xc9, 0xd9, 0x1a, 0xd8, 0x49,
        0x08, 0xd9, 0x5a, 0x08, 0xd9, 0x41, 0x04, 0xdd, 0x05, 0x18, 0xc2, 0x93, 0x6f, 0xdc, 0xc9, 0xd9,
        0xc9, 0xd9, 0x5a, 0x04, 0xd8, 0x49, 0x0c, 0xd9, 0x5a, 0x0c, 0xc3,
    };
    CHECK(constant_operands(rect_x87, sizeof rect_x87, 0x6f8768b0, at));
    CHECK(at.size() == 1 && at[0] == 4);
    const uint8_t point_x87[] = {
        0x85, 0xc9, 0x74, 0x0c, 0xd9, 0x44, 0x24, 0x04, 0xdc, 0x35, 0xb0, 0x68, 0x87, 0x6f, 0xd9, 0x19,
        0x85, 0xd2, 0x74, 0x0c, 0xd9, 0x44, 0x24, 0x08, 0xdc, 0x35, 0x18, 0xc2, 0x93, 0x6f, 0xd9, 0x1a,
        0xc2, 0x08, 0x00,
    };
    CHECK(constant_operands(point_x87, sizeof point_x87, 0x6f8768b0, at));
    CHECK(at.size() == 1 && at[0] == 10);

    // No ret in what's readable, or an opcode the decoder doesn't know.
    CHECK(!constant_operands(width, 12, 0x6faa9328, at));
    const uint8_t unknown[] = {0x0f, 0x04, 0xc3};  // undefined
    CHECK(!constant_operands(unknown, sizeof unknown, 0x6faa9328, at));
}
