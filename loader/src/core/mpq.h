#pragma once

// Minimal MPQ (format version 1) writer for Tome's overlay archive: files are
// stored uncompressed and unencrypted, which every Storm version reads.

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace tome::mpq {

enum class HashType : uint32_t { TableOffset = 0, NameA = 1, NameB = 2, FileKey = 3 };

// Blizzard's string hash (case-insensitive, '/' treated as '\').
uint32_t hash_string(std::string_view text, HashType type);

void encrypt(uint32_t* words, size_t count, uint32_t key);
void decrypt(uint32_t* words, size_t count, uint32_t key);

struct File {
    std::string name;  // archive path with backslashes, e.g. "UI\\FrameDef\\Glue\\MainMenu.fdf"
    std::string data;
};

// Builds a complete archive. A "(listfile)" is added automatically.
std::string build_archive(const std::vector<File>& files);

// Builds an archive piece by piece, so large files never have to be in memory
// at once. The caller writes the bytes: header_size() placeholder bytes first,
// then each file's data right after add() accepts it, then finish(), and
// finally header() over the placeholder at offset 0.
class ArchiveWriter {
public:
    static constexpr uint32_t header_size() { return 32; }

    // Registers the next `size` bytes as file `name`. False if the name is
    // already in the archive (names are case-insensitive) or the archive
    // would pass 4 GiB, the format's limit; the caller then skips the data.
    bool add(std::string_view name, uint32_t size);
    bool contains(std::string_view name) const;
    size_t file_count() const { return names_.size(); }

    // The (listfile), hash table and block table: the bytes after the last file.
    std::string finish();
    // The header, valid after finish().
    std::string header() const;

private:
    // A name's NameA hash in the high word, NameB in the low one.
    static uint64_t key(std::string_view name);

    std::vector<std::string> names_;
    std::vector<uint32_t> blocks_;        // 4 words per file
    std::unordered_set<uint64_t> keys_;  // every name's key, for contains()
    uint64_t size_ = header_size();
    uint32_t hash_pos_ = 0, block_pos_ = 0, hash_size_ = 0;
};

} // namespace tome::mpq
