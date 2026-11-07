// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/log_guard.h"
#include "test.h"

#include <string>

using tome::log::Level;
using tome::log::printable;
using tome::log::SizeCap;

TEST(log_printable_keeps_plain_text) {
    CHECK_EQ(printable("Hello, Tome!", 100), std::string("Hello, Tome!"));
    CHECK_EQ(printable("", 100), std::string(""));
    // UTF-8 stays as it is.
    CHECK_EQ(printable("Hello, \xce\x96\xce\xb5\xcf\x8d\xcf\x82!", 100), std::string("Hello, \xce\x96\xce\xb5\xcf\x8d\xcf\x82!"));
}

TEST(log_printable_replaces_control_characters) {
    // A newline can't start a forged line.
    CHECK_EQ(printable("a\r\n[12:00:00.000] [    1] [ERROR] forged", 100),
             std::string("a??[12:00:00.000] [    1] [ERROR] forged"));
    CHECK_EQ(printable(std::string("x\0y\ty\x1b[31m\x7f", 11), 100), std::string("x?y?y?[31m?"));
}

TEST(log_printable_cuts_long_text) {
    CHECK_EQ(printable(std::string(10, 'a'), 10), std::string(10, 'a'));
    CHECK_EQ(printable(std::string(11, 'a'), 10), std::string(10, 'a') + "...");
    CHECK_EQ(printable("abc", 0), std::string("..."));
    // Not inside a UTF-8 character: "\xce\x96" (2 bytes) doesn't fit in 4 after "abc".
    CHECK_EQ(printable("abc\xce\x96zz", 4), std::string("abc..."));
    CHECK_EQ(printable("abc\xce\x96zz", 5), std::string("abc\xce\x96..."));
    // A 4-byte character cut anywhere goes whole.
    CHECK_EQ(printable("\xf0\x9f\x98\x80", 3), std::string("..."));
}

TEST(log_size_cap_writes_below_the_soft_limit) {
    SizeCap cap(100, 150);
    for (int i = 0; i < 10; ++i) {
        auto v = cap.take(Level::Info, 10);
        CHECK(v.write);
        CHECK(!v.notice);
    }
    CHECK_EQ(cap.written(), uint64_t{100});
}

TEST(log_size_cap_keeps_only_warnings_past_the_soft_limit) {
    SizeCap cap(100, 150);
    for (int i = 0; i < 10; ++i) cap.take(Level::Info, 10);
    auto info = cap.take(Level::Info, 10);
    CHECK(!info.write);
    CHECK(info.notice);  // the first dropped line brings the notice
    cap.count(20);
    auto debug = cap.take(Level::Debug, 10);
    CHECK(!debug.write);
    CHECK(!debug.notice);  // only once
    CHECK(cap.take(Level::Warn, 10).write);
    CHECK(cap.take(Level::Error, 10).write);
    CHECK_EQ(cap.written(), uint64_t{140});
    // Past the hard limit nothing goes in, errors neither.
    CHECK(!cap.take(Level::Error, 20).write);
    CHECK(cap.take(Level::Error, 10).write);  // still fits exactly
    CHECK(!cap.take(Level::Error, 1).write);
    CHECK_EQ(cap.written(), uint64_t{150});
}

TEST(log_size_cap_notices_a_warning_crossing_the_soft_limit) {
    SizeCap cap(100, 150);
    cap.take(Level::Info, 95);
    auto warn = cap.take(Level::Warn, 10);
    CHECK(warn.write);
    CHECK(warn.notice);  // later info lines are dropped: say so before this one
    CHECK(!cap.take(Level::Info, 1).notice);
}

TEST(log_size_cap_default_limits) {
    CHECK_EQ(SizeCap::kSoft, uint64_t{32} << 20);
    CHECK(SizeCap::kHard > SizeCap::kSoft);
}
