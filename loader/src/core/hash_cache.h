// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// SHA-256 of files, remembered across starts (tome/cache/file-hashes.json)
// so a mod's code hash (core/mod_state.h) doesn't read every file again:
// a large asset mod would otherwise be hashed in full at every start, and
// again whenever any of its files changed. Each mod folder's code hash is
// kept too, so a start where nothing changed opens none of its files (the
// startup asset mount runs in DllMain).
//
// A file's entry counts only while its stamp is the same: size, times and
// the file's identity on its volume. A file replaced by another (an update,
// a package unpacked again) is a new file and is hashed again. The cache is
// as trusted as tome/state.json beside it: whoever can write one can write
// the other.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace tome::hash_cache {

struct Stamp {
    uint64_t size = 0;
    uint64_t written = 0;  // FILETIME
    uint64_t created = 0;
    uint64_t file_id = 0;  // the file's index on its volume
    uint32_t volume = 0;   // the volume's serial number

    bool operator==(const Stamp&) const = default;
};

// Entries kept at most: past that, the ones not used this session go first.
constexpr size_t kMaxEntries = 200000;
constexpr size_t kMaxFolders = 10000;

class Cache {
public:
    // What tome/cache/file-hashes.json held. False (and the cache empty) if
    // it isn't a cache this version wrote; entries that aren't valid are
    // skipped.
    bool parse(std::string_view text);
    // The entries used (looked up or put) this session: what wasn't is a
    // file no mod has any more. Every mod's files are looked up by the
    // first discover, so save after that.
    std::string serialize() const;

    // The file's SHA-256 (64 hex digits) if `path` was hashed with this
    // stamp; nullptr otherwise.
    const std::string* find(const std::string& path, const Stamp& stamp);
    void put(const std::string& path, const Stamp& stamp, std::string sha256);

    // A mod folder's code hash, by the folder's full path, if it was put
    // with this `print` (a hash of its code files' names, sizes and times);
    // nullptr otherwise. A hit counts as a use of every file entry inside
    // the folder too, so they stay for when one of its files changes.
    const std::string* find_folder(const std::string& dir, uint64_t print);
    void put_folder(const std::string& dir, uint64_t print, std::string sha256);

    // What serialize() gives differs from the file: an entry was put, or
    // one wasn't used and would be dropped.
    bool dirty() const;
    // After writing serialize(): forgets the unused entries.
    void mark_saved();
    size_t size() const { return entries_.size(); }
    size_t folders() const { return folders_.size(); }

private:
    struct Entry {
        Stamp stamp;
        std::string sha256;
        bool used = false;
    };
    struct Folder {
        uint64_t print = 0;
        std::string sha256;
        bool used = false;
    };
    std::map<std::string, Entry> entries_;   // by full path (UTF-8)
    std::map<std::string, Folder> folders_;  // by the folder's full path (UTF-8)
    bool dirty_ = false;
};

} // namespace tome::hash_cache
