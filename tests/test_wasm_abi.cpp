#include "core/wasm_abi.h"
#include "test.h"

#include <cstring>
#include <initializer_list>
#include <vector>

using namespace tome::wasm;

namespace {

struct Fixture {
    uint8_t bytes[64] = {};
    Memory memory{bytes, sizeof(bytes)};
    void put(uint32_t at, const char* text) { std::memcpy(bytes + at, text, std::strlen(text)); }
};

} // namespace

TEST(wasm_bounds) {
    Fixture f;
    CHECK(in_bounds(f.memory, 0, 64));
    CHECK(in_bounds(f.memory, 64, 0));
    CHECK(in_bounds(f.memory, 60, 4));
    CHECK(!in_bounds(f.memory, 61, 4));
    CHECK(!in_bounds(f.memory, 65, 0));
    CHECK(!in_bounds(f.memory, 0xFFFFFFFFu, 2));  // would wrap in 32 bits
    CHECK(!in_bounds(f.memory, 1, 0xFFFFFFFFu));
    Memory none;
    CHECK(in_bounds(none, 0, 0));
    CHECK(!in_bounds(none, 0, 1));
    CHECK(!in_bounds(none, 4, 0));
}

TEST(wasm_strings) {
    Fixture f;
    f.put(0, "hello");
    std::string out;
    CHECK(read_string(f.memory, 0, 5, 100, out) == StringError::None);
    CHECK_EQ(out, std::string("hello"));
    CHECK(read_string(f.memory, 0, 0, 100, out) == StringError::None && out.empty());
    CHECK(read_string(f.memory, 0, 5, 4, out) == StringError::TooLong);
    CHECK(read_string(f.memory, 62, 5, 100, out) == StringError::OutOfBounds);
    CHECK(read_string(f.memory, 0, 6, 100, out) == StringError::Nul);  // the zero after "hello"
    CHECK(out.empty());
    f.put(10, "\xC3\xA9t\xC3\xA9 \xEF\xBF\xBD");  // "été" and a real U+FFFD
    CHECK(read_string(f.memory, 10, 9, 100, out) == StringError::None);
    f.put(20, "a\xC3");  // cut short
    CHECK(read_string(f.memory, 20, 2, 100, out) == StringError::InvalidUtf8);
    f.put(30, "\xED\xA0\x80");  // a UTF-16 surrogate
    CHECK(read_string(f.memory, 30, 3, 100, out) == StringError::InvalidUtf8);
    f.put(40, "\xC0\xAF");  // overlong '/'
    CHECK(read_string(f.memory, 40, 2, 100, out) == StringError::InvalidUtf8);
    CHECK(std::string(describe(StringError::OutOfBounds)) == "outside the module's memory");
}

TEST(wasm_bytes_and_out_params) {
    Fixture f;
    f.put(0, "abc");
    std::vector<uint8_t> bytes;
    CHECK(read_bytes(f.memory, 0, 3, 3, bytes) && bytes.size() == 3 && bytes[2] == 'c');
    CHECK(!read_bytes(f.memory, 0, 4, 3, bytes));
    CHECK(!read_bytes(f.memory, 63, 2, 10, bytes));

    CHECK_EQ(write_out(f.memory, 10, 0, "hello"), 5u);  // the size only
    CHECK_EQ(f.bytes[10], 0);
    CHECK_EQ(write_out(f.memory, 10, 3, "hello"), 5u);  // as much as fits
    CHECK(std::memcmp(f.bytes + 10, "hel", 3) == 0 && f.bytes[13] == 0);
    CHECK_EQ(write_out(f.memory, 20, 10, "hello"), 5u);
    CHECK(std::memcmp(f.bytes + 20, "hello", 5) == 0);
    CHECK_EQ(write_out(f.memory, 60, 10, "hello"), UINT32_MAX);  // the buffer runs past the end
    CHECK_EQ(f.bytes[60], 0);

    uint32_t v = 0;
    CHECK(store_u32(f.memory, 30, 0x11223344u));
    CHECK(f.bytes[30] == 0x44 && f.bytes[33] == 0x11);  // little-endian
    CHECK(load_u32(f.memory, 30, v) && v == 0x11223344u);
    CHECK(store_u32(f.memory, 60, 1));
    CHECK(!store_u32(f.memory, 61, 1));
    CHECK(!load_u32(f.memory, 61, v));
}

