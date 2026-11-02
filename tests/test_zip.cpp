// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstring>

#include "core/zip.h"
#include "test.h"

using namespace tome;

namespace {

// Builds an archive the way packers do. Entries are stored unless `packed`
// gives their deflated bytes.
struct TestFile {
    std::string name;
    std::string data;
    std::string packed;  // raw deflate of data; empty: stored
    uint16_t flags = 0;
};

void put16(std::string& s, uint32_t v) {
    s.push_back(static_cast<char>(v & 0xFF));
    s.push_back(static_cast<char>(v >> 8 & 0xFF));
}

void put32(std::string& s, uint32_t v) {
    put16(s, v & 0xFFFF);
    put16(s, v >> 16);
}

std::string make_zip(const std::vector<TestFile>& files, const std::string& comment = "") {
    std::string out, dir;
    for (const TestFile& f : files) {
        const bool deflated = !f.packed.empty();
        const std::string& body = deflated ? f.packed : f.data;
        uint32_t offset = static_cast<uint32_t>(out.size());
        uint32_t crc = zip::crc32(f.data);
        put32(out, 0x04034b50);
        put16(out, 20);
        put16(out, f.flags);
        put16(out, deflated ? 8 : 0);
        put32(out, 0);  // time, date
        put32(out, crc);
        put32(out, static_cast<uint32_t>(body.size()));
        put32(out, static_cast<uint32_t>(f.data.size()));
        put16(out, static_cast<uint32_t>(f.name.size()));
        put16(out, 0);
        out += f.name;
        out += body;

        put32(dir, 0x02014b50);
        put16(dir, 20);
        put16(dir, 20);
        put16(dir, f.flags);
        put16(dir, deflated ? 8 : 0);
        put32(dir, 0);
        put32(dir, crc);
        put32(dir, static_cast<uint32_t>(body.size()));
        put32(dir, static_cast<uint32_t>(f.data.size()));
        put16(dir, static_cast<uint32_t>(f.name.size()));
        put16(dir, 0);  // extra
        put16(dir, 0);  // comment
        put16(dir, 0);  // disk
        put16(dir, 0);  // internal attributes
        put32(dir, 0);  // external attributes
        put32(dir, offset);
        dir += f.name;
    }
    uint32_t dir_offset = static_cast<uint32_t>(out.size());
    out += dir;
    put32(out, 0x06054b50);
    put16(out, 0);
    put16(out, 0);
    put16(out, static_cast<uint32_t>(files.size()));
    put16(out, static_cast<uint32_t>(files.size()));
    put32(out, static_cast<uint32_t>(dir.size()));
    put32(out, dir_offset);
    put16(out, static_cast<uint32_t>(comment.size()));
    out += comment;
    return out;
}

std::string bytes(const char* s, size_t n) { return std::string(s, n); }

// zlib, level 6, Z_FIXED: "Hello, Hello, Hello, Tome!"
const std::string kFixed = bytes("\xf3\x48\xcd\xc9\xc9\xd7\x51\xf0\x40\xa1\x42\xf2\x73\x53\x15\x01", 16);
// zlib, level 0: "plain bytes" as one stored block.
const std::string kStored = bytes("\x01\x0b\x00\xf4\xff\x70\x6c\x61\x69\x6e\x20\x62\x79\x74\x65\x73", 16);

// zlib, level 9, a dynamic Huffman block: dynamic_text().
const char kDynamicBytes[] =
    "\x85\x99\x5d\x6e\x55\x31\x0c\x84\xdf\x59\xc5\x59\x42\x9c\x3f\xdb\x2c\x07\x54\x44\x45\xd5\xaa\x15"
    "\x08\x96\x8f\xd0\xb5\xcd\xcb\x99\xc9\xb3\xef\xc9\xcf\xc4\xb1\xe7\xcb\x7d\x79\x7e\x7d\xba\xda\xe7"
    "\xeb\xe7\xf7\xa7\xeb\xfd\xd7\xf3\xd7\x1f\xd7\x97\x8f\xb7\xdf\xaf\xd7\xb7\xb7\x3f\x57\xfb\xf4\xf2"
    "\x2f\x2a\xf7\x51\x79\x44\xfb\x7d\x74\x3e\xa2\xe3\x3e\xea\x8f\xe8\x04\x23\xef\x47\x78\xdd\x87\xfb"
    "\x7a\x84\xf7\x7d\x78\xc4\xd7\x0a\x16\x16\x73\xdb\x7d\x78\xc7\xc2\xfd\x3e\x6c\xb1\x6b\x01\x92\x8d"
    "\x08\x03\xcd\x7a\x8c\x2e\x48\x35\x8d\x38\xd0\x4d\x7b\xc4\x81\x72\x19\x06\xca\x8d\x5c\x3d\x90\x6e"
    "\xe7\xf7\x40\x3b\x0f\xe5\x05\x88\x37\x72\xfb\x40\x3d\x8d\x8c\xea\x40\x3d\x89\xf9\x3b\x90\x6f\xc5"
    "\xf8\x1d\xc8\xe7\x71\xf4\x1d\xc8\x37\x43\xfe\x0e\xe4\xf3\xcc\x69\xa0\xdf\xcc\xf9\x81\x7e\x9e\xe3"
    "\x03\xfd\x56\xee\x1f\xe8\x67\x11\x06\xf2\xed\x90\x7f\x00\xf9\x7a\x64\xcf\x00\xf2\x59\x8c\x3f\x80"
    "\x7c\x2b\x2f\x2d\x90\xaf\xc7\xf1\x0c\x20\x9f\xc5\xd5\x1a\x40\xbe\x1d\xf2\x0e\x74\x73\x73\x7f\x40"
    "\x3e\xc9\xef\x91\x7c\x71\xfc\x03\xe9\x17\xf1\x09\xf4\x9b\xa1\xcf\x04\xfa\x8d\xd8\xff\x04\xfa\x49"
    "\x7e\x0f\xf4\xcb\xe9\x51\xf6\x45\x76\x4d\x20\x9f\x85\x3c\x13\xc8\xa7\x59\x55\x81\x7c\x9a\xdf\x03"
    "\xf9\x34\xe7\x77\x1e\x5f\x8d\x8f\xbf\x84\xaf\x6f\x75\xbe\xbf\x35\xb8\x3e\x6b\x52\x79\xd7\xe2\xa7"
    "\xb3\x36\x3f\xdd\xa5\x3c\x3b\x96\xf1\xec\x5a\xce\xb3\x73\x37\x9e\xdd\x5b\xf8\xed\xd8\x9d\xdf\xae"
    "\x3d\xf8\xed\xdc\x93\xdf\xee\xbd\x78\x75\xd8\x9b\x57\x97\xad\xbc\x3a\x6d\xe3\xd5\x6d\x3b\x2d\x8e"
    "\xda\x78\x6d\x55\xe1\xb5\x59\x3b\xaf\xed\x3a\x78\x6f\xd0\xc9\x7b\x8b\x2e\xde\x9b\x74\xf3\xde\xa6"
    "\xca\x7b\xa3\x1a\xef\xad\xea\xbc\x37\x5b\xe3\xbd\xdd\x84\x7b\x03\xeb\xdc\x5b\xd8\xa0\xd6\xc4\x26"
    "\x77\x36\xb6\xb8\x33\xb2\xcd\x9d\x95\x29\x35\x66\x66\xdc\xd7\x99\x1f\x6c\x61\xe3\xae\xd2\x85\x9b"
    "\x52\xef\xdc\xd3\xfa\xe0\x96\xd8\x27\x35\xd4\xbe\xa8\x1b\xf7\x4d\xad\xbc\x2b\xe5\x00\x37\xfe\xb5"
    "\xd3\xb9\xa5\x35\xba\x74\x69\xc2\xb7\x2e\xed\xa0\x9d\xb4\xc1\xc5\x97\x36\xf9\xe9\x49\x5b\xfc\xf8"
    "\xa5\xed\x13\x17\xf0\xfc\x93\x66\x07\x32\x68\x7e\x40\x03\x84\x1e\xc5\x06\x10\x3e\x32\x7e\xb8\xc1"
    "\x82\xe8\xa3\xf0\x00\xe1\x47\xf1\x01\x04\x90\xe2\xa3\xcd\xab\x98\xc8\xa1\x0c\x0a\x62\x90\x55\x53"
    "\x38\x2f\xc4\x82\x28\x64\x16\xa4\x09\x6f\x05\xd2\x0f\xbd\x44\x10\x88\x78\x4d\x31\x79\x37\x13\x84"
    "\x22\x96\xf1\xcd\xbb\xa9\xf4\x43\x3b\x16\x08\x23\x35\x85\x73\x43\x20\x90\x47\xf2\xac\x20\x90\xe4"
    "\xbd\x1b\x07\x4f\x23\x08\x49\xd2\x14\x09\x62\x12\xa9\x11\x16\xb7\x65\x82\xa8\x64\xd7\x0f\x0e\xc6"
    "\x50\x10\x97\x8c\xd2\xc1\xb9\x35\x15\x44\x26\xb9\x04\x44\x26\x69\x8d\x65\x1e\xbc\xb5\x20\x36\x49"
    "\x73\x2e\x88\x4e\xb4\x46\x58\x1c\x0f\x04\xf2\x49\xfd\x40\x4f\x53\xd8\x69\x91\x7e\xd8\x26\x62\x94"
    "\x12\x0a\x41\x4a\x2a\xbd\x0e\x8c\x27\x88\x52\xea\xb0\x11\xa6\x54\xba\x20\x50\xa9\x84\x43\xa4\x52"
    "\x29\xbb\x0e\xa0\x2c\x88\x55\xea\xda\x20\x58\xa9\x8b\x87\x68\xa5\xae\x2e\xc2\x95\xba\xfc\xfb\xf0"
    "\xda\x20\x10\x58\x52\x28\x48\x2c\x59\xc2\x10\xb2\x54\x11\x84\xcc\x92\xf1\xc3\x83\x8d\x20\x68\xa9"
    "\x42\x8e\xa8\xa5\x5a\x01\xe2\x96\x6a\x26\x08\x5c\xaa\x1d\xe9\xe1\xd5\x4b\x10\xba\x54\x4b\x44\xec"
    "\x52\x4d\x15\xc1\x4b\xb5\x65\x44\x2f\xd5\xd8\xf5\xf4\x74\x88\xf8\xe5\xff\xdb\xa3\x1f\xec\x09\x22"
    "\x98\x1c\x00\x11\x4c\x19\x24\x3b\xbd\xbe\x42\x86\xc9\xa3\x40\x14\x93\x2a\x20\x8a\x29\x9b\x88\x30"
    "\xa6\x8c\xa6\x1d\x9e\xaf\x05\x91\x4c\x99\x5d\x84\x32\x65\x97\x11\xcb\x94\xe1\x46\x30\x93\x4b\x70"
    "\xfe\xf6\x2f\x10\x66\x32\x3e\xf9\xff\x0e\x88\x66\xea\xfb\x7d\x98\x5f\x0f\xeb\xb7\x93\x00\x58\xc2"
    "\xbf"
    ;

std::string dynamic_text() {
    std::string text;
    char line[64];
    for (int i = 0; i < 200; ++i) {
        std::snprintf(line, sizeof line, "line %d: the quick brown fox %d\n", i, i * i % 97);
        text += line;
    }
    return text;
}

std::string dynamic_packed() { return std::string(kDynamicBytes, sizeof kDynamicBytes - 1); }

// Letters 'a' + the trailing zero bits of an LCG: halving frequencies, so the
// Huffman codes run past the one-lookup length.
std::string skewed_text() {
    std::string text;
    uint32_t seed = 1;
    for (int i = 0; i < 6000; ++i) {
        seed = seed * 1103515245 + 12345;
        uint32_t v = seed >> 8;
        int b = 0;
        while (b < 24 && !(v >> b & 1)) ++b;
        text.push_back(static_cast<char>('a' + b));
    }
    return text;
}

// zlib, level 9: skewed_text() in two dynamic blocks with Z_SYNC_FLUSH (an
// empty stored block) between them.
const char kSkewedBytes[] =
    "\x84\x56\x59\x76\xe3\x30\x0c\x3b\x2b\x37\xd9\x4d\xe7\xfe\xdf\x43\x00\x94\xed\x74\x79\x4d\xf3\x12"
    "\x57\x96\x49\x8a\x00\x88\x78\x85\x59\xb8\x87\x79\x78\x59\xbf\xdc\xb2\x97\x7a\xcd\x7a\xd5\xd3\xa3"
    "\xd7\x83\xeb\x96\xbd\xd4\x17\x85\xef\x7e\xf1\xbf\xc0\x9f\xe3\xbe\x5b\x3f\xcf\x10\xd8\x9e\xfd\xee"
    "\x50\xd8\x82\xe7\x18\x32\x43\x77\x0a\xe9\x2c\xf0\x58\xf4\x0e\xae\xf6\x7a\x15\x82\xe2\x16\x1f\xe8"
    "\x0f\x63\x8e\x50\xb4\x7e\xe7\x95\x2c\x10\xb1\x0b\x56\xd5\x2a\xa2\xaf\xa6\xd0\x2e\xa3\x12\x25\x46"
    "\xa1\x60\x96\x7b\x98\xa7\x4d\xc9\x58\xc3\xf3\x4c\xcc\x12\xb9\xd8\xe5\x26\x63\xa0\x1f\x5a\x99\x17"
    "\x6a\x4d\x14\x64\x73\x20\xbc\x0e\xac\xf7\x35\xc3\x26\xab\x48\x94\xe1\xa8\xea\xca\x74\x62\x11\x3b"
    "\x94\x06\x77\x99\xce\xaf\xd4\xfd\x7d\xb0\x19\x2c\x61\xce\xcd\x8f\xc5\xd8\x86\xda\x9d\x07\x67\x09"
    "\xb9\x43\x4d\x21\x0c\x39\xd8\x1d\xd1\x47\xf7\x7c\x14\xef\x7c\x46\x7d\x41\x37\x16\xdb\x61\xc5\xd3"
    "\x7b\x10\x51\x1e\xaf\xb1\xc6\xb6\xa5\xaa\x00\x40\xf8\x81\x0d\xdc\xc3\x33\xe0\xbc\x3c\x42\x44\xac"
    "\xe4\x7f\xc4\x08\x65\x54\xa8\xea\x41\x80\x61\xcd\xcf\x14\xa5\x74\x5a\x45\x36\x64\x4a\x35\x19\xe8"
    "\x4f\x17\x00\x57\xa4\x89\x01\x68\xa0\x60\xdd\x61\x7d\x0e\xca\x64\xe9\x24\x0f\xf6\xc5\xf4\xca\x5f"
    "\xa1\xd6\x7e\x28\x87\xaf\x00\xb9\x0a\x38\x7d\x46\xa6\x8a\x88\xb4\xbb\xff\xea\x79\xd5\xae\xb9\x09"
    "\x25\x78\x32\xf6\x51\x76\x13\x71\x16\x14\xcd\x33\xdf\x0d\x55\x45\xdd\xb4\x1c\x9e\x19\xcf\x00\x76"
    "\x56\x4c\x23\x7c\x76\x28\xcd\x0a\xf0\x46\xfc\xb3\xa8\x01\xdd\xdd\x07\x31\x61\x49\xbe\x7f\x28\x36"
    "\x29\x9f\x21\xbc\x44\x5b\xc1\x3e\x09\xc8\xba\xb9\x52\xb6\xf4\x47\xf1\xe9\x43\x86\x21\xbc\x7f\x00"
    "\x3d\xb5\x18\x85\x42\xb4\xa0\xaf\xd7\xc8\xc1\xe2\x65\xa4\x8a\x63\xc9\xbb\x73\xf1\xa9\xd3\x86\x0d"
    "\x5b\x1e\xfa\x07\x5f\x07\x5d\xdb\xa2\x49\x65\x23\x3c\xd3\x6c\x2c\x1d\xe0\x8c\xce\x03\x98\x0b\x8f"
    "\x55\xef\xc8\x73\x34\x46\xed\x0c\x03\x74\x26\xdf\x5d\xd7\x3b\xd0\x1a\x17\x50\x29\x82\xb1\x6a\x15"
    "\xc1\x99\xb1\xe1\x65\xb0\x9e\x5c\x97\xfe\xa7\x81\x23\x1a\xb7\x93\xf3\xe3\x12\xe1\x92\xa4\x98\x42"
    "\x72\xce\x07\xfe\xa8\x75\x98\xe8\x7b\x58\x70\x2e\x71\x22\xcd\xc0\x28\xa0\x94\x17\x02\x93\x29\x8a"
    "\x88\x8f\xfe\x73\xf4\x5f\xef\xfa\x27\x3f\x27\x68\xdc\xfa\x57\x6c\x1b\xf2\xf9\x45\x5b\xf1\x76\x06"
    "\x8c\x48\xb8\xb1\x5b\x41\xf6\xdd\xc5\x7b\xdc\x74\x1d\xd1\xcc\x98\xc5\x41\x0f\x22\x8a\xec\x52\xd2"
    "\x06\x19\x77\x23\x69\x00\xda\xa3\x49\x74\x9e\x64\x4b\xcf\xea\x85\xd1\x97\x23\x3d\x4d\xf7\x6f\xfa"
    "\x4f\xf3\x91\x4b\xbc\xeb\x3f\x98\x5f\xf3\x38\xf3\xd6\xff\x79\xcc\xf4\x37\x52\x18\xb0\xae\xfa\xaa"
    "\x7f\xb9\x53\xfa\x18\x4c\x6e\xfd\x47\xee\xd1\x2a\xc8\xbb\xa7\x31\x83\xbc\x31\x88\x19\xa0\x61\x77"
    "\xff\xd5\xf3\x88\x5b\xff\x33\x9e\x23\xbf\xe9\x3f\x87\xe0\x6b\xfd\xa4\xff\x88\x4b\xff\xff\x1a\x81"
    "\x05\xfa\xd6\x9b\xfe\x63\xd2\xc4\x36\x29\x29\x62\x40\x77\x36\x63\xeb\x3f\xc4\xf7\x21\x02\x29\x1f"
    "\x4b\xf6\x45\x18\xfe\x29\xd4\x24\x20\xeb\xe6\xca\x2e\xf7\xb9\xad\xcb\xc5\x6d\x7b\x99\x8d\x68\xb2"
    "\xa6\xc5\x68\xdf\xc3\xff\x35\x56\x38\xb9\xb6\xff\x2f\xcc\xe7\x9c\x99\x77\x4a\xff\x3d\xc9\x7e\xf6"
    "\xff\x2d\x1a\x6c\xff\x24\x41\x45\x9d\x1a\xd0\x5e\xec\xc1\xc2\xf4\xbe\xfc\xff\xb8\xfc\xff\xc8\xdc"
    "\xfe\xcf\x07\xea\xe1\xff\x9c\xe2\x76\xd4\x17\xff\xcf\xa9\x7a\xfc\x3f\x2f\xff\x6f\xe6\x14\x45\x9f"
    "\x97\xfe\x13\x36\xfa\x87\xff\x17\xfc\xea\x37\xff\xaf\x75\xe9\xff\x1c\xab\x06\x77\x16\xda\xdb\xd4"
    "\x65\x15\xc7\x77\xff\xcf\x1e\xa7\x7f\xfa\x7f\x5e\xfe\x7f\xce\xb9\x4d\x02\x28\xf9\xe5\xf6\x7f\xd2"
    "\x76\xdd\xfe\x5f\x97\xfe\x73\xff\x76\xc3\x24\x3c\x9f\xfe\x5f\x37\x5d\x47\x34\x33\x66\xdf\xfd\xbf"
    "\xbc\x2e\x0e\x69\x6c\x14\x7f\xfe\xfd\xe6\xff\xb9\x6e\xff\x5f\x62\xe1\x0f\xfe\x4f\xd7\x73\xce\xaa"
    "\x87\xfe\x8b\x10\xff\xe4\xff\xf5\xb7\xff\x23\xe2\xf1\x83\xff\xe7\xd0\x3e\x47\xff\xdd\xd3\x63\x06"
    "\x79\x46\x95\x8a\x88\xf5\xcd\xff\x33\x6f\xfd\xcf\x78\xae\x5f\xfc\xff\xf5\x9b\xff\x67\x3d\xfc\x7f"
    "\x69\x02\xbf\xfb\x3f\x34\x27\xa4\xa7\xc0\x99\x99\x92\xe6\xbb\xff\xa7\x7e\x49\xe7\xc3\xff\xd7\x9b"
    "\xff\x2f\xfb\x0f\x00\x00\xff\xff\xbd\x98\xcb\x76\xc2\x30\x0c\x44\xbf\x95\x44\x33\x76\xc9\xff\xef"
    "\x6b\xc9\xf2\x2b\x71\x48\xda\x42\x17\x2c\x38\x07\x70\xb0\x2d\xdd\xb9\x1a\xf8\x1f\x76\xfc\x67\x5f"
    "\xff\xdc\xf3\x5f\x46\xfe\x5b\x50\xb7\x74\x5c\xf9\x9f\x13\x96\x6e\xa1\x76\x00\xe4\xbc\x71\xc2\x7f"
    "\xa4\xeb\x8f\x1d\xff\x71\xe4\x3f\xec\x86\x08\x9c\xff\x7a\xcc\x12\x73\xd2\x5a\x82\xf4\xfc\xe7\xc3"
    "\xe1\x54\xf9\x1f\x3b\xfe\x8b\x2f\x26\xd6\xfd\x56\x7f\xea\xf4\x08\xda\x6a\x8c\xdd\x03\xff\x65\xc6"
    "\x7f\x94\xfe\xbb\x48\xab\x7f\xa9\xfc\x37\x2a\xe5\x86\xd3\x42\x54\x98\xf3\xff\x89\x8e\xff\xa2\xfd"
    "\x9c\xf5\x04\x7c\xa5\x55\xfc\x27\x73\xcc\x58\xca\x25\x1d\xf9\x4f\x5e\xf2\x9f\x3b\xfe\xcb\x94\xff"
    "\x92\xf9\x8f\x1b\xfc\x17\xbf\xf0\x29\xa5\xe3\xc8\xff\x74\x4d\xe2\xa2\x06\x90\x3f\x63\x5d\xce\x6a"
    "\x45\xdf\x52\xd4\xe5\x80\x9e\xff\xe1\x50\xff\xb8\xe0\xbf\xf5\x63\xb0\xd5\xbf\x88\x77\x7f\x8b\x9f"
    "\x69\xc9\x67\xb1\xae\x03\xff\xb7\xac\x33\x5c\x50\xf9\x8f\x1d\xff\x89\xca\x7f\x29\xfc\x2f\xa9\x5c"
    "\x72\x23\x3d\xe3\x3f\x0e\xf5\x8f\x62\xa2\x72\xc1\x7f\xb1\x10\x88\x6a\x1a\x7b\xfe\x17\x48\xe5\x8a"
    "\x68\xfc\xc7\x19\xff\xed\xca\x6b\xba\x82\x7f\x2d\xf3\xd8\xf3\x80\x9e\xcc\xea\x71\xd2\x9a\xea\x84"
    "\xff\x52\x70\x65\x3b\x9e\x3e\x2d\xbe\xc5\xe9\xc5\x8e\xff\x3c\xf1\x7f\x8e\xfe\x1f\xce\xfc\x3f\x3a"
    "\x19\xaf\xfc\x9f\x73\xff\x27\x7f\xea\xff\xd1\x9f\xda\xf9\xbf\x55\xfe\x07\x35\x32\x2d\x7a\xd6\xfa"
    "\xc7\x3d\xff\x8f\xf7\xfd\x1f\xb7\xfc\x1f\x77\xfc\x1f\x73\xff\x87\xfb\x3f\xae\xfd\x3f\x96\xec\xb6"
    "\x3d\xe2\xa9\xff\x73\x71\xb1\xb3\x36\x3b\xf7\x7f\xf4\xfe\x8f\x73\xfe\x63\xe7\xff\x9c\xf2\x1f\xff"
    "\xe7\xff\x58\x8b\x5a\xe5\x23\x87\xf9\x3f\x6d\x18\xf4\xca\xff\x19\x5a\xfd\xf3\x97\xfe\x8f\xa3\xff"
    "\xc7\x91\xff\xc8\x96\xab\xb3\x34\x34\xfe\x73\xea\xff\x31\xdf\x77\x7c\xc6\xff\x31\xf2\x9f\x06\xa5"
    "\xb4\xef\x5f\x95\xff\x78\xf8\x98\x88\xee\xff\x78\x8b\xff\x63\x45\xef\xff\x36\x33\xa1\xc6\x76\xdc"
    "\xf7\xff\xcd\x17\x63\xf1\x7f\xbe\xf4\xff\x78\xee\xff\xda\x48\xd9\xea\x1f\x95\xff\x9c\xf8\x7f\xb8"
    "\xe3\xff\x9c\xfb\x3f\x7d\xa4\x30\xf8\x3f\xff\xee\xff\x61\xca\x7f\xdc\xf7\x7f\xfa\xc0\x6b\xee\xff"
    "\xb1\xfa\x3f\x9a\xff\x2b\xf1\xd5\xe9\x24\xb1\xf5\x03\xfe\x0f\x1c\xfc\x1f\xe1\x1d\xfe\x1f\xf7\xfe"
    "\xcf\xdf\xfa\x3f\x70\xc1\x7f\xfd\x0f\x36\x81\xe5\x8c\xff\xdf"
    ;

std::string skewed_packed() { return std::string(kSkewedBytes, sizeof kSkewedBytes - 1); }

bool names_ok(const std::vector<TestFile>& files, std::string* error = nullptr) {
    std::vector<zip::Entry> entries;
    std::string e;
    bool ok = zip::read_directory(make_zip(files), entries, e);
    if (error) *error = e;
    return ok;
}

} // namespace

