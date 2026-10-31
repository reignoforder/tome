#include "core/hash_cache.h"
#include "test.h"

#include <string>

using namespace tome::hash_cache;

namespace {

const std::string kShaA(64, 'a');
const std::string kShaB(64, 'b');

Stamp stamp(uint64_t size) {
    Stamp s;
    s.size = size;
    s.written = 0x01dc0000aabbccddull;
    s.created = 0x01db0000aabbccddull;
    s.file_id = 0xffffffffffffffffull;
    s.volume = 0xdeadbeef;
    return s;
}

} // namespace

TEST(hash_cache_finds_a_file_only_with_its_stamp) {
    Cache c;
    CHECK(c.find("C:\\mods\\a\\x.blp", stamp(1)) == nullptr);
    c.put("C:\\mods\\a\\x.blp", stamp(1), kShaA);
    CHECK(c.dirty());
    const std::string* sha = c.find("C:\\mods\\a\\x.blp", stamp(1));
    CHECK(sha && *sha == kShaA);
    CHECK(c.find("C:\\mods\\a\\x.blp", stamp(2)) == nullptr);
    Stamp moved = stamp(1);
    moved.file_id = 7;  // another file at the same path
    CHECK(c.find("C:\\mods\\a\\x.blp", moved) == nullptr);
    Stamp other_volume = stamp(1);
    other_volume.volume = 1;
    CHECK(c.find("C:\\mods\\a\\x.blp", other_volume) == nullptr);
}

TEST(hash_cache_round_trips_and_drops_unused_entries) {
    Cache c;
    c.put("C:\\mods\\a\\x.blp", stamp(1), kShaA);
    c.put("C:\\mods\\a\\y.mdx", stamp(2), kShaB);
    Cache read;
    CHECK(read.parse(c.serialize()));
    CHECK_EQ(read.size(), size_t(2));
    CHECK(read.dirty());  // nothing used yet: both would be dropped
    const std::string* sha = read.find("C:\\mods\\a\\y.mdx", stamp(2));
    CHECK(sha && *sha == kShaB);
    CHECK(read.dirty());  // x.blp wasn't used
    read.mark_saved();
    CHECK(!read.dirty());
    CHECK_EQ(read.size(), size_t(1));

    Cache again;
    CHECK(again.parse(read.serialize()));
    CHECK(again.find("C:\\mods\\a\\x.blp", stamp(1)) == nullptr);
    CHECK(again.find("C:\\mods\\a\\y.mdx", stamp(2)) != nullptr);
    CHECK(!again.dirty());  // the same as the file: no write
}

TEST(hash_cache_refuses_what_it_did_not_write) {
    Cache c;
    CHECK(!c.parse(""));
    CHECK(!c.parse("[]"));
    CHECK(!c.parse(R"({"version": 2, "files": {}})"));
    CHECK(!c.parse(R"({"version": 1, "files": []})"));
    CHECK(c.parse(R"({"version": 1, "files": {}})"));
    CHECK_EQ(c.size(), size_t(0));
}

TEST(hash_cache_skips_invalid_entries) {
    const std::string sha = kShaA;
    const std::string text = R"({"version": 1, "files": {
        "good": {"stamp": "1-2-3-4-5", "sha256": ")" + sha + R"("},
        "": {"stamp": "1-2-3-4-5", "sha256": ")" + sha + R"("},
        "short sha": {"stamp": "1-2-3-4-5", "sha256": "abc"},
        "upper sha": {"stamp": "1-2-3-4-5", "sha256": ")" + std::string(64, 'A') + R"("},
        "few fields": {"stamp": "1-2-3-4", "sha256": ")" + sha + R"("},
        "many fields": {"stamp": "1-2-3-4-5-6", "sha256": ")" + sha + R"("},
        "empty field": {"stamp": "1--3-4-5", "sha256": ")" + sha + R"("},
        "not hex": {"stamp": "1-2-3-4-x", "sha256": ")" + sha + R"("},
        "too long": {"stamp": "11112222333344445-2-3-4-5", "sha256": ")" + sha + R"("},
        "volume too big": {"stamp": "1-2-3-4-100000000", "sha256": ")" + sha + R"("},
        "number stamp": {"stamp": 5, "sha256": ")" + sha + R"("},
        "not an object": 3
    }})";
    Cache c;
    CHECK(c.parse(text));
    CHECK_EQ(c.size(), size_t(1));
    Stamp s;
    s.size = 1;
    s.written = 2;
    s.created = 3;
    s.file_id = 4;
    s.volume = 5;
    CHECK(c.find("good", s) != nullptr);
}

TEST(hash_cache_ignores_a_bad_sha) {
    Cache c;
    c.put("x", stamp(1), "not a hash");
    CHECK(!c.dirty());
    CHECK(c.find("x", stamp(1)) == nullptr);
}

TEST(hash_cache_folder_hashes_keep_their_files) {
    Cache c;
    c.put("C:\\mods\\a\\x.blp", stamp(1), kShaA);
    c.put("C:\\mods\\ab\\y.blp", stamp(2), kShaA);  // another folder, though the name starts the same
    c.put_folder("C:\\mods\\a", 0xfeedface12345678ull, kShaB);
    Cache read;
    CHECK(read.parse(c.serialize()));
    CHECK_EQ(read.folders(), size_t(1));
    CHECK(read.find_folder("C:\\mods\\a", 1) == nullptr);  // another print: a file changed
    const std::string* sha = read.find_folder("C:\\mods\\a", 0xfeedface12345678ull);
    CHECK(sha && *sha == kShaB);
    // The folder's files count as used, the other folder's don't.
    read.mark_saved();
    CHECK_EQ(read.size(), size_t(1));
    CHECK(read.find("C:\\mods\\a\\x.blp", stamp(1)) != nullptr);
    CHECK_EQ(read.folders(), size_t(1));

    Cache unused;
    CHECK(unused.parse(c.serialize()));
    CHECK(unused.dirty());  // the folder wasn't looked up: dropped
    unused.mark_saved();
    CHECK_EQ(unused.folders(), size_t(0));
}

TEST(hash_cache_skips_invalid_folders) {
    const std::string sha = kShaA;
    const std::string text = R"({"version": 1, "files": {}, "folders": {
        "good": {"print": "ff", "sha256": ")" + sha + R"("},
        "": {"print": "ff", "sha256": ")" + sha + R"("},
        "empty print": {"print": "", "sha256": ")" + sha + R"("},
        "not hex": {"print": "fg", "sha256": ")" + sha + R"("},
        "too long": {"print": "11112222333344445", "sha256": ")" + sha + R"("},
        "dash": {"print": "1-2", "sha256": ")" + sha + R"("},
        "bad sha": {"print": "ff", "sha256": "abc"},
        "number print": {"print": 255, "sha256": ")" + sha + R"("}
    }})";
    Cache c;
    CHECK(c.parse(text));
    CHECK_EQ(c.folders(), size_t(1));
    CHECK(c.find_folder("good", 0xff) != nullptr);
    c.put_folder("x", 1, "not a hash");
    CHECK(c.find_folder("x", 1) == nullptr);
}
