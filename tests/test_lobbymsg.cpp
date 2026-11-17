// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/lobbymsg.h"
#include "test.h"

#include <string>

using namespace tome::lobbymsg;

namespace {

// Feeds every line to `r` from `sender`; the last result.
Reassembler::Result feed(Reassembler& r, int sender, const std::vector<std::string>& lines, Message& out) {
    Reassembler::Result last = Reassembler::Result::Bad;
    for (const std::string& l : lines) last = r.add(sender, l, out);
    return last;
}

std::string bytes(size_t size) {
    std::string data;
    for (size_t i = 0; i < size; ++i) data.push_back(static_cast<char>(i * 7));  // NULs included
    return data;
}

} // namespace

TEST(lobbymsg_round_trip) {
    for (size_t size : {size_t(0), size_t(1), kChunk - 1, kChunk, kChunk + 1, size_t(500), kMaxMessage}) {
        const std::string data = bytes(size);
        std::vector<std::string> lines = encode("my-mod", 42, data);
        CHECK_EQ(lines.size(), size == 0 ? size_t(1) : (size + kChunk - 1) / kChunk);
        for (const std::string& l : lines) {
            CHECK(l.rfind("[Tome] Msg 1 42 my-mod ", 0) == 0);
            CHECK(l.size() <= kMaxLine);
            CHECK(l.find('\0') == std::string::npos);
            CHECK(is_message_line(l));
        }
        Reassembler r;
        Message m;
        CHECK(feed(r, 3, lines, m) == Reassembler::Result::Complete);
        CHECK_EQ(m.mod_id, std::string("my-mod"));
        CHECK(m.data == data);
        CHECK_EQ(r.pending(), size_t(0));
    }
}

TEST(lobbymsg_longest_line_fits) {
    const std::string id(64, 'a');
    for (const std::string& l : encode(id, 4294967295u, bytes(kMaxMessage))) CHECK(l.size() <= kMaxLine);
    CHECK_EQ(encode(id, 1, bytes(kMaxMessage)).size(), size_t(kMaxParts));
}

TEST(lobbymsg_encode_refuses) {
    CHECK(encode("Not A Mod", 1, "x").empty());
    CHECK(encode("", 1, "x").empty());
    CHECK(encode("my-mod", 1, bytes(kMaxMessage + 1)).empty());
}

TEST(lobbymsg_people_and_other_lines) {
    Reassembler r;
    Message m;
    CHECK(r.add(1, "hello there", m) == Reassembler::Result::NotOurs);
    CHECK(r.add(1, "[Tome] Required mods 1/1: none", m) == Reassembler::Result::NotOurs);
    CHECK(r.add(1, "[Tome] Message for you", m) == Reassembler::Result::NotOurs);
    // Typed by hand to look like one, but not well formed: dropped, not shown
    // (the prefix is Tome's; lobby_messages.cpp shows nothing either way).
    CHECK(r.add(1, "[Tome] Msg hi", m) == Reassembler::Result::Bad);
}

TEST(lobbymsg_interleaved_senders) {
    const std::vector<std::string> a = encode("a-mod", 1, bytes(300));
    const std::vector<std::string> b = encode("b-mod", 9, bytes(200));
    Reassembler r;
    Message m;
    CHECK(r.add(1, a[0], m) == Reassembler::Result::Partial);
    CHECK(r.add(2, b[0], m) == Reassembler::Result::Partial);
    CHECK(r.add(1, a[1], m) == Reassembler::Result::Partial);
    CHECK(r.add(2, b[1], m) == Reassembler::Result::Partial);
    CHECK(r.add(1, a[2], m) == Reassembler::Result::Partial);
    CHECK(r.add(2, b[2], m) == Reassembler::Result::Complete);
    CHECK_EQ(m.mod_id, std::string("b-mod"));
    CHECK(r.add(1, a[3], m) == Reassembler::Result::Complete);
    CHECK_EQ(m.mod_id, std::string("a-mod"));
    CHECK(m.data == bytes(300));
}

TEST(lobbymsg_bad_parts) {
    const std::vector<std::string> lines = encode("my-mod", 5, bytes(300));
    Message m;
    {
        Reassembler r;  // a part out of order drops the message
        CHECK(r.add(1, lines[0], m) == Reassembler::Result::Partial);
        CHECK(r.add(1, lines[2], m) == Reassembler::Result::Bad);
        CHECK_EQ(r.pending(), size_t(0));
        CHECK(r.add(1, lines[3], m) == Reassembler::Result::Bad);
    }
    {
        Reassembler r;  // a part of another message
        CHECK(r.add(1, lines[0], m) == Reassembler::Result::Partial);
        CHECK(r.add(1, encode("my-mod", 6, bytes(300))[1], m) == Reassembler::Result::Bad);
    }
    {
        Reassembler r;  // a later part without the first
        CHECK(r.add(1, lines[1], m) == Reassembler::Result::Bad);
    }
    for (const char* bad : {
             "[Tome] Msg 2 1 my-mod 1/1: aGk=",          // another version
             "[Tome] Msg 1 x my-mod 1/1: aGk=",          // not a number
             "[Tome] Msg 1 1 My-Mod 1/1: aGk=",          // not a mod id
             "[Tome] Msg 1 1 my-mod 2/1: aGk=",          // part past the count
             "[Tome] Msg 1 1 my-mod 0/1: aGk=",          // parts count from 1
             "[Tome] Msg 1 1 my-mod 1/99: aGk=",         // more parts than a message has
             "[Tome] Msg 1 1 my-mod 1/2: aGk=",          // a short part that isn't the last
             "[Tome] Msg 1 1 my-mod 1/1:aGk=",           // no space
             "[Tome] Msg 1 1 my-mod 1/1: a!k=",          // not base64
             "[Tome] Msg 1 12345678901 my-mod 1/1: aGk=",  // id too long
         }) {
        Reassembler r;
        CHECK(r.add(1, bad, m) == Reassembler::Result::Bad);
    }
    {
        Reassembler r;  // a line past the limit
        std::string longline = encode("my-mod", 1, "hi")[0] + std::string(kMaxLine, 'A');
        CHECK(r.add(1, longline, m) == Reassembler::Result::Bad);
    }
    {
        Reassembler r;  // a sender who left: its message in progress goes
        CHECK(r.add(1, lines[0], m) == Reassembler::Result::Partial);
        r.forget(1);
        CHECK_EQ(r.pending(), size_t(0));
    }
}