TEST(wasm_handles) {
    HandleTable t;
    int a = 0, b = 0;
    const uint32_t ha = t.add(HandleKind::Frame, &a);
    CHECK(ha != 0);
    CHECK_EQ(t.add(HandleKind::Frame, &a), ha);  // one handle per object
    const uint32_t hu = t.add(HandleKind::Unit, &a);
    CHECK(hu != ha);                             // per kind
    CHECK(t.get(ha, HandleKind::Frame) == &a);
    CHECK(t.get(ha, HandleKind::Unit) == nullptr);  // the wrong kind
    CHECK(t.get(0, HandleKind::Frame) == nullptr);
    CHECK(t.get(0x7FFFF, HandleKind::Frame) == nullptr);
    CHECK_EQ(t.add(HandleKind::Frame, nullptr), 0u);
    CHECK_EQ(t.live(), 2u);

    // A stale handle doesn't name the slot's next object.
    t.remove(ha);
    CHECK(t.get(ha, HandleKind::Frame) == nullptr);
    const uint32_t hb = t.add(HandleKind::Frame, &b);
    CHECK((hb & HandleTable::kMaxHandles) == (ha & HandleTable::kMaxHandles));  // the slot is reused
    CHECK(hb != ha);
    CHECK(t.get(ha, HandleKind::Frame) == nullptr);
    CHECK(t.get(hb, HandleKind::Frame) == &b);
    t.remove(ha);  // stale: nothing happens
    CHECK(t.get(hb, HandleKind::Frame) == &b);

    t.remove_all(HandleKind::Frame);
    CHECK(t.get(hb, HandleKind::Frame) == nullptr);
    CHECK(t.get(hu, HandleKind::Unit) == &a);
    CHECK_EQ(t.live(), 1u);
}

TEST(wasm_handles_never_repeat_in_a_slot) {
    // A slot goes through every generation once, then retires: a handle
    // never comes back as the handle of something else.
    HandleTable t;
    int x = 0;
    const uint32_t first = t.add(HandleKind::Frame, &x);
    std::vector<uint32_t> seen{first};
    t.remove(first);
    for (int i = 0; i < 5000; ++i) {
        const uint32_t h = t.add(HandleKind::Frame, &x);
        CHECK(h != 0);
        if ((h & HandleTable::kMaxHandles) != (first & HandleTable::kMaxHandles)) break;  // retired
        bool repeated = false;
        for (uint32_t old : seen) repeated = repeated || h == old;
        CHECK(!repeated);
        if (h == first) break;
        seen.push_back(h);
        t.remove(h);
    }
    CHECK(seen.size() < 4096);
}

TEST(wasm_file_names) {
    CHECK(valid_file_name("state.json"));
    CHECK(valid_file_name("save_1-a.bin"));
    CHECK(!valid_file_name(""));
    CHECK(!valid_file_name(".hidden"));
    CHECK(!valid_file_name(".."));
    CHECK(!valid_file_name("a/b"));
    CHECK(!valid_file_name("a\\b"));
    CHECK(!valid_file_name("c:x"));
    CHECK(!valid_file_name("name."));
    CHECK(!valid_file_name("con"));
    CHECK(!valid_file_name("NUL.txt"));
    CHECK(!valid_file_name("com1.json"));
    CHECK(!valid_file_name("Lpt9"));
    CHECK(valid_file_name("com10"));
    CHECK(valid_file_name("console.txt"));
    CHECK(!valid_file_name(std::string(65, 'a')));
    CHECK(!valid_file_name("sp ace"));
}

TEST(wasm_start_section) {
    const uint8_t header[] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    auto module = [&](std::initializer_list<uint8_t> sections) {
        std::vector<uint8_t> m(header, header + sizeof(header));
        m.insert(m.end(), sections);
        return m;
    };
    // A type section (one () -> ()), then a start section naming function 0.
    std::vector<uint8_t> with = module({0x01, 0x04, 0x01, 0x60, 0x00, 0x00, 0x08, 0x01, 0x00});
    CHECK(has_start_section(with.data(), with.size()));
    // A custom section first, its size as a padded LEB128.
    std::vector<uint8_t> custom = module({0x00, 0x82, 0x00, 0x01, 'x', 0x08, 0x01, 0x00});
    CHECK(has_start_section(custom.data(), custom.size()));
    std::vector<uint8_t> without = module({0x01, 0x04, 0x01, 0x60, 0x00, 0x00});
    CHECK(!has_start_section(without.data(), without.size()));
    CHECK(!has_start_section(header, sizeof(header)));
    // Malformed: a section running past the end, a size that never ends,
    // not a module at all. The 8 inside a section's bytes isn't a section.
    std::vector<uint8_t> overrun = module({0x01, 0x10, 0x08});
    CHECK(!has_start_section(overrun.data(), overrun.size()));
    std::vector<uint8_t> endless = module({0x08, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80});
    CHECK(!has_start_section(endless.data(), endless.size()));
    std::vector<uint8_t> inside = module({0x00, 0x02, 0x08, 0x01});
    CHECK(!has_start_section(inside.data(), inside.size()));
    const uint8_t junk[] = {0x08, 0x01, 0x00};
    CHECK(!has_start_section(junk, sizeof(junk)));
    CHECK(!has_start_section(nullptr, 0));
}
