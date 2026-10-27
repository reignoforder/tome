#include "test.h"

#include <tome/utf8.h>

#include <string>

namespace {

struct Decoded {
    uint32_t cp;
    size_t length;
};

Decoded decode(const std::string& s) {
    Decoded d{0, 0};
    d.length = tome_utf8_decode(s.data(), s.size(), &d.cp);
    return d;
}

std::string sanitize(const char* s, size_t size = 256) {
    char buf[256];
    size_t need = tome_utf8_sanitize(buf, size, s);
    (void)need;
    return buf;
}

} // namespace

TEST(utf8_decode) {
    CHECK_EQ(decode("A").cp, 0x41u);
    CHECK_EQ(decode("A").length, size_t(1));
    CHECK_EQ(decode("\xc3\xa9").cp, 0xE9u);          // é
    CHECK_EQ(decode("\xc3\xa9").length, size_t(2));
    CHECK_EQ(decode("\xe2\x82\xac").cp, 0x20ACu);     // €
    CHECK_EQ(decode("\xf0\x9f\x98\x80").cp, 0x1F600u);
    CHECK_EQ(decode("\xf0\x9f\x98\x80").length, size_t(4));
    CHECK_EQ(decode("\xf4\x8f\xbf\xbf").cp, 0x10FFFFu);

    // Invalid: each maximal invalid part reads as one U+FFFD.
    CHECK_EQ(decode("\x80").cp, 0xFFFDu);              // lone continuation byte
    CHECK_EQ(decode("\x80").length, size_t(1));
    CHECK_EQ(decode("\xc0\x80").length, size_t(1));    // overlong lead
    CHECK_EQ(decode("\xe0\x80\x80").length, size_t(1)); // overlong 3-byte
    CHECK_EQ(decode("\xed\xa0\x80").cp, 0xFFFDu);      // surrogate
    CHECK_EQ(decode("\xed\xa0\x80").length, size_t(1));
    CHECK_EQ(decode("\xf4\x90\x80\x80").length, size_t(1)); // above U+10FFFF
    CHECK_EQ(decode("\xf5").length, size_t(1));
    CHECK_EQ(decode("\xe2\x82").cp, 0xFFFDu);          // truncated
    CHECK_EQ(decode("\xe2\x82").length, size_t(2));
    CHECK_EQ(decode("\xf0\x9f\x98" "A").length, size_t(3));
    CHECK_EQ(decode("\xe2" "A").length, size_t(1));
}

TEST(utf8_encode) {
    char buf[4];
    CHECK_EQ(tome_utf8_encode(0x41, buf), size_t(1));
    CHECK_EQ(std::string(buf, 1), std::string("A"));
    CHECK_EQ(tome_utf8_encode(0xE9, buf), size_t(2));
    CHECK_EQ(std::string(buf, 2), std::string("\xc3\xa9"));
    CHECK_EQ(tome_utf8_encode(0x20AC, buf), size_t(3));
    CHECK_EQ(std::string(buf, 3), std::string("\xe2\x82\xac"));
    CHECK_EQ(tome_utf8_encode(0x1F600, buf), size_t(4));
    CHECK_EQ(std::string(buf, 4), std::string("\xf0\x9f\x98\x80"));
    CHECK_EQ(tome_utf8_encode(0xD800, buf), size_t(3));
    CHECK_EQ(std::string(buf, 3), std::string("\xef\xbf\xbd"));
    CHECK_EQ(tome_utf8_encode(0x110000, buf), size_t(3));
}