TEST(zip_crc32_known_values) {
    CHECK_EQ(zip::crc32(""), 0u);
    CHECK_EQ(zip::crc32("123456789"), 0xCBF43926u);
    CHECK_EQ(zip::crc32(dynamic_text()), 0xd3c93739u);
    // Continued over pieces.
    CHECK_EQ(zip::crc32("6789", zip::crc32("12345")), 0xCBF43926u);
}

TEST(zip_inflate_block_types) {
    std::string out;
    CHECK(zip::inflate(kFixed, 26, out));
    CHECK_EQ(out, std::string("Hello, Hello, Hello, Tome!"));
    CHECK(zip::inflate(kStored, 11, out));
    CHECK_EQ(out, std::string("plain bytes"));
    std::string text = dynamic_text();
    CHECK(zip::inflate(dynamic_packed(), text.size(), out));
    CHECK(out == text);    // Codes longer than one lookup, and a stored block after a compressed one.
    std::string skewed = skewed_text();
    CHECK_EQ(zip::crc32(skewed), 0xe5dc4319u);
    CHECK(zip::inflate(skewed_packed(), skewed.size(), out));
    CHECK(out == skewed);
}

TEST(zip_inflate_overlapping_copies) {
    // A copy longer than its distance repeats the pattern: "ab", then length
    // 258 at distance 2 three times, in one fixed block.
    std::string packed;
    uint32_t acc = 0;
    int n = 0;
    auto push = [&](uint32_t value, int bits) {
        acc |= value << n;
        n += bits;
        while (n >= 8) {
            packed.push_back(static_cast<char>(acc & 0xFF));
            acc >>= 8;
            n -= 8;
        }
    };
    auto push_code = [&](uint32_t code, int bits) {  // Huffman codes go MSB first
        for (int i = bits - 1; i >= 0; --i) push(code >> i & 1, 1);
    };
    push(1, 1);  // final
    push(1, 2);  // fixed
    push_code(0x30 + 'a', 8);
    push_code(0x30 + 'b', 8);
    for (int i = 0; i < 3; ++i) {
        push_code(0xC5, 8);  // length symbol 285: 258
        push_code(1, 5);     // distance symbol 1: 2
    }
    push_code(0, 7);  // end of block
    if (n) packed.push_back(static_cast<char>(acc));
    std::string out, want;
    for (int i = 0; i < 2 + 3 * 258; ++i) want.push_back(i % 2 ? 'b' : 'a');
    CHECK(zip::inflate(packed, want.size(), out));
    CHECK(out == want);
    // One byte short: the last copy would pass the size.
    CHECK(!zip::inflate(packed, want.size() - 1, out));
}

