// SPDX-License-Identifier: GPL-3.0-or-later
#include "verify.h"

#include "text.h"

#include <cstdio>
#include <cstring>

namespace tome::mappings {
namespace {

std::string hex(uint32_t value, int digits = 8) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%0*x", digits, static_cast<unsigned>(value));
    return buf;
}

// The image, with the bytes before Tome's patches where it has them.
struct Image {
    const uint8_t* data;
    size_t size;
    const std::map<uint32_t, uint8_t>* originals;

    // `offset` < size.
    uint8_t at(size_t offset) const {
        if (originals) {
            auto it = originals->find(static_cast<uint32_t>(offset));
            if (it != originals->end()) return it->second;
        }
        return data[offset];
    }
};

std::string where(const Symbol& sym) {
    return "\"" + sym.name + "\" at " + hex(sym.value);
}

void check_function(const Symbol& sym, const Image& image, std::vector<std::string>& errors) {
    if (sym.value > image.size || sym.bytes.size() > image.size - sym.value) {
        errors.push_back(where(sym) + ": outside the loaded image");
        return;
    }
    for (size_t i = 0; i < sym.bytes.size(); ++i) {
        const uint8_t actual = image.at(sym.value + i);
        if (sym.bytes[i] >= 0 && actual != sym.bytes[i]) {
            errors.push_back(where(sym) + ": byte " + std::to_string(i) + " is " + hex(actual, 2) + ", expected " +
                             hex(static_cast<uint32_t>(sym.bytes[i]), 2));
            return;
        }
    }
}

void check_call(const Symbol& sym, const Build& build, const Image& image, std::vector<std::string>& errors) {
    if (sym.value > image.size || image.size - sym.value < 5) {
        errors.push_back(where(sym) + ": outside the loaded image");
        return;
    }
    const uint8_t op = image.at(sym.value);
    if (op != 0xE8) {
        errors.push_back(where(sym) + ": not a call (byte " + hex(op, 2) + ", expected 0xe8)");
        return;
    }
    uint32_t bits = 0;
    for (int i = 3; i >= 0; --i) bits = bits << 8 | image.at(sym.value + 1 + i);
    const int32_t rel = static_cast<int32_t>(bits);
    const uint32_t actual = sym.value + 5 + static_cast<uint32_t>(rel);
    const Symbol* target = build.find(sym.target);
    if (target && actual != target->value) {
        errors.push_back(where(sym) + ": calls " + hex(actual) + ", expected \"" + target->name + "\" at " +
                         hex(target->value));
    }
}

bool read_u32(const Image& image, uint32_t offset, uint32_t& out) {
    if (offset > image.size || image.size - offset < 4) return false;
    out = 0;
    for (int i = 3; i >= 0; --i) out = out << 8 | image.at(offset + i);
    return true;
}

// A NUL-terminated string at `offset`, or "" if it runs off the image.
std::string read_cstr(const Image& image, uint32_t offset) {
    std::string out;
    for (uint32_t i = offset; i < image.size && image.at(i); ++i) {
        if (out.size() > 255) return "";
        out += static_cast<char>(image.at(i));
    }
    return out;
}

// Finds the slot in the image's import directory and checks that it imports
// sym.target ("module!function"). The names come from the import lookup
// table, which the loader leaves alone (it overwrites the slots themselves).
void check_import(const Symbol& sym, const Image& image, std::vector<std::string>& errors) {
    uint32_t nt = 0, dir = 0;
    if (!read_u32(image, 0x3C, nt) || !read_u32(image, nt + 0x18 + 104, dir) || dir == 0) {
        errors.push_back(where(sym) + ": no import directory");
        return;
    }
    const size_t bang = sym.target.find('!');
    const std::string module = sym.target.substr(0, bang), function = sym.target.substr(bang + 1);
    for (uint32_t desc = dir;; desc += 20) {
        uint32_t lookup = 0, name = 0, slots = 0;
        if (!read_u32(image, desc, lookup) || !read_u32(image, desc + 12, name) ||
            !read_u32(image, desc + 16, slots)) {
            break;
        }
        if (name == 0) break;
        if (lookup == 0) lookup = slots;  // no separate lookup table: the slots hold the names until loaded
        for (uint32_t i = 0;; ++i) {
            uint32_t entry = 0;
            if (!read_u32(image, lookup + i * 4, entry) || entry == 0) break;
            if (slots + i * 4 != sym.value) continue;
            const std::string actual_module = read_cstr(image, name);
            const std::string actual = entry & 0x80000000u ? "#" + std::to_string(entry & 0xFFFF)
                                                           : read_cstr(image, entry + 2);
            if (!text::iequals(actual_module, module) || actual != function) {
                errors.push_back(where(sym) + ": imports " + actual_module + "!" + actual + ", expected " +
                                 sym.target);
            }
            return;
        }
    }
    errors.push_back(where(sym) + ": not an import slot");
}

} // namespace

std::vector<std::string> verify_code(const Build& build, const uint8_t* data, size_t size,
                                     const std::map<uint32_t, uint8_t>* originals) {
    std::vector<std::string> errors;
    const Image image{data, size, originals};
    for (const Symbol& sym : build.symbols) {
        if (sym.kind == Kind::Function) check_function(sym, image, errors);
        if (sym.kind == Kind::Call) check_call(sym, build, image, errors);
        if (sym.kind == Kind::Import) check_import(sym, image, errors);
    }
    return errors;
}

} // namespace tome::mappings
