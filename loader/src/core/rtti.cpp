// SPDX-License-Identifier: GPL-3.0-or-later
#include "rtti.h"

namespace tome::rtti {
namespace {

constexpr uint32_t kMaxBases = 64;      // far more than any game class has
constexpr uint32_t kMaxNameLength = 256;

bool read32(const Memory& memory, uint32_t address, uint32_t& out) {
    return memory.read(memory.ctx, address, &out, sizeof(out));
}

// The decorated name stored in a TypeDescriptor, e.g. ".?AVCTextFrame@@".
bool type_name(const Memory& memory, uint32_t type, std::string& out) {
    out.clear();
    for (uint32_t i = 0; i < kMaxNameLength; ++i) {
        char c = 0;
        if (!memory.read(memory.ctx, type + 8 + i, &c, 1)) return false;
        if (c == '\0') return true;
        out += c;
    }
    return false;
}

std::string undecorate(const std::string& decorated) {
    if (decorated.size() < 6 || decorated.compare(0, 4, ".?AV") != 0 ||
        decorated.compare(decorated.size() - 2, 2, "@@") != 0) {
        return "";
    }
    return decorated.substr(4, decorated.size() - 6);
}

bool hierarchy(const Memory& memory, uint32_t vtable, uint32_t& type, uint32_t& bases, uint32_t& count) {
    uint32_t locator = 0, hier = 0;
    if (!read32(memory, vtable - 4, locator) || !read32(memory, locator + 12, type) ||
        !read32(memory, locator + 16, hier) || !read32(memory, hier + 8, count) ||
        !read32(memory, hier + 12, bases)) {
        return false;
    }
    return count > 0 && count <= kMaxBases;
}

} // namespace

std::string class_name(const Memory& memory, uint32_t vtable) {
    uint32_t type = 0, bases = 0, count = 0;
    std::string decorated;
    if (!hierarchy(memory, vtable, type, bases, count) || !type_name(memory, type, decorated)) return "";
    return undecorate(decorated);
}

bool derives_from(const Memory& memory, uint32_t vtable, std::string_view name) {
    uint32_t type = 0, bases = 0, count = 0;
    if (!hierarchy(memory, vtable, type, bases, count)) return false;
    std::string decorated;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t base = 0, base_type = 0;
        if (!read32(memory, bases + i * 4, base) || !read32(memory, base, base_type) ||
            !type_name(memory, base_type, decorated)) {
            return false;
        }
        if (undecorate(decorated) == name) return true;
    }
    return false;
}

} // namespace tome::rtti