TEST(zip_inflate_refuses_bad_streams) {
    std::string out;
    // Wrong expected size, either way.
    CHECK(!zip::inflate(kFixed, 25, out));
    CHECK(!zip::inflate(kFixed, 27, out));
    // Truncated.
    CHECK(!zip::inflate(kFixed.substr(0, 10), 26, out));
    std::string dyn = dynamic_packed();
    CHECK(!zip::inflate(dyn.substr(0, dyn.size() / 2), dynamic_text().size(), out));
    // Bytes after the final block.
    CHECK(!zip::inflate(kFixed + "x", 26, out));
    // Block type 3 is reserved.
    CHECK(!zip::inflate(std::string("\x07", 1), 0, out));
    // A stored block whose length and complement disagree.
    std::string bad = kStored;
    bad[3] = 0;
    CHECK(!zip::inflate(bad, 11, out));
    // A back-reference before the start (fixed block: literal 'a', then
    // length 3 at distance 2). Built bit by bit, LSB first.
    std::string far;
    {
        uint32_t acc = 0;
        int n = 0;
        auto push = [&](uint32_t value, int bits) {
            acc |= value << n;
            n += bits;
            while (n >= 8) {
                far.push_back(static_cast<char>(acc & 0xFF));
                acc >>= 8;
                n -= 8;
            }
        };
        auto push_code = [&](uint32_t code, int bits) {  // Huffman codes go MSB first
            for (int i = bits - 1; i >= 0; --i) push(code >> i & 1, 1);
        };
        push(1, 1);  // final
        push(1, 2);  // fixed
        push_code(0x30 + 'a', 8);
        push_code(1, 7);  // length symbol 257: 3
        push_code(1, 5);  // distance symbol 1: 2
        push_code(0, 7);  // end of block
        if (n) far.push_back(static_cast<char>(acc));
    }
    CHECK(!zip::inflate(far, 4, out));
    // A tiny stream claiming a huge size fails without reserving that size.
    CHECK(!zip::inflate(kFixed, size_t(1) << 30, out));
    // Random bytes never crash it.
    uint32_t seed = 12345;
    for (int round = 0; round < 2000; ++round) {
        std::string noise(1 + round % 64, '\0');
        for (char& c : noise) {
            seed = seed * 1103515245 + 12345;
            c = static_cast<char>(seed >> 16);
        }
        zip::inflate(noise, round % 300, out);
    }
}

