// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/desync.h"
#include "test.h"

using namespace tome::desync;

TEST(desync_encode_parse) {
    Part p = encode(12, "my-mod", 0x0123456789abcdefull);
    CHECK_EQ(p.mission, std::string("1 12 my-mod"));
    CHECK_EQ(p.key, std::string("0123456789abcdef"));
    uint32_t round = 0;
    std::string mod;
    uint64_t hash = 0;
    CHECK(parse(p.mission, p.key, round, mod, hash));
    CHECK_EQ(round, 12u);
    CHECK_EQ(mod, std::string("my-mod"));
    CHECK_EQ(hash, 0x0123456789abcdefull);
    CHECK(encode(0, "my-mod", 1).mission.empty());
    CHECK(encode(1, "Not Valid", 1).mission.empty());
}

TEST(desync_parse_refuses) {
    uint32_t round;
    std::string mod;
    uint64_t hash;
    const char* key = "0000000000000001";
    CHECK(!parse("2 1 my-mod", key, round, mod, hash));      // another version
    CHECK(!parse("1 0 my-mod", key, round, mod, hash));      // round 0
    CHECK(!parse("1 01 my-mod", key, round, mod, hash));     // leading zero
    CHECK(!parse("1 -1 my-mod", key, round, mod, hash));
    CHECK(!parse("1 99999999999 my-mod", key, round, mod, hash));
    CHECK(!parse("1 1", key, round, mod, hash));
    CHECK(!parse("1 1 ", key, round, mod, hash));
    CHECK(!parse("1 1 My Mod", key, round, mod, hash));
    CHECK(!parse("1", key, round, mod, hash));
    CHECK(!parse("", key, round, mod, hash));
    CHECK(!parse("1 1 my-mod", "1", round, mod, hash));
    CHECK(!parse("1 1 my-mod", "000000000000000G", round, mod, hash));
    CHECK(!parse("1 1 my-mod", "000000000000000A", round, mod, hash));  // uppercase
    CHECK(!parse("1 1 my-mod", "00000000000000001", round, mod, hash));
    CHECK(parse("1 1 my-mod", key, round, mod, hash));
}

TEST(desync_round_ending) {
    CHECK_EQ(round_ending(0, 5000), 0u);
    CHECK_EQ(round_ending(2995000, 3000000), 1u);
    CHECK_EQ(round_ending(3000000, 3005000), 0u);
    CHECK_EQ(round_ending(5900000, 6200000), 2u);
    CHECK_EQ(round_ending(1000000, 1000000), 0u);   // no step
    CHECK_EQ(round_ending(10000000, 9000000), 0u);  // backwards
    // Steps of 0.005 s, as the game takes them (a float), counted the way
    // events.cpp does: one round every 3 s, in order, each in the step that
    // reaches 3 s exactly.
    uint32_t expected = 1;
    tome::game_clock::Micros t = 0;
    for (int step = 0; step < 4000; ++step) {
        const tome::game_clock::Micros before = t;
        t += tome::game_clock::from_seconds(0.005f);
        const uint32_t r = round_ending(before, t);
        if (r) {
            CHECK_EQ(r, expected);
            CHECK_EQ(t, static_cast<tome::game_clock::Micros>(r) * kInterval);
            ++expected;
        }
    }
    CHECK_EQ(expected, 7u);  // 20 s: rounds 1 to 6
}

TEST(desync_checker_compares) {
    Checker c;
    std::vector<Comparison> out;
    c.local(1, "a", 10, out);
    c.local(1, "b", 20, out);
    c.remote(2, 1, "a", 10, out);
    CHECK_EQ(out.size(), 1u);
    CHECK(out[0].same());
    c.remote(2, 1, "b", 21, out);
    CHECK_EQ(out.size(), 2u);
    CHECK(!out[1].same());
    CHECK_EQ(out[1].player, 2);
    CHECK_EQ(out[1].mod_id, std::string("b"));
    CHECK_EQ(out[1].round, 1u);
    CHECK_EQ(out[1].local, 20u);
    CHECK_EQ(out[1].remote, 21u);
}

TEST(desync_checker_waits_for_local) {
    Checker c;
    std::vector<Comparison> out;
    c.local(1, "a", 1, out);
    // Player 3 is a round ahead: its hash waits for ours.
    c.remote(3, 2, "a", 5, out);
    c.remote(4, 2, "a", 6, out);
    CHECK(out.empty());
    CHECK_EQ(c.waiting(), 2u);
    c.local(2, "a", 6, out);
    CHECK_EQ(out.size(), 2u);
    CHECK_EQ(out[0].player, 3);
    CHECK(!out[0].same());
    CHECK_EQ(out[1].player, 4);
    CHECK(out[1].same());
    CHECK_EQ(c.waiting(), 0u);
}

TEST(desync_checker_ignores_mods_not_run_here) {
    Checker c;
    std::vector<Comparison> out;
    c.local(1, "a", 1, out);
    c.local(2, "a", 1, out);
    c.remote(2, 2, "other", 9, out);  // a round we passed without "other"
    c.remote(2, 1, "other", 9, out);
    CHECK(out.empty());
    CHECK_EQ(c.waiting(), 0u);
}

TEST(desync_checker_is_bounded) {
    Checker c;
    std::vector<Comparison> out;
    c.local(1, "a", 1, out);
    c.remote(2, 1 + kWindow + 1, "a", 9, out);  // too far ahead
    CHECK_EQ(c.waiting(), 0u);
    for (uint32_t i = 0; i < kMaxWaiting + 50; ++i) {
        c.remote(static_cast<int>(i % 24), 2 + i % kWindow, "m" + std::to_string(i), 1, out);
    }
    CHECK_EQ(c.waiting(), kMaxWaiting);
    // Old rounds go as the game goes on.
    for (uint32_t r = 2; r < 40; ++r) c.local(r, "a", 1, out);
    CHECK(c.kept() <= kWindow + 1);
    CHECK_EQ(c.waiting(), 0u);
    c.reset();
    CHECK_EQ(c.kept(), 0u);
}

TEST(desync_checker_reports_once) {
    Checker c;
    CHECK(c.first_report(2, "a", false));
    CHECK(!c.first_report(2, "a", false));
    CHECK(c.first_report(2, "a", true));  // a match is told apart from a difference
    CHECK(!c.first_report(2, "a", true));
    CHECK(c.first_report(3, "a", false));
    CHECK(c.first_report(2, "b", false));
    c.reset();
    CHECK(c.first_report(2, "a", false));
}

TEST(desync_format_game_time) {
    CHECK_EQ(format_game_time(0), std::string("0:00"));
    CHECK_EQ(format_game_time(42.9), std::string("0:42"));
    CHECK_EQ(format_game_time(62), std::string("1:02"));
    CHECK_EQ(format_game_time(3723), std::string("1:02:03"));
    CHECK_EQ(format_game_time(-5), std::string("0:00"));
}
