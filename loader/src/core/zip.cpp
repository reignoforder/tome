// SPDX-License-Identifier: GPL-3.0-or-later AND Zlib
#include "zip.h"

#include <tome/utf8.h>

#include <algorithm>
#include <cstring>
#include <unordered_set>

#include "path_guard.h"

namespace tome::zip {
namespace {

// --- CRC-32 (IEEE, reflected) ------------------------------------------------

struct CrcTable {
    uint32_t values[256];
    CrcTable() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            values[i] = c;
        }
    }
};

// --- Inflate (RFC 1951), after zlib's puff.c ---------------------------------
//
// Altered from puff.c (zlib's contrib/puff, version 2.3; loader/third_party/
// puff/LICENSE): C++, a bit buffer and one-lookup tables for short codes,
// errors through failed_ instead of longjmp. Its notice:
//
//   Copyright (C) 2002-2013 Mark Adler, all rights reserved
//
//   This software is provided 'as-is', without any express or implied
//   warranty.  In no event will the author be held liable for any damages
//   arising from the use of this software.
//
//   Permission is granted to anyone to use this software for any purpose,
//   including commercial applications, and to alter it and redistribute it
//   freely, subject to the following restrictions:
//
//   1. The origin of this software must not be misrepresented; you must not
//      claim that you wrote the original software. If you use this software
//      in a product, an acknowledgment in the product documentation would be
//      appreciated but is not required.
//   2. Altered source versions must be plainly marked as such, and must not be
//      misrepresented as being the original software.
//   3. This notice may not be removed or altered from any source distribution.

constexpr int kMaxBits = 15;
constexpr int kFastBits = 9;  // codes up to this long decode with one table lookup

struct Huffman {
    uint16_t count[kMaxBits + 1];  // codes of each length
    uint16_t symbol[288];          // symbols ordered by code
    // By the next kFastBits input bits (first bit lowest): the symbol << 4 |
    // its code length, or 0 for a longer code (or none).
    uint16_t fast[1 << kFastBits];
};

// Builds a canonical code from code lengths. False if the lengths
// over-subscribe the code space; an incomplete code is accepted (a single
// distance code is legal), and decode() fails on a code it doesn't have.
bool build(Huffman& h, const uint8_t* lengths, int n) {
    std::memset(h.count, 0, sizeof h.count);
    std::memset(h.fast, 0, sizeof h.fast);
    for (int i = 0; i < n; ++i) h.count[lengths[i]]++;
    if (h.count[0] == n) return true;
    int left = 1;
    for (int len = 1; len <= kMaxBits; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return false;
    }
    uint16_t offs[kMaxBits + 1];
    offs[1] = 0;
    for (int len = 1; len < kMaxBits; ++len) offs[len + 1] = offs[len] + h.count[len];
    for (int i = 0; i < n; ++i) {
        if (lengths[i]) h.symbol[offs[lengths[i]]++] = static_cast<uint16_t>(i);
    }
    // Canonical codes in symbol order; deflate sends them first bit first,
    // so the table is indexed by the code reversed.
    uint32_t next[kMaxBits + 1];
    next[1] = 0;
    for (int len = 1; len < kMaxBits; ++len) next[len + 1] = (next[len] + h.count[len]) << 1;
    for (int i = 0; i < n; ++i) {
        int len = lengths[i];
        if (len == 0 || len > kFastBits) continue;
        uint32_t c = next[len]++, reversed = 0;
        for (int b = 0; b < len; ++b) reversed |= (c >> b & 1) << (len - 1 - b);
        for (uint32_t fill = reversed; fill < (1u << kFastBits); fill += 1u << len) {
            h.fast[fill] = static_cast<uint16_t>(i << 4 | len);
        }
    }
    return true;
}

class Inflater {
public:
    Inflater(std::string_view in, size_t size, std::string& out) : in_(in), size_(size), out_(out) {}