TEST(zip_reads_and_extracts) {
    std::string text = dynamic_text();
    std::string archive = make_zip({
        {"tome.mod.json", "{}", ""},
        {"assets/", "", ""},
        {"assets/a.txt", "Hello, Hello, Hello, Tome!", kFixed},
        {"big.txt", text, dynamic_packed()},
    }, "a comment");
    std::vector<zip::Entry> entries;
    std::string error;
    CHECK(zip::read_directory(archive, entries, error));
    CHECK_EQ(entries.size(), size_t(4));
    if (entries.size() != 4) return;
    CHECK_EQ(entries[0].name, std::string("tome.mod.json"));
    CHECK(entries[1].directory());
    CHECK_EQ(entries[2].method, uint16_t(8));
    std::string out;
    CHECK(zip::extract(archive, entries[0], out, error));
    CHECK_EQ(out, std::string("{}"));
    CHECK(zip::extract(archive, entries[2], out, error));
    CHECK_EQ(out, std::string("Hello, Hello, Hello, Tome!"));
    CHECK(zip::extract(archive, entries[3], out, error));
    CHECK(out == text);
}

TEST(zip_extract_checks_crc_and_ranges) {
    std::string archive = make_zip({{"a.txt", "hello", ""}});
    std::vector<zip::Entry> entries;
    std::string error, out;
    CHECK(zip::read_directory(archive, entries, error));
    if (entries.size() != 1) return;
    std::string corrupt = archive;
    corrupt[30 + 5] = 'j';  // "jello"
    CHECK(!zip::extract(corrupt, entries[0], out, error));
    CHECK(error.find("CRC") != std::string::npos);
    zip::Entry moved = entries[0];
    moved.local_offset = 1;
    CHECK(!zip::extract(archive, moved, out, error));
    moved.local_offset = 0xFFFFFF00;
    CHECK(!zip::extract(archive, moved, out, error));
    zip::Entry longer = entries[0];
    longer.compressed_size = 1 << 20;
    CHECK(!zip::extract(archive, longer, out, error));
}

