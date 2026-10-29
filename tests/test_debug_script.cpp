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
    CHECK_EQ(steps.size(), size_t(1));
    if (!steps.empty()) CHECK_EQ(steps[0].at_ms, uint32_t(30));
    CHECK_EQ(bad.size(), size_t(8));  // the empty item between ";;" is skipped quietly
    if (!bad.empty()) CHECK_EQ(bad[0], std::string("0.5,0.5"));
}

TEST(debug_script_wait) {
    std::vector<std::string> bad;
    auto steps = parse("wait:Screen: custom game@0;0.5,0.5@1500;wait:a@b@200;wait:x;wait:@5", bad);
    CHECK_EQ(steps.size(), size_t(3));
    if (steps.size() != 3) return;
    CHECK(steps[0].kind == Kind::Wait);
    CHECK_EQ(steps[0].text, std::string("Screen: custom game"));
    CHECK_EQ(steps[0].at_ms, uint32_t(0));
    CHECK(steps[2].kind == Kind::Wait);
    CHECK_EQ(steps[2].text, std::string("a@b"));
    CHECK_EQ(steps[2].at_ms, uint32_t(200));
    CHECK_EQ(bad.size(), size_t(2));  // "wait:a" has no time, "wait:@5" no text
}

TEST(debug_script_empty) {
    std::vector<std::string> bad;
    CHECK(parse("", bad).empty());
    CHECK(parse(";", bad).empty());
    CHECK(bad.empty());
}