    bool run() {
        out_.clear();
        // Deflate expands at most 1032:1, so a tiny stream claiming a huge
        // size can't make it reserve memory it will never fill (a failed
        // allocation ends the process: no exceptions).
        out_.reserve(static_cast<size_t>(std::min<uint64_t>(size_, uint64_t(in_.size()) * 1032)));
        bool last;
        do {
            last = bits(1);
            int type = bits(2);
            bool ok = type == 0   ? stored()
                      : type == 1 ? fixed()
                      : type == 2 ? dynamic()
                                  : false;
            if (!ok || failed_) return false;
        } while (!last);
        // The rest of the last byte may be padding; whole bytes after it
        // (read ahead into the bit buffer or not) aren't.
        return out_.size() == size_ && pos_ == in_.size() && bit_count_ < 8;
    }

private:
    // Tops the bit buffer up to at least 25 bits, or to the end of the input.
    void refill() {
        while (bit_count_ <= 24 && pos_ < in_.size()) {
            bit_buf_ |= static_cast<uint32_t>(static_cast<unsigned char>(in_[pos_++])) << bit_count_;
            bit_count_ += 8;
        }
    }

    int bits(int need) {
        if (bit_count_ < need) {
            refill();
            if (bit_count_ < need) {
                failed_ = true;
                return 0;
            }
        }
        uint32_t value = bit_buf_ & ((1u << need) - 1);
        bit_buf_ >>= need;
        bit_count_ -= need;
        return static_cast<int>(value);
    }

    int decode(const Huffman& h) {
        if (bit_count_ < kFastBits) refill();
        uint16_t hit = h.fast[bit_buf_ & ((1u << kFastBits) - 1)];
        if (hit && (hit & 15) <= bit_count_) {
            bit_buf_ >>= hit & 15;
            bit_count_ -= hit & 15;
            return hit >> 4;
        }
        return decode_slow(h);
    }

    // Bit by bit: codes longer than kFastBits, and the end of the input.
    int decode_slow(const Huffman& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            code |= bits(1);
            if (failed_) return -1;
            int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;  // a code the (incomplete) table doesn't have
    }

    bool stored() {
        // Skip to the byte boundary and give back the whole bytes read ahead.
        pos_ -= bit_count_ / 8;
        bit_buf_ = 0;
        bit_count_ = 0;
        if (in_.size() - pos_ < 4) return false;
        auto byte = [&](size_t i) { return static_cast<unsigned>(static_cast<unsigned char>(in_[pos_ + i])); };
        unsigned len = byte(0) | byte(1) << 8;
        unsigned nlen = byte(2) | byte(3) << 8;
        pos_ += 4;
        if (len != (~nlen & 0xFFFFu)) return false;
        if (in_.size() - pos_ < len || size_ - out_.size() < len) return false;
        out_.append(in_.data() + pos_, len);
        pos_ += len;
        return true;
    }