TEST(zip_refuses_unsafe_names) {
    CHECK(zip::safe_name("a.txt"));
    CHECK(zip::safe_name("assets/UI/x.blp"));
    CHECK(zip::safe_name("assets/"));
    CHECK(zip::safe_name(".hidden"));
    CHECK(zip::safe_name("caf\xc3\xa9.txt"));
    CHECK(!zip::safe_name(""));
    CHECK(!zip::safe_name("/"));
    CHECK(!zip::safe_name("/etc/passwd"));
    CHECK(!zip::safe_name("../x"));
    CHECK(!zip::safe_name("a/../../x"));
    CHECK(!zip::safe_name("a/.."));
    CHECK(!zip::safe_name("a\\b"));
    CHECK(!zip::safe_name("C:/x"));
    CHECK(!zip::safe_name("a:stream"));
    CHECK(!zip::safe_name("a//b"));
    CHECK(!zip::safe_name("./a"));
    CHECK(!zip::safe_name("a./b"));
    CHECK(!zip::safe_name("a "));
    CHECK(!zip::safe_name("nul.txt"));
    CHECK(!zip::safe_name("con .txt"));
    CHECK(!zip::safe_name("assets/COM1"));
    CHECK(!zip::safe_name("a\x01"));
    CHECK(!zip::safe_name("a?"));
    CHECK(!zip::safe_name("caf\xe9.txt"));  // Latin-1, not UTF-8
    CHECK(!zip::safe_name(std::string("a\0b", 3)));

    std::string error;
    CHECK(!names_ok({{"../evil.dll", "x", ""}}, &error));
    CHECK(error.find("unsafe") != std::string::npos);
}

