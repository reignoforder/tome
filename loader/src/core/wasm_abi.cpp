// SPDX-License-Identifier: GPL-3.0-or-later
#include "wasm_abi.h"

#include <tome/utf8.h>

#include <cstring>

namespace tome::wasm {
namespace {

constexpr uint16_t kGenerationMask = (1u << (32 - HandleTable::kIndexBits)) - 1;

} // namespace

bool in_bounds(const Memory& memory, uint32_t ptr, uint32_t len) {
    if (!memory.base) return len == 0 && ptr == 0;
    // 64-bit arithmetic: ptr + len can't wrap.
    return uint64_t(ptr) + uint64_t(len) <= memory.size;
}

const char* describe(StringError error) {
    switch (error) {
    case StringError::None: return "";
    case StringError::OutOfBounds: return "outside the module's memory";
    case StringError::TooLong: return "too long";
    case StringError::InvalidUtf8: return "not valid UTF-8";
    case StringError::Nul: return "contains a NUL byte";
    }
    return "";
}

StringError read_string(const Memory& memory, uint32_t ptr, uint32_t len, size_t max, std::string& out) {
    out.clear();
    if (len > max) return StringError::TooLong;
    if (!in_bounds(memory, ptr, len)) return StringError::OutOfBounds;
    if (len == 0) return StringError::None;
    // Copy first, then check the copy: the module can't change what was checked.
    out.assign(reinterpret_cast<const char*>(memory.base + ptr), len);
    for (size_t i = 0; i < out.size();) {
        const unsigned char c = static_cast<unsigned char>(out[i]);
        if (c == 0) {
            out.clear();
            return StringError::Nul;
        }
        uint32_t cp = 0;
        const size_t n = tome_utf8_decode(out.data() + i, out.size() - i, &cp);
        // U+FFFD is also what an invalid sequence decodes to: only EF BF BD is the real one.
        if (cp == TOME_UTF8_REPLACEMENT && !(n == 3 && out.compare(i, 3, "\xEF\xBF\xBD") == 0)) {
            out.clear();
            return StringError::InvalidUtf8;
        }
        i += n;
    }
    return StringError::None;
}

bool read_bytes(const Memory& memory, uint32_t ptr, uint32_t len, size_t max, std::vector<uint8_t>& out) {
    out.clear();
    if (len > max || !in_bounds(memory, ptr, len)) return false;
    if (len) out.assign(memory.base + ptr, memory.base + ptr + len);
    return true;
}

uint32_t write_out(const Memory& memory, uint32_t ptr, uint32_t capacity, std::string_view data) {
    if (data.size() >= UINT32_MAX) return UINT32_MAX;
    if (!in_bounds(memory, ptr, capacity)) return UINT32_MAX;
    const size_t n = data.size() < capacity ? data.size() : capacity;
    if (n) std::memcpy(memory.base + ptr, data.data(), n);
    return static_cast<uint32_t>(data.size());
}

bool store_u32(const Memory& memory, uint32_t ptr, uint32_t value) {
    if (!in_bounds(memory, ptr, 4)) return false;
    const uint8_t bytes[4] = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24)};
    std::memcpy(memory.base + ptr, bytes, 4);
    return true;
}

bool load_u32(const Memory& memory, uint32_t ptr, uint32_t& value) {
    if (!in_bounds(memory, ptr, 4)) return false;
    const uint8_t* p = memory.base + ptr;
    value = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    return true;
}

bool has_start_section(const uint8_t* bytes, size_t size) {
    static const uint8_t kHeader[8] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    if (!bytes || size < sizeof(kHeader) || std::memcmp(bytes, kHeader, sizeof(kHeader)) != 0) return false;
    size_t at = sizeof(kHeader);
    while (at < size) {
        const uint8_t id = bytes[at++];
        // The section's size: a LEB128 u32 (at most 5 bytes).
        uint64_t length = 0;
        bool done = false;
        for (int shift = 0; shift < 35 && at < size; shift += 7) {
            const uint8_t byte = bytes[at++];
            length |= uint64_t(byte & 0x7f) << shift;
            if (!(byte & 0x80)) {
                done = true;
                break;
            }
        }
        if (!done || length > size - at) return false;
        if (id == 8) return true;
        at += static_cast<size_t>(length);
    }
    return false;
}

bool valid_file_name(std::string_view name) {
    if (name.empty() || name.size() > 64 || name[0] == '.') return false;
    for (char c : name) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '_' || c == '-';
        if (!ok) return false;
    }
    if (name.back() == '.') return false;  // Windows drops it: "a." is "a"
    // CON, PRN, AUX, NUL, COM1-9, LPT1-9, also with an extension.
    std::string stem(name.substr(0, name.find('.')));
    for (char& c : stem) c = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") return false;
    if (stem.size() == 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0) && stem[3] >= '0' &&
        stem[3] <= '9') {
        return false;
    }
    return true;
}

uint32_t HandleTable::add(HandleKind kind, void* object) {
    if (!object) return 0;
    auto known = index_of_.find({object, kind});
    if (known != index_of_.end()) return make(known->second, slots_[known->second].generation);
    uint32_t index;
    if (!free_.empty()) {
        index = free_.back();
        free_.pop_back();
    } else {
        if (slots_.size() > kMaxHandles) return 0;
        index = static_cast<uint32_t>(slots_.size());
        slots_.emplace_back();
    }
    Slot& s = slots_[index];
    s.object = object;
    s.kind = kind;
    s.used = true;
    ++live_;
    index_of_[{object, kind}] = index;
    return make(index, s.generation);
}

void* HandleTable::get(uint32_t handle, HandleKind kind) const {
    const uint32_t index = handle & kMaxHandles;
    const uint16_t generation = static_cast<uint16_t>(handle >> kIndexBits);
    if (index == 0 || index >= slots_.size()) return nullptr;
    const Slot& s = slots_[index];
    if (!s.used || s.generation != generation || s.kind != kind) return nullptr;
    return s.object;
}

void HandleTable::remove(uint32_t handle) {
    const uint32_t index = handle & kMaxHandles;
    const uint16_t generation = static_cast<uint16_t>(handle >> kIndexBits);
    if (index == 0 || index >= slots_.size()) return;
    Slot& s = slots_[index];
    if (!s.used || s.generation != generation) return;
    index_of_.erase({s.object, s.kind});
    s.used = false;
    s.object = nullptr;
    // Never generation 0, so no handle is ever 0. A slot whose generations
    // ran out is retired rather than reused.
    s.generation = static_cast<uint16_t>((s.generation + 1) & kGenerationMask);
    --live_;
    if (s.generation == 0) return;
    free_.push_back(index);
}

void HandleTable::remove_all(HandleKind kind) {
    for (uint32_t i = 1; i < slots_.size(); ++i) {
        if (slots_[i].used && slots_[i].kind == kind) remove(make(i, slots_[i].generation));
    }
}

} // namespace tome::wasm
