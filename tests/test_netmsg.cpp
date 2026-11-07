// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/netmsg.h"
#include "test.h"

#include <string>

using namespace tome::netmsg;

namespace {

// Feeds every part to `r` from `sender`; the last result.
Reassembler::Result feed(Reassembler& r, int sender, const std::vector<Part>& parts, Message& out) {
    Reassembler::Result last = Reassembler::Result::Bad;
    for (const Part& p : parts) last = r.add(sender, p.mission, p.key, p.value, out);
    return last;
}

} // namespace

TEST(netmsg_round_trip) {
    for (size_t size : {size_t(0), size_t(1), kChunk - 1, kChunk, kChunk + 1, size_t(500), kMaxMessage}) {
        std::string data;
        for (size_t i = 0; i < size; ++i) data.push_back(static_cast<char>(i * 7));  // NULs included
        std::vector<Part> parts = encode("my-mod", 42, data);
        CHECK_EQ(parts.size(), size == 0 ? size_t(1) : (size + kChunk - 1) / kChunk);
        for (const Part& p : parts) {
            CHECK_EQ(p.mission, std::string("1 42 my-mod"));
            CHECK(p.key.size() <= 252);  // fits the key's 0x100 bytes with its NUL
            CHECK(p.key.find('\0') == std::string::npos);
        }
        Reassembler r;
        Message m;
        CHECK(feed(r, 3, parts, m) == Reassembler::Result::Complete);
        CHECK_EQ(m.mod_id, std::string("my-mod"));
        CHECK(m.data == data);
        CHECK_EQ(r.pending(), size_t(0));
    }
}

TEST(netmsg_encode_refuses) {
    CHECK(encode("Not An Id", 1, "x").empty());
    CHECK(encode("", 1, "x").empty());
    CHECK(encode("m", 1, "x").empty());  // ids have two characters at least
    CHECK(encode("my-mod", 1, std::string(kMaxMessage + 1, 'x')).empty());
    CHECK_EQ(kMaxParts, 6);
}

TEST(netmsg_mission) {
    uint32_t id = 0;
    std::string mod;
    CHECK(parse_mission("1 7 hello", id, mod));
    CHECK_EQ(id, 7u);
    CHECK_EQ(mod, std::string("hello"));
    for (const char* bad : {"", "1", "1 ", "1 7", "1 7 ", "2 7 hello", "1 x hello", "1 -1 hello", "1 7 Hello",
                            "17 hello", "1 99999999999 hello", "1  7 hello"}) {
        if (parse_mission(bad, id, mod)) test::fail(__FILE__, __LINE__, std::string("accepted: ") + bad);
    }
}

TEST(netmsg_senders_interleave) {
    // Two players' messages in progress at once don't mix.
    std::string a(300, 'a'), b(300, 'b');
    std::vector<Part> pa = encode("mm", 1, a), pb = encode("mm", 1, b);
    CHECK(pa.size() == 2 && pb.size() == 2);
    if (pa.size() != 2 || pb.size() != 2) return;
    Reassembler r;
    Message m;
    CHECK(r.add(1, pa[0].mission, pa[0].key, pa[0].value, m) == Reassembler::Result::Partial);
    CHECK(r.add(2, pb[0].mission, pb[0].key, pb[0].value, m) == Reassembler::Result::Partial);
    CHECK(r.add(1, pa[1].mission, pa[1].key, pa[1].value, m) == Reassembler::Result::Complete);
    CHECK(m.data == a);
    CHECK(r.add(2, pb[1].mission, pb[1].key, pb[1].value, m) == Reassembler::Result::Complete);
    CHECK(m.data == b);
}

TEST(netmsg_bad_parts_are_dropped) {
    std::string data(400, 'x');
    std::vector<Part> parts = encode("mm", 5, data);  // 3 parts
    CHECK_EQ(parts.size(), size_t(3));
    if (parts.size() != 3) return;
    Reassembler r;
    Message m;
    // Out of order: the sender's message in progress goes too.
    CHECK(r.add(1, parts[0].mission, parts[0].key, parts[0].value, m) == Reassembler::Result::Partial);
    CHECK(r.add(1, parts[2].mission, parts[2].key, parts[2].value, m) == Reassembler::Result::Bad);
    CHECK_EQ(r.pending(), size_t(0));
    CHECK(r.add(1, parts[1].mission, parts[1].key, parts[1].value, m) == Reassembler::Result::Bad);
    // A part of another message (id or mod) in the middle.
    CHECK(r.add(1, parts[0].mission, parts[0].key, parts[0].value, m) == Reassembler::Result::Partial);
    CHECK(r.add(1, "1 6 mm", parts[1].key, parts[1].value, m) == Reassembler::Result::Bad);
    CHECK(r.add(1, parts[0].mission, parts[0].key, parts[0].value, m) == Reassembler::Result::Partial);
    CHECK(r.add(1, "1 5 nn", parts[1].key, parts[1].value, m) == Reassembler::Result::Bad);
    // A new message starts over the old one.
    CHECK(r.add(1, parts[0].mission, parts[0].key, parts[0].value, m) == Reassembler::Result::Partial);
    CHECK(feed(r, 1, parts, m) == Reassembler::Result::Complete);
    CHECK(m.data == data);
    // Malformed values and keys.
    for (int32_t value : {0, 1 | (1 << 8), 7 << 8, 1 << 16, -1}) {
        CHECK(r.add(1, "1 1 mm", "eA==", value, m) == Reassembler::Result::Bad);
    }
    CHECK(r.add(1, "1 1 mm", "not base64!", 1 << 8, m) == Reassembler::Result::Bad);
    // A short part that isn't the last.
    CHECK(r.add(1, "1 1 mm", "eA==", 2 << 8, m) == Reassembler::Result::Bad);
    // A chunk over kChunk bytes.
    std::string big = encode("mm", 1, std::string(kChunk, 'x'))[0].key + "eHh4";
    CHECK(r.add(1, "1 1 mm", big, 1 << 8, m) == Reassembler::Result::Bad);
    r.reset();
    CHECK_EQ(r.pending(), size_t(0));
}