TEST(zip_refuses_duplicates_and_conflicts) {
    CHECK(!names_ok({{"a.txt", "1", ""}, {"A.TXT", "2", ""}}));
    CHECK(!names_ok({{"a", "", ""}, {"a/", "", ""}}));
    std::string error;
    CHECK(!names_ok({{"Assets", "file", ""}, {"assets/x.txt", "x", ""}}, &error));
    CHECK(error.find("both a file and a folder") != std::string::npos);
    CHECK(names_ok({{"assets/", "", ""}, {"assets/x.txt", "x", ""}, {"Assets/y.txt", "y", ""}}));    // Beyond ASCII, as NTFS compares names.
    CHECK(!names_ok({{"\xc3\x89.txt", "1", ""}, {"\xc3\xa9.txt", "2", ""}}));    // É, é
    CHECK(!names_ok({{"\xd0\x96/", "", ""}, {"\xd0\xb6/", "", ""}}));            // Ж, ж
    CHECK(!names_ok({{"\xce\xa3", "f", ""}, {"\xcf\x83/x", "x", ""}}, &error));  // Σ file, σ folder
    CHECK(error.find("both a file and a folder") != std::string::npos);
    CHECK(!names_ok({{"\xc4\xb1", "1", ""}, {"I", "2", ""}}));  // dotless ı upper-cases to I
    // Not one file on NTFS: no normalization, no full case mapping.
    CHECK(names_ok({{"\xc3\xa9", "1", ""}, {"e\xcc\x81", "2", ""}}));  // é, e + combining acute
    CHECK(names_ok({{"\xc3\x9f", "1", ""}, {"ss", "2", ""}}));          // ß, ss
}

