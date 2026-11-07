// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace tome {

// True if all `size` bytes at `address` are committed memory that can be
// read (not no-access or guard pages) and lie in one region.
bool readable(uintptr_t address, size_t size);

// Redirects a `call rel32` (E8) instruction at `call_address` to `new_target`.
// The current target must be `expected_target`, or nothing is changed: a
// mismatch means the mapping is wrong for this binary (or another tool
// patched it first), and guessing would crash the game.
bool patch_call(uintptr_t call_address, uintptr_t expected_target, const void* new_target, std::string& error);

// Replaces a pointer-sized value (e.g. a vtable slot) if it currently holds
// `expected`. Same safety rule as patch_call.
bool patch_pointer(uintptr_t address, uintptr_t expected, const void* new_value, std::string& error);

// Overwrites `n` bytes of code at `address` with `replacement`, but only if
// they currently equal `expected` (same safety rule as patch_call: a mismatch
// means the mapping is wrong for this binary, so nothing is changed). Makes
// the page writable around the write and flushes the instruction cache.
bool patch_bytes(uintptr_t address, const uint8_t* expected, const uint8_t* replacement, size_t n,
                 std::string& error);

// Remembers the `n` bytes at `address` as they are, before Tome writes over
// them (the patch_* functions above and the hook manager call it; the first
// call for a byte wins). Mods' mapping files are checked against the game's
// own code, not Tome's patches (mod_mappings::verify).
void note_original(uintptr_t address, size_t n);

// Every byte note_original kept, by address.
std::map<uintptr_t, uint8_t> original_bytes();

} // namespace tome