TEST(utf8_valid_length_offset) {
    CHECK(tome_utf8_valid(""));
    CHECK(tome_utf8_valid("plain ascii"));
    CHECK(tome_utf8_valid("Z\xce\xb5\xcf\x8d\xcf\x82 \xe2\x82\xac \xf0\x9f\x98\x80"));
    CHECK(tome_utf8_valid("\xef\xbf\xbd"));  // U+FFFD itself is fine
    CHECK(!tome_utf8_valid("\xff"));
    CHECK(!tome_utf8_valid("ab\xe2\x82"));
    CHECK(!tome_utf8_valid("\xf0\x9f\x98"));
    CHECK(!tome_utf8_valid("\xed\xb0\x80"));

    const char* zeus = "\xce\x96\xce\xb5\xcf\x8d\xcf\x82";  // Ζεύς: 4 code points, 8 bytes
    CHECK_EQ(tome_utf8_length(zeus), size_t(4));
    CHECK_EQ(tome_utf8_length(""), size_t(0));
    CHECK_EQ(tome_utf8_length("a\xff" "b"), size_t(3));
    CHECK_EQ(tome_utf8_offset(zeus, 0), size_t(0));
    CHECK_EQ(tome_utf8_offset(zeus, 1), size_t(2));
    CHECK_EQ(tome_utf8_offset(zeus, 3), size_t(6));
    CHECK_EQ(tome_utf8_offset(zeus, 4), size_t(8));
    CHECK_EQ(tome_utf8_offset(zeus, 100), size_t(8));
}

TEST(utf8_sanitize) {
    CHECK_EQ(sanitize("Hello, \xc3\xa9!"), std::string("Hello, \xc3\xa9!"));
    CHECK_EQ(sanitize("a\xff" "b"), std::string("a\xef\xbf\xbd" "b"));
    CHECK_EQ(sanitize("\xe2\x82" "x"), std::string("\xef\xbf\xbd" "x"));
    CHECK_EQ(sanitize("\xed\xa0\x80"), std::string("\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd"));

    // Cut short at a code point boundary, still terminated; returns the full length.
    char buf[4];
    CHECK_EQ(tome_utf8_sanitize(buf, sizeof(buf), "ab\xc3\xa9"), size_t(4));
    CHECK_EQ(std::string(buf), std::string("ab"));
    CHECK_EQ(tome_utf8_sanitize(buf, sizeof(buf), "abc"), size_t(3));
    CHECK_EQ(std::string(buf), std::string("abc"));
    CHECK_EQ(tome_utf8_sanitize(buf, sizeof(buf), "abcd"), size_t(4));
    CHECK_EQ(std::string(buf), std::string("abc"));
    CHECK_EQ(tome_utf8_sanitize(nullptr, 0, "\xff"), size_t(3));
    CHECK_EQ(tome_utf8_sanitize(buf, 1, "abc"), size_t(3));
    CHECK_EQ(buf[0], '\0');
}

TEST(utf8_utf16) {
    wchar_t wide[16];
    CHECK_EQ(tome_utf8_to_utf16(wide, 16, "a\xc3\xa9\xf0\x9f\x98\x80"), size_t(4));
    CHECK_EQ(int(wide[0]), int('a'));
    CHECK_EQ(int(wide[1]), 0xE9);
    CHECK_EQ(int(wide[2]), 0xD83D);
    CHECK_EQ(int(wide[3]), 0xDE00);
    CHECK_EQ(int(wide[4]), 0);

    // A surrogate pair is never split.
    CHECK_EQ(tome_utf8_to_utf16(wide, 3, "a\xf0\x9f\x98\x80"), size_t(3));
    CHECK_EQ(int(wide[0]), int('a'));
    CHECK_EQ(int(wide[1]), 0);
    CHECK_EQ(tome_utf8_to_utf16(nullptr, 0, "abc"), size_t(3));

    char narrow[16];
    const wchar_t back[] = {L'a', 0xE9, 0xD83D, 0xDE00, 0};
    CHECK_EQ(tome_utf16_to_utf8(narrow, sizeof(narrow), back), size_t(7));
    CHECK_EQ(std::string(narrow), std::string("a\xc3\xa9\xf0\x9f\x98\x80"));
    const wchar_t lone[] = {0xD83D, L'x', 0xDE00, 0};
    CHECK_EQ(tome_utf16_to_utf8(narrow, sizeof(narrow), lone), size_t(7));
    CHECK_EQ(std::string(narrow), std::string("\xef\xbf\xbdx\xef\xbf\xbd"));
    CHECK_EQ(tome_utf16_to_utf8(narrow, 3, back), size_t(7));
    CHECK_EQ(std::string(narrow), std::string("a"));
}
