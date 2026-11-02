// SPDX-License-Identifier: GPL-3.0-or-later
#include "mpq.h"

#include <algorithm>
#include <cstring>

namespace tome::mpq {
namespace {

constexpr uint32_t kMagic = 0x1A51504D;  // "MPQ\x1A"
constexpr uint32_t kFileExists = 0x80000000;
constexpr uint32_t kEmptyEntry = 0xFFFFFFFF;

struct CryptTable {
    uint32_t values[0x500];

    CryptTable() {
        uint32_t seed = 0x00100001;
        for (uint32_t i = 0; i < 0x100; ++i) {
            uint32_t index = i;
            for (int j = 0; j < 5; ++j) {
                seed = (seed * 125 + 3) % 0x2AAAAB;
                uint32_t high = (seed & 0xFFFF) << 16;
                seed = (seed * 125 + 3) % 0x2AAAAB;
                values[index] = high | (seed & 0xFFFF);
                index += 0x100;
            }
        }
    }
};

const CryptTable& crypt() {
    static const CryptTable table;
    return table;
}

void put32(std::string& out, size_t at, uint32_t value) {
    std::memcpy(&out[at], &value, 4);
}

void append_words(std::string& out, const std::vector<uint32_t>& words) {
    size_t at = out.size();
    out.resize(at + words.size() * 4);
    std::memcpy(&out[at], words.data(), words.size() * 4);
}

} // namespace

uint32_t hash_string(std::string_view text, HashType type) {
    const uint32_t* table = crypt().values;
    uint32_t seed1 = 0x7FED7FED;
    uint32_t seed2 = 0xEEEEEEEE;
    uint32_t offset = static_cast<uint32_t>(type) * 0x100;
    for (char c : text) {
        uint32_t ch = static_cast<unsigned char>(c);
        if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
        if (ch == '/') ch = '\\';
        seed1 = table[offset + ch] ^ (seed1 + seed2);
        seed2 = ch + seed1 + seed2 + (seed2 << 5) + 3;
    }
    return seed1;
}

void encrypt(uint32_t* words, size_t count, uint32_t key) {
    const uint32_t* table = crypt().values;
    uint32_t seed = 0xEEEEEEEE;
    for (size_t i = 0; i < count; ++i) {
        seed += table[0x400 + (key & 0xFF)];
        uint32_t plain = words[i];
        words[i] = plain ^ (key + seed);
        key = ((~key << 0x15) + 0x11111111) | (key >> 0x0B);
        seed = plain + seed + (seed << 5) + 3;
    }
}

void decrypt(uint32_t* words, size_t count, uint32_t key) {
    const uint32_t* table = crypt().values;
    uint32_t seed = 0xEEEEEEEE;
    for (size_t i = 0; i < count; ++i) {
        seed += table[0x400 + (key & 0xFF)];
        uint32_t plain = words[i] ^ (key + seed);
        words[i] = plain;
        key = ((~key << 0x15) + 0x11111111) | (key >> 0x0B);
        seed = plain + seed + (seed << 5) + 3;
    }
}

uint64_t ArchiveWriter::key(std::string_view name) {
    return (uint64_t{hash_string(name, HashType::NameA)} << 32) | hash_string(name, HashType::NameB);
}

bool ArchiveWriter::add(std::string_view name, uint32_t size) {
    const uint64_t k = key(name);
    if (keys_.count(k)) return false;
    // Leave room for the tables and the listfile that finish() adds.
    if (size_ + size > 0xF0000000ull) return false;
    keys_.insert(k);
    names_.emplace_back(name);
    blocks_.insert(blocks_.end(), {static_cast<uint32_t>(size_), size, size, kFileExists});
    size_ += size;
    return true;
}

bool ArchiveWriter::contains(std::string_view name) const {
    return keys_.count(key(name)) > 0;
}

std::string ArchiveWriter::finish() {
    std::string out;
    std::string listfile;
    for (const std::string& name : names_) listfile += name + "\r\n";
    if (add("(listfile)", static_cast<uint32_t>(listfile.size()))) out += listfile;

    // Hash table: a power of two, at most half full.
    hash_size_ = 16;
    while (hash_size_ < names_.size() * 2) hash_size_ *= 2;
    std::vector<uint32_t> hashes(hash_size_ * 4, kEmptyEntry);
    for (uint32_t block = 0; block < names_.size(); ++block) {
        const std::string& name = names_[block];
        uint32_t slot = hash_string(name, HashType::TableOffset) & (hash_size_ - 1);
        while (hashes[slot * 4 + 3] != kEmptyEntry) slot = (slot + 1) & (hash_size_ - 1);
        hashes[slot * 4 + 0] = hash_string(name, HashType::NameA);
        hashes[slot * 4 + 1] = hash_string(name, HashType::NameB);
        hashes[slot * 4 + 2] = 0;  // locale 0 (neutral), platform 0
        hashes[slot * 4 + 3] = block;
    }
    std::vector<uint32_t> blocks = blocks_;
    encrypt(hashes.data(), hashes.size(), hash_string("(hash table)", HashType::FileKey));
    encrypt(blocks.data(), blocks.size(), hash_string("(block table)", HashType::FileKey));

    hash_pos_ = static_cast<uint32_t>(size_);
    append_words(out, hashes);
    block_pos_ = static_cast<uint32_t>(size_ + hashes.size() * 4);
    append_words(out, blocks);
    size_ += hashes.size() * 4 + blocks.size() * 4;
    return out;
}

std::string ArchiveWriter::header() const {
    std::string out(header_size(), '\0');
    put32(out, 0, kMagic);
    put32(out, 4, header_size());
    put32(out, 8, static_cast<uint32_t>(size_));
    uint16_t version = 0, sector_shift = 3;  // 512 << 3 = 4 KiB sectors
    std::memcpy(&out[12], &version, 2);
    std::memcpy(&out[14], &sector_shift, 2);
    put32(out, 16, hash_pos_);
    put32(out, 20, block_pos_);
    put32(out, 24, hash_size_);
    put32(out, 28, static_cast<uint32_t>(names_.size()));
    return out;
}

std::string build_archive(const std::vector<File>& files) {
    ArchiveWriter writer;
    std::string out(ArchiveWriter::header_size(), '\0');
    for (const File& f : files) {
        if (writer.add(f.name, static_cast<uint32_t>(f.data.size()))) out += f.data;
    }
    out += writer.finish();
    std::string header = writer.header();
    out.replace(0, header.size(), header);
    return out;
}

} // namespace tome::mpq