    bool codes(const Huffman& lencode, const Huffman& distcode) {
        static constexpr uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                  31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                  2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static constexpr uint16_t kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                                   33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                                   1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                   6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int symbol = decode(lencode);
            if (symbol < 0) return false;
            if (symbol < 256) {
                if (out_.size() == size_) return false;
                if (out_.size() == out_.capacity()) grow(1);
                out_.push_back(static_cast<char>(symbol));
            } else if (symbol == 256) {
                return true;
            } else {
                symbol -= 257;
                if (symbol >= 29) return false;
                size_t len = kLenBase[symbol] + bits(kLenExtra[symbol]);
                symbol = decode(distcode);
                if (symbol < 0 || symbol >= 30) return false;
                size_t dist = kDistBase[symbol] + bits(kDistExtra[symbol]);
                if (failed_ || dist > out_.size() || size_ - out_.size() < len) return false;
                // In pieces of at most `dist` bytes, so no piece overlaps
                // what it writes (a short distance repeats a pattern).
                grow(len);
                for (size_t from = out_.size() - dist; len;) {
                    size_t n = std::min(len, dist);
                    out_.append(out_.data() + from, n);
                    from += n;
                    len -= n;
                }
            }
        }
    }

    bool fixed() {
        static Huffman lencode, distcode;
        static bool built = [] {
            uint8_t lengths[288];
            int i = 0;
            for (; i < 144; ++i) lengths[i] = 8;
            for (; i < 256; ++i) lengths[i] = 9;
            for (; i < 280; ++i) lengths[i] = 7;
            for (; i < 288; ++i) lengths[i] = 8;
            build(lencode, lengths, 288);
            for (i = 0; i < 30; ++i) lengths[i] = 5;
            build(distcode, lengths, 30);
            return true;
        }();
        (void)built;
        return codes(lencode, distcode);
    }

    bool dynamic() {
        static constexpr uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        int nlen = bits(5) + 257;
        int ndist = bits(5) + 1;
        int ncode = bits(4) + 4;
        if (failed_ || nlen > 286 || ndist > 30) return false;

        uint8_t lengths[320] = {};
        for (int i = 0; i < ncode; ++i) lengths[kOrder[i]] = static_cast<uint8_t>(bits(3));
        Huffman lencode, distcode;
        if (failed_ || !build(lencode, lengths, 19)) return false;

        for (int index = 0; index < nlen + ndist;) {
            int symbol = decode(lencode);
            if (symbol < 0) return false;
            if (symbol < 16) {
                lengths[index++] = static_cast<uint8_t>(symbol);
                continue;
            }
            uint8_t len = 0;
            int repeat;
            if (symbol == 16) {
                if (index == 0) return false;
                len = lengths[index - 1];
                repeat = 3 + bits(2);
            } else if (symbol == 17) {
                repeat = 3 + bits(3);
            } else {
                repeat = 11 + bits(7);
            }
            if (failed_ || index + repeat > nlen + ndist) return false;
            while (repeat--) lengths[index++] = len;
        }
        if (lengths[256] == 0) return false;  // no end-of-block code
        if (!build(lencode, lengths, nlen) || !build(distcode, lengths + nlen, ndist)) return false;
        return codes(lencode, distcode);
    }

    // Room for `more` bytes without reallocating (append() copies from
    // out_ itself), growing geometrically but never past size_.
    void grow(size_t more) {
        if (out_.capacity() - out_.size() >= more) return;
        out_.reserve(std::min(size_, std::max(out_.size() + more, out_.capacity() * 2)));
    }

    std::string_view in_;
    size_t pos_ = 0;
    uint32_t bit_buf_ = 0;
    int bit_count_ = 0;
    bool failed_ = false;
    size_t size_;
    std::string& out_;
};

// --- Archive structure -------------------------------------------------------

constexpr uint32_t kEndSignature = 0x06054b50;
constexpr uint32_t kCentralSignature = 0x02014b50;
constexpr uint32_t kLocalSignature = 0x04034b50;
constexpr size_t kEndSize = 22;
constexpr size_t kCentralSize = 46;
constexpr size_t kLocalSize = 30;

uint16_t u16(std::string_view s, size_t at) {
    auto p = reinterpret_cast<const unsigned char*>(s.data()) + at;
    return static_cast<uint16_t>(p[0] | p[1] << 8);
}

uint32_t u32(std::string_view s, size_t at) {
    return u16(s, at) | static_cast<uint32_t>(u16(s, at + 2)) << 16;
}

struct UpcaseRun {
    uint16_t first, last;
    int32_t delta;
    uint8_t stride;
};

constexpr UpcaseRun kUpcase[] = {
#include "upcase_table.inc"
};

uint32_t upcase(uint32_t cp) {
    if (cp < 0x80) return cp >= 'a' && cp <= 'z' ? cp - 32 : cp;
    if (cp > 0xFFFF) return cp;
    auto run = std::upper_bound(std::begin(kUpcase), std::end(kUpcase), cp,
                                [](uint32_t value, const UpcaseRun& r) { return value < r.first; });
    if (run == std::begin(kUpcase)) return cp;
    --run;
    if (cp > run->last || (cp - run->first) % run->stride) return cp;
    return static_cast<uint32_t>(static_cast<int32_t>(cp) + run->delta);
}

