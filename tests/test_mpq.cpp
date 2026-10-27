#include "core/mpq.h"
#include "test.h"

#include <cstring>

using namespace tome::mpq;

namespace {

uint32_t get32(const std::string& s, size_t at) {
    uint32_t v;
    std::memcpy(&v, &s[at], 4);
    return v;
}

// Reads a file back the way Storm does: hash lookup, then block entry.
bool read_back(const std::string& archive, const std::string& name, std::string& out) {
    uint32_t hash_pos = get32(archive, 16), block_pos = get32(archive, 20);
    uint32_t hash_size = get32(archive, 24), block_count = get32(archive, 28);
    std::vector<uint32_t> hashes(hash_size * 4), blocks(block_count * 4);
    std::memcpy(hashes.data(), &archive[hash_pos], hashes.size() * 4);
    std::memcpy(blocks.data(), &archive[block_pos], blocks.size() * 4);
    decrypt(hashes.data(), hashes.size(), hash_string("(hash table)", HashType::FileKey));
    decrypt(blocks.data(), blocks.size(), hash_string("(block table)", HashType::FileKey));

    uint32_t a = hash_string(name, HashType::NameA), b = hash_string(name, HashType::NameB);
    uint32_t slot = hash_string(name, HashType::TableOffset) & (hash_size - 1);
    for (uint32_t n = 0; n < hash_size; ++n, slot = (slot + 1) & (hash_size - 1)) {
        uint32_t block = hashes[slot * 4 + 3];
        if (block == 0xFFFFFFFF) return false;
        if (hashes[slot * 4] == a && hashes[slot * 4 + 1] == b) {
            uint32_t offset = blocks[block * 4], size = blocks[block * 4 + 2];
            out = archive.substr(offset, size);
            return true;
        }
    }
    return false;
}

} // namespace

TEST(mpq_hash_known_answers) {
    // Well-known table keys (MPQ_KEY_HASH_TABLE / MPQ_KEY_BLOCK_TABLE in StormLib).
    CHECK_EQ(hash_string("(hash table)", HashType::FileKey), 0xC3AF3770u);
    CHECK_EQ(hash_string("(block table)", HashType::FileKey), 0xEC83B3A3u);
    // Case-insensitive, and '/' is the same as '\'.
    CHECK_EQ(hash_string("ui\\framedef\\x.fdf", HashType::NameA), hash_string("UI/FrameDef/X.FDF", HashType::NameA));
}

TEST(mpq_encrypt_round_trip) {
    uint32_t words[5] = {1, 2, 3, 0xDEADBEEF, 0};
    uint32_t copy[5];
    std::memcpy(copy, words, sizeof(words));
    encrypt(words, 5, 0x12345678);
    CHECK(std::memcmp(words, copy, sizeof(words)) != 0);
    decrypt(words, 5, 0x12345678);
    CHECK(std::memcmp(words, copy, sizeof(words)) == 0);
}

TEST(mpq_build_and_read_back) {
    std::vector<File> files;
    files.push_back({"UI\\FrameDef\\Glue\\MainMenu.fdf", "Frame \"FRAME\" \"Test\" {}\r\n"});
    files.push_back({"UI\\FrameDef\\FrameDef.toc", "UI\\FrameDef\\Glue\\MainMenu.fdf\r\n"});
    files.push_back({"Empty.txt", ""});
    for (int i = 0; i < 40; ++i) files.push_back({"many\\file" + std::to_string(i), std::string(i, 'x')});

    std::string archive = build_archive(files);
    CHECK_EQ(get32(archive, 0), 0x1A51504Du);
    CHECK_EQ(get32(archive, 8), static_cast<uint32_t>(archive.size()));

    std::string out;
    CHECK(read_back(archive, "UI\\FrameDef\\Glue\\MainMenu.fdf", out));
    CHECK_EQ(out, files[0].data);
    CHECK(read_back(archive, "ui/framedef/framedef.toc", out));
    CHECK_EQ(out, files[1].data);
    CHECK(read_back(archive, "Empty.txt", out));
    CHECK(out.empty());
    CHECK(read_back(archive, "many\\file39", out));
    CHECK_EQ(out.size(), size_t(39));
    CHECK(read_back(archive, "(listfile)", out));
    CHECK(out.find("UI\\FrameDef\\FrameDef.toc\r\n") != std::string::npos);
    CHECK(!read_back(archive, "missing.txt", out));
}

TEST(mpq_writer_streams_and_skips_duplicates) {
    ArchiveWriter writer;
    std::string out(ArchiveWriter::header_size(), '\0');
    CHECK(writer.add("Textures\\a.blp", 3));
    out += "abc";
    CHECK(writer.contains("textures/A.BLP"));
    CHECK(!writer.add("TEXTURES\\A.blp", 5));  // already there, any case
    CHECK(writer.add("b.txt", 0));
    CHECK(!writer.contains("c.txt"));
    CHECK_EQ(writer.file_count(), size_t{2});
    out += writer.finish();
    std::string header = writer.header();
    out.replace(0, header.size(), header);

    CHECK_EQ(get32(out, 8), static_cast<uint32_t>(out.size()));
    CHECK_EQ(out, build_archive({{"Textures\\a.blp", "abc"}, {"b.txt", ""}}));
    std::string data;
    CHECK(read_back(out, "Textures\\a.blp", data));
    CHECK_EQ(data, std::string("abc"));
    CHECK(read_back(out, "(listfile)", data));
    CHECK_EQ(data, std::string("Textures\\a.blp\r\nb.txt\r\n"));
}
