// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Just enough of an x86-32 decoder to move a function's first instructions
// somewhere else: the stolen bytes of an inline hook (core/hooks.h,
// loader/src/hooks.cpp). Opcodes it doesn't know are refused rather than
// guessed, so a hook fails cleanly instead of corrupting the game's code.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tome::x86 {

enum class Branch {
    None,
    Call32,  // E8 rel32
    Jmp32,   // E9 rel32
    Jcc32,   // 0F 80+cc rel32
    Jmp8,    // EB rel8
    Jcc8,    // 70+cc rel8
    Loop8,   // E0-E3: loopne, loope, loop, jecxz (rel8 only)
};

struct Insn {
    size_t length = 0;
    bool falls_through = true;  // control can reach the next instruction
    Branch branch = Branch::None;
    int32_t displacement = 0;   // relative branches: target = end of the instruction + displacement
};

// Decodes the instruction at `code` (at most `available` bytes readable).
// Returns false for opcodes this decoder doesn't know or that run past
// `available`; `error` says which.
bool decode(const uint8_t* code, size_t available, Insn& out, std::string& error);

// Copies whole instructions from `code`, which runs at address `from`, until
// at least `min_length` source bytes are taken, so the copy works at address
// `to`. Relative branches keep their targets: short ones are widened to rel32
// (so the copy can be longer than the source), and targets inside the taken
// bytes move with the copy. If the function returns or jumps away before
// `min_length`, the bytes up to it must be padding (int3 or nop). `consumed`
// is the number of source bytes taken. Fails on unknown opcodes and on
// branches into the middle of a taken instruction.
bool relocate(const uint8_t* code, size_t available, uint32_t from, uint32_t to, size_t min_length,
              std::vector<uint8_t>& out, size_t& consumed, std::string& error);

} // namespace tome::x86
