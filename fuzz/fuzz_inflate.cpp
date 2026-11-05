// SPDX-License-Identifier: GPL-3.0-or-later
// The deflate decoder alone: the first 2 bytes give the expected size.
#include "core/zip.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 2) return 0;
    size_t expected = data[0] | (data[1] << 8);
    std::string_view in(reinterpret_cast<const char*>(data + 2), size - 2);
    std::string out;
    if (tome::zip::inflate(in, expected, out) && out.size() != expected) __builtin_trap();
    tome::zip::crc32(in);
    return 0;
}