TEST(zip_fold_name) {
    CHECK_EQ(zip::fold_name("assets/UI/a.txt"), std::string("ASSETS/UI/A.TXT"));
    CHECK_EQ(zip::fold_name("caf\xc3\xa9"), std::string("CAF\xc3\x89"));
    CHECK_EQ(zip::fold_name("\xc4\x81\xc4\x83"), std::string("\xc4\x80\xc4\x82"));    // a stride-2 run
    CHECK_EQ(zip::fold_name("\xc3\xbf"), std::string("\xc5\xb8"));                    // ÿ -> Ÿ
    CHECK_EQ(zip::fold_name("\xc3\x97"), std::string("\xc3\x97"));                    // × has no case
    CHECK_EQ(zip::fold_name("\xf0\x90\x90\xa8"), std::string("\xf0\x90\x90\xa8"));  // outside the BMP: kept
}

TEST(zip_refuses_unsupported_and_damaged) {
    std::vector<zip::Entry> entries;
    std::string error;
    CHECK(!zip::read_directory("", entries, error));
    CHECK(!zip::read_directory("PK\x03\x04 definitely not a zip", entries, error));
    CHECK(!names_ok({{"secret.txt", "x", "", 1}}, &error));
    CHECK(error.find("encrypted") != std::string::npos);

    // An empty archive is fine.
    CHECK(zip::read_directory(make_zip({}), entries, error));
    CHECK(entries.empty());

    std::string archive = make_zip({{"a.txt", "hello", ""}});
    const size_t end = archive.size() - 22;
    const size_t central = end - (46 + 5);
    // Compression method 14 (LZMA).
    std::string lzma = archive;
    lzma[central + 10] = 14;
    CHECK(!zip::read_directory(lzma, entries, error));
    CHECK(error.find("method 14") != std::string::npos);
    // Central directory offset past the end record.
    std::string moved = archive;
    moved[end + 16] = static_cast<char>(0xFF);
    CHECK(!zip::read_directory(moved, entries, error));
    // More entries claimed than the directory holds.
    std::string more = archive;
    more[end + 8] = more[end + 10] = 2;
    CHECK(!zip::read_directory(more, entries, error));
    // A stored entry whose sizes differ.
    std::string sizes = archive;
    sizes[central + 20] = 4;
    CHECK(!zip::read_directory(sizes, entries, error));
    // Truncated anywhere.
    for (size_t n = 0; n < archive.size(); ++n) {
        if (zip::read_directory(archive.substr(0, n), entries, error) && !entries.empty()) {
            std::string out;
            CHECK(!zip::extract(archive.substr(0, n), entries[0], out, error));
        }
    }
}

TEST(zip_limits) {
    std::vector<zip::Entry> entries;
    std::string error;
    zip::Limits limits;
    limits.max_total = 8;
    CHECK(zip::read_directory(make_zip({{"a", "1234", ""}, {"b", "5678", ""}}), entries, error, limits));
    CHECK(!zip::read_directory(make_zip({{"a", "1234", ""}, {"b", "56789", ""}}), entries, error, limits));
    limits = {};
    limits.max_entries = 1;
    CHECK(!zip::read_directory(make_zip({{"a", "", ""}, {"b", "", ""}}), entries, error, limits));
}
