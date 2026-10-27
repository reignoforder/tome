#pragma once

#include <cstdint>
#include <string>

namespace tome {

// Redirects a `call rel32` (E8) instruction at `call_address` to `new_target`.
// The current target must be `expected_target`, or nothing is changed: a
// mismatch means the mapping is wrong for this binary (or another tool
// patched it first), and guessing would crash the game.
bool patch_call(uintptr_t call_address, uintptr_t expected_target, const void* new_target, std::string& error);

// Replaces a pointer-sized value (e.g. a vtable slot) if it currently holds
// `expected`. Same safety rule as patch_call.
bool patch_pointer(uintptr_t address, uintptr_t expected, const void* new_value, std::string& error);

} // namespace tome