bool forbidden_char(unsigned char c) {
    return c < 0x20 || c == 0x7F || c == '<' || c == '>' || c == ':' || c == '"' || c == '|' || c == '?' ||
           c == '*' || c == '\\';
}

} // namespace

uint32_t crc32(std::string_view data, uint32_t crc) {
    static const CrcTable table;
    crc = ~crc;
    for (unsigned char c : data) crc = table.values[(crc ^ c) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

bool inflate(std::string_view in, size_t size, std::string& out) {
    return Inflater(in, size, out).run();
}

std::string fold_name(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (size_t i = 0; i < name.size();) {
        uint32_t cp;
        size_t n = tome_utf8_decode(name.data() + i, name.size() - i, &cp);
        char buf[4];
        out.append(buf, tome_utf8_encode(upcase(cp), buf));
        i += n;
    }
    return out;
}

bool safe_name(std::string_view name) {
    if (name.empty()) return false;
    std::string_view body = name;
    if (body.back() == '/') body.remove_suffix(1);  // a directory
    if (body.empty() || body.front() == '/') return false;
    for (unsigned char c : body) {
        if (forbidden_char(c)) return false;
    }
    std::string copy(body);
    if (!tome_utf8_valid(copy.c_str())) return false;
    for (size_t i = 0; i <= body.size();) {
        size_t end = body.find('/', i);
        if (end == std::string_view::npos) end = body.size();
        std::string_view part = body.substr(i, end - i);
        // Windows drops a trailing space or dot, so "a." and "a" are one file.
        if (part.empty() || part == "." || part.back() == ' ' || part.back() == '.') return false;
        i = end + 1;
    }
    // "..", reserved device names.
    return !path_guard::is_unsafe(body);
}

bool read_directory(std::string_view archive, std::vector<Entry>& entries, std::string& error,
                    const Limits& limits) {
    entries.clear();
    // The end record is the last 22 bytes plus a comment of up to 64 KiB.
    if (archive.size() < kEndSize) {
        error = "not a zip archive";
        return false;
    }
    size_t end = std::string_view::npos;
    size_t lowest = archive.size() > kEndSize + 0xFFFF ? archive.size() - kEndSize - 0xFFFF : 0;
    for (size_t at = archive.size() - kEndSize + 1; at-- > lowest;) {
        if (u32(archive, at) == kEndSignature && at + kEndSize + u16(archive, at + 20) == archive.size()) {
            end = at;
            break;
        }
    }
    if (end == std::string_view::npos) {
        error = "not a zip archive";
        return false;
    }
    uint16_t disk = u16(archive, end + 4), dir_disk = u16(archive, end + 6);
    uint16_t on_disk = u16(archive, end + 8), count = u16(archive, end + 10);
    uint32_t dir_size = u32(archive, end + 12), dir_offset = u32(archive, end + 16);
    if (count == 0xFFFF || dir_size == 0xFFFFFFFF || dir_offset == 0xFFFFFFFF) {
        error = "zip64 archives aren't supported";
        return false;
    }
    if (disk != 0 || dir_disk != 0 || on_disk != count) {
        error = "split archives aren't supported";
        return false;
    }
    if (dir_offset > end || dir_size > end - dir_offset) {
        error = "central directory out of range";
        return false;
    }
    if (count > limits.max_entries) {
        error = "too many entries (" + std::to_string(count) + ")";
        return false;
    }

    std::unordered_set<std::string> seen;   // folded names, without a trailing '/'
    std::unordered_set<std::string> files;  // folded names of files (not folders)
    uint64_t total = 0;
    size_t at = dir_offset;
    const size_t dir_end = static_cast<size_t>(dir_offset) + dir_size;
    for (uint16_t i = 0; i < count; ++i) {
        if (dir_end - at < kCentralSize || u32(archive, at) != kCentralSignature) {
            error = "central directory is damaged";
            return false;
        }
        uint16_t flags = u16(archive, at + 8);
        Entry entry;
        entry.method = u16(archive, at + 10);
        entry.crc = u32(archive, at + 16);
        entry.compressed_size = u32(archive, at + 20);
        entry.size = u32(archive, at + 24);
        uint16_t name_len = u16(archive, at + 28), extra_len = u16(archive, at + 30),
                 comment_len = u16(archive, at + 32);
        uint16_t start_disk = u16(archive, at + 34);
        entry.local_offset = u32(archive, at + 42);
        size_t record = kCentralSize + name_len + extra_len + comment_len;
        if (dir_end - at < record) {
            error = "central directory is damaged";
            return false;
        }
        entry.name.assign(archive.data() + at + kCentralSize, name_len);
        at += record;

        auto quoted = [&] { return "\"" + entry.name + "\""; };
        if (flags & 0x41) {  // bit 0 encrypted, bit 6 strong encryption
            error = quoted() + " is encrypted";
            return false;
        }
        if (entry.compressed_size == 0xFFFFFFFF || entry.size == 0xFFFFFFFF ||
            entry.local_offset == 0xFFFFFFFF) {
            error = quoted() + ": zip64 entries aren't supported";
            return false;
        }
        if (start_disk != 0) {
            error = "split archives aren't supported";
            return false;
        }
        if (entry.method != 0 && entry.method != 8) {
            error = quoted() + " uses compression method " + std::to_string(entry.method) +
                    " (only stored and deflate are supported)";
            return false;
        }
        if (entry.method == 0 && entry.compressed_size != entry.size) {
            error = quoted() + " is damaged";
            return false;
        }
        if (!safe_name(entry.name)) {
            error = "unsafe file name " + quoted();
            return false;
        }
        if (entry.directory() && entry.size != 0) {
            error = quoted() + " is damaged";
            return false;
        }
        std::string bare = fold_name(entry.name);
        if (entry.directory()) bare.pop_back();
        if (!seen.insert(bare).second) {
            error = quoted() + " is in the archive twice";
            return false;
        }
        if (!entry.directory()) files.insert(std::move(bare));
        total += entry.size;
        if (total > limits.max_total) {
            error = "unpacks to more than " +
                    (limits.max_total >= (1u << 20) ? std::to_string(limits.max_total >> 20) + " MiB"
                                                    : std::to_string(limits.max_total) + " bytes");
            return false;
        }
        entries.push_back(std::move(entry));
    }
    // A file can't sit at "a" and inside a folder "a/" at once.
    for (const Entry& entry : entries) {
        const std::string& name = entry.name;
        for (size_t slash = name.find('/'); slash != std::string::npos && slash + 1 < name.size();
             slash = name.find('/', slash + 1)) {
            if (files.count(fold_name(std::string_view(name).substr(0, slash)))) {
                error = "\"" + name.substr(0, slash) + "\" is both a file and a folder";
                return false;
            }
        }
    }
    return true;
}

bool extract(std::string_view archive, const Entry& entry, std::string& out, std::string& error) {
    out.clear();
    const std::string quoted = "\"" + entry.name + "\"";
    size_t at = entry.local_offset;
    if (at > archive.size() || archive.size() - at < kLocalSize || u32(archive, at) != kLocalSignature) {
        error = quoted + ": local header out of range";
        return false;
    }
    size_t data = at + kLocalSize + u16(archive, at + 26) + u16(archive, at + 28);
    if (data > archive.size() || archive.size() - data < entry.compressed_size) {
        error = quoted + ": data out of range";
        return false;
    }
    std::string_view packed = archive.substr(data, entry.compressed_size);
    if (entry.method == 0) {
        out.assign(packed);
    } else if (!inflate(packed, entry.size, out)) {
        error = quoted + " is damaged (deflate)";
        return false;
    }
    if (crc32(out) != entry.crc) {
        error = quoted + " is damaged (CRC)";
        return false;
    }
    return true;
}

} // namespace tome::zip
