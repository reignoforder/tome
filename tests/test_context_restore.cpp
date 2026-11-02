// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/context_restore.h"
#include "test.h"

using namespace tome;

TEST(context_restore_after_fullscreen_hide) {
    ContextRestore r;
    CHECK(!r.frame(true, true));
    CHECK(!r.frame(true, true));
    CHECK(!r.frame(false, true));  // minimized
    CHECK(!r.frame(false, true));
    CHECK(r.frame(true, true));    // back: rebuild once
    r.rebuilt(true);
    CHECK(!r.frame(true, true));
    CHECK_EQ(r.failures(), 0u);
}

TEST(context_restore_not_before_first_frame) {
    // CanDraw is false while the window is being made: nothing to lose yet.
    ContextRestore r;
    CHECK(!r.frame(false, true));
    CHECK(!r.frame(true, true));
    CHECK(!r.frame(true, true));
}

TEST(context_restore_not_in_a_window) {
    ContextRestore r;
    CHECK(!r.frame(true, false));
    CHECK(!r.frame(false, false));  // windowed and minimized
    CHECK(!r.frame(true, false));
    // Fullscreen at any point while hidden is enough.
    CHECK(!r.frame(false, false));
    CHECK(!r.frame(false, true));
    CHECK(r.frame(true, true));
}

TEST(context_restore_retries_a_failed_rebuild) {
    ContextRestore r;
    r.frame(true, true);
    r.frame(false, true);
    CHECK(r.frame(true, true));
    r.rebuilt(false);
    CHECK_EQ(r.failures(), 1u);
    CHECK(r.frame(true, true));  // still due
    r.rebuilt(false);
    CHECK_EQ(r.failures(), 2u);
    CHECK(!r.frame(false, true));  // hidden again: waits, stays due
    CHECK(r.frame(true, true));
    r.rebuilt(true);
    CHECK_EQ(r.failures(), 0u);
    CHECK(!r.frame(true, true));
}
