// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/debug_script.h"
#include "test.h"

using namespace tome::debug_script;

TEST(debug_script_parses_every_kind) {
    std::vector<std::string> bad;
    auto steps = parse("0.5,0.25@1000;type:/tome list@2000;text:a@b@2500;key:Ctrl+J@3000;"
                       "minimize@4000;restore@4500;close@5000",
                       bad);
    CHECK(bad.empty());
    CHECK_EQ(steps.size(), size_t(7));
    if (steps.size() != 7) return;
    CHECK(steps[0].kind == Kind::Click);
    CHECK(steps[0].x == 0.5 && steps[0].y == 0.25);
    CHECK_EQ(steps[0].at_ms, uint32_t(1000));
    CHECK(steps[1].kind == Kind::Type);
    CHECK_EQ(steps[1].text, std::string("/tome list"));
    CHECK(steps[2].kind == Kind::Text);
    CHECK_EQ(steps[2].text, std::string("a@b"));  // the time comes after the last '@'
    CHECK_EQ(steps[2].at_ms, uint32_t(2500));
    CHECK(steps[3].kind == Kind::Key);
    CHECK_EQ(steps[3].text, std::string("Ctrl+J"));
    CHECK(steps[4].kind == Kind::Minimize);
    CHECK(steps[5].kind == Kind::Restore);
    CHECK(steps[6].kind == Kind::Close);
    CHECK_EQ(steps[6].at_ms, uint32_t(5000));
}

TEST(debug_script_move) {
    std::vector<std::string> bad;
    auto steps = parse("move:0,1@100;move:0.5@5;move:,0.5@5", bad);
    CHECK_EQ(steps.size(), size_t(1));
    if (!steps.empty()) {
        CHECK(steps[0].kind == Kind::Move);
        CHECK(steps[0].x == 0.0 && steps[0].y == 1.0);
        CHECK_EQ(steps[0].at_ms, uint32_t(100));
    }
    CHECK_EQ(bad.size(), size_t(2));
}

TEST(debug_script_reports_bad_items) {
    std::vector<std::string> bad;
    auto steps = parse("0.5,0.5;close@x;0.1,0.2@30;;close@10extra;key:@5;minimize@;-1,0.5@5;0.5@5;closer@5", bad);
    CHECK_EQ(steps.size(), size_t(2));  // a step needs no time
    if (steps.size() == 2) {
        CHECK_EQ(steps[0].at_ms, uint32_t(0));
        CHECK_EQ(steps[1].at_ms, uint32_t(30));
    }
    CHECK_EQ(bad.size(), size_t(7));  // the empty item between ";;" is skipped quietly
    if (!bad.empty()) CHECK_EQ(bad[0], std::string("close@x"));
}

TEST(debug_script_wait) {
    std::vector<std::string> bad;
    auto steps = parse("wait:Screen: custom game@0;0.5,0.5@1500;wait:a@b@200;wait:x;wait:@5;wait:a@b", bad);
    CHECK_EQ(steps.size(), size_t(5));
    if (steps.size() != 5) return;
    CHECK(steps[0].kind == Kind::Wait);
    CHECK_EQ(steps[0].text, std::string("Screen: custom game"));
    CHECK_EQ(steps[0].at_ms, uint32_t(0));
    CHECK(steps[2].kind == Kind::Wait);
    CHECK_EQ(steps[2].text, std::string("a@b"));
    CHECK_EQ(steps[2].at_ms, uint32_t(200));
    CHECK_EQ(steps[3].text, std::string("x"));
    CHECK_EQ(steps[4].text, std::string("a@b"));  // "@b" isn't a time
    CHECK_EQ(bad.size(), size_t(1));  // "wait:@5" has no text
}

TEST(debug_script_frames_and_until) {
    std::vector<std::string> bad;
    auto steps = parse("wait:Screen ready: main menu;0.86,0.245;until:Screen: single player;frames:30;"
                       "frames:0;frames:;frames:x;frames:1234567;until:",
                       bad);
    CHECK_EQ(steps.size(), size_t(5));
    if (steps.size() != 5) return;
    CHECK(steps[1].kind == Kind::Click);
    CHECK(steps[2].kind == Kind::Until);
    CHECK_EQ(steps[2].text, std::string("Screen: single player"));
    CHECK(steps[3].kind == Kind::Frames);
    CHECK_EQ(steps[3].frames, uint32_t(30));
    CHECK_EQ(steps[4].frames, uint32_t(0));
    CHECK_EQ(bad.size(), size_t(4));  // no number, not a number, too many digits, no text
    steps = parse("settle;settle@100", bad);
    CHECK_EQ(steps.size(), size_t(2));
    if (steps.size() == 2) CHECK(steps[0].kind == Kind::Settle && steps[1].at_ms == 100);
}

TEST(debug_script_click_frame) {
    std::vector<std::string> bad;
    auto steps = parse("click:TomeModsButton;click:HelloButton,13;click:;click:a b;click:X,;click:X,12345;click:X,y", bad);
    CHECK_EQ(steps.size(), size_t(2));
    if (steps.size() == 2) {
        CHECK(steps[0].kind == Kind::ClickFrame);
        CHECK_EQ(steps[0].text, std::string("TomeModsButton"));
        CHECK_EQ(steps[0].context, 0);
        CHECK_EQ(steps[1].text, std::string("HelloButton"));
        CHECK_EQ(steps[1].context, 13);
    }
    CHECK_EQ(bad.size(), size_t(5));
    steps = parse("focus:TomeModsRow0Edit;focus:Box,2;focus:", bad);
    CHECK_EQ(steps.size(), size_t(2));
    if (steps.size() == 2) {
        CHECK(steps[0].kind == Kind::Focus);
        CHECK_EQ(steps[0].text, std::string("TomeModsRow0Edit"));
        CHECK_EQ(steps[1].context, 2);
    }
}

TEST(debug_script_empty) {
    std::vector<std::string> bad;
    CHECK(parse("", bad).empty());
    CHECK(parse(";", bad).empty());
    CHECK(bad.empty());
}
