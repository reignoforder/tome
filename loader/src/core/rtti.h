// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Reads MSVC RTTI (32-bit layout) from game.dll's memory, so Tome can check an
// object's class before calling a method that only exists on some classes
// (CTextFrame::SetText on a frame that isn't a text frame would corrupt
// memory). game.dll keeps RTTI for every polymorphic class.
//
//   vtable[-1]                 -> CompleteObjectLocator {sig, offset, cd_offset, type, hierarchy}
//   hierarchy                  -> {sig, attributes, base_count, bases}
//   bases[i]                   -> BaseClassDescriptor {type, ...}; bases[0] is the class itself
//   type                       -> TypeDescriptor {vtable, spare, ".?AVName@@"}

#include <cstdint>
#include <string>
#include <string_view>

namespace tome::rtti {

// Reads `size` bytes at `address`; returns false if the range isn't readable.
// The game-side reader only allows addresses inside game.dll's image.
struct Memory {
    bool (*read)(const void* ctx, uint32_t address, void* out, uint32_t size);
    const void* ctx;
};

// The class name of an object with this primary vtable, e.g. "CTextFrame",
// or "" if the vtable has no readable RTTI.
std::string class_name(const Memory& memory, uint32_t vtable);

// True if the class with this primary vtable is `name` or derives from it.
bool derives_from(const Memory& memory, uint32_t vtable, std::string_view name);

} // namespace tome::rtti
