// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/frames.h"
#include "test.h"

using namespace tome::ui;

TEST(frame_table_ids) {
    FrameTable t;
    CHECK_EQ(t.remember(0, "Nothing", 0), 0);
    CHECK_EQ(t.size(), size_t{0});
    CHECK_EQ(t.remember(0x1000, "A", 0), 1);
    CHECK_EQ(t.remember(0x2000, "B", 3), 2);
    CHECK_EQ(t.remember(0x1000, "A", 0), 1);  // same frame, same id
    CHECK_EQ(t.size(), size_t{2});

    const FrameRecord* b = t.by_id(2);
    CHECK(b != nullptr);
    CHECK_EQ(b->frame, uintptr_t{0x2000});
    CHECK_EQ(b->name, std::string("B"));
    CHECK_EQ(b->context, 3);
    CHECK(t.by_frame(0x2000) == b);

    CHECK(t.by_id(0) == nullptr);
    CHECK(t.by_id(-1) == nullptr);
    CHECK(t.by_id(3) == nullptr);
    CHECK(t.by_frame(0x3000) == nullptr);
}

TEST(frame_table_reused_address) {
    FrameTable t;
    CHECK_EQ(t.remember(0x1000, "Old", 0), 1);
    CHECK_EQ(t.remember(0x1000, "New", 2), 1);
    CHECK_EQ(t.by_id(1)->name, std::string("New"));
    CHECK_EQ(t.by_id(1)->context, 2);
}

TEST(mod_fdf_paths) {
    CHECK(is_valid_mod_fdf_path("hello.fdf"));
    CHECK(is_valid_mod_fdf_path("ui/hello.FDF"));
    CHECK(is_valid_mod_fdf_path("ui\\menus\\hello.fdf"));
    CHECK(!is_valid_mod_fdf_path(""));
    CHECK(!is_valid_mod_fdf_path(".fdf"));
    CHECK(!is_valid_mod_fdf_path("hello.txt"));
    CHECK(!is_valid_mod_fdf_path("../other/hello.fdf"));
    CHECK(!is_valid_mod_fdf_path("ui/../../hello.fdf"));
    CHECK(!is_valid_mod_fdf_path("/abs/hello.fdf"));
    CHECK(!is_valid_mod_fdf_path("C:\\abs\\hello.fdf"));
    CHECK(!is_valid_mod_fdf_path("ui//hello.fdf"));
    CHECK(!is_valid_mod_fdf_path("ui/./hello.fdf"));

    CHECK_EQ(mod_fdf_archive_path("hello", "ui/hello.fdf"), std::string("UI\\FrameDef\\Tome\\hello\\ui\\hello.fdf"));
    CHECK_EQ(toc_for("UI\\X.fdf"), std::string("UI\\X.fdf\r\n\r\n"));
}
