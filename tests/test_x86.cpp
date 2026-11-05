// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/embedded_mappings.h"
#include "core/mappings.h"
#include "core/x86.h"
#include "test.h"

#include <cstring>

using namespace tome;

namespace {

size_t length_of(std::vector<uint8_t> code) {
    x86::Insn insn;
    std::string error;
    code.resize(code.size() + 16, 0xCC);  // room past the instruction, as in real code
    return x86::decode(code.data(), code.size(), insn, error) ? insn.length : 0;
}

uint32_t read32(const std::vector<uint8_t>& v, size_t at) {
    uint32_t value;
    std::memcpy(&value, &v[at], 4);
    return value;
}

} // namespace

TEST(x86_decodes_common_prologue_instructions) {
    CHECK_EQ(length_of({0x55}), size_t(1));                                   // push ebp
    CHECK_EQ(length_of({0x8b, 0xec}), size_t(2));                             // mov ebp, esp
    CHECK_EQ(length_of({0x81, 0xec, 0x0c, 0x01, 0x00, 0x00}), size_t(6));    // sub esp, imm32
    CHECK_EQ(length_of({0x83, 0xec, 0x10}), size_t(3));                       // sub esp, imm8
    CHECK_EQ(length_of({0x6a, 0xff}), size_t(2));                             // push -1
    CHECK_EQ(length_of({0x68, 1, 2, 3, 4}), size_t(5));                       // push imm32
    CHECK_EQ(length_of({0x64, 0xa1, 0, 0, 0, 0}), size_t(6));                 // mov eax, fs:[0]
    CHECK_EQ(length_of({0x8b, 0x45, 0x08}), size_t(3));                       // mov eax, [ebp+8]
    CHECK_EQ(length_of({0x8b, 0x44, 0x24, 0x04}), size_t(4));                 // mov eax, [esp+4]
    CHECK_EQ(length_of({0x8b, 0x04, 0x25, 1, 2, 3, 4}), size_t(7));           // mov eax, [disp32] via SIB
    CHECK_EQ(length_of({0x8b, 0x05, 1, 2, 3, 4}), size_t(6));                 // mov eax, [disp32]
    CHECK_EQ(length_of({0x8b, 0x84, 0x24, 1, 2, 3, 4}), size_t(7));           // mov eax, [esp+disp32]
    CHECK_EQ(length_of({0x83, 0xb9, 0x74, 0x01, 0x00, 0x00, 0x00}), size_t(7));  // cmp [ecx+0x174], 0
    CHECK_EQ(length_of({0xc7, 0x45, 0xfc, 1, 2, 3, 4}), size_t(7));           // mov [ebp-4], imm32
    CHECK_EQ(length_of({0x66, 0xc7, 0x45, 0xfc, 1, 2}), size_t(6));           // mov word [ebp-4], imm16
    CHECK_EQ(length_of({0xf6, 0x41, 0x08, 0x01}), size_t(4));                 // test byte [ecx+8], 1
    CHECK_EQ(length_of({0xf7, 0xd8}), size_t(2));                             // neg eax
    CHECK_EQ(length_of({0xf7, 0xc1, 1, 2, 3, 4}), size_t(6));                 // test ecx, imm32
    CHECK_EQ(length_of({0x0f, 0xb6, 0x41, 0x08}), size_t(4));                 // movzx eax, byte [ecx+8]
    CHECK_EQ(length_of({0xf3, 0x0f, 0x10, 0x45, 0x08}), size_t(5));           // movss xmm0, [ebp+8]
    CHECK_EQ(length_of({0xd9, 0x45, 0x08}), size_t(3));                       // fld dword [ebp+8]
    CHECK_EQ(length_of({0xc8, 0x10, 0x00, 0x00}), size_t(4));                 // enter 16, 0
    CHECK_EQ(length_of({0xe8, 1, 2, 3, 4}), size_t(5));                       // call rel32
    CHECK_EQ(length_of({0x0f, 0x84, 1, 2, 3, 4}), size_t(6));                 // jz rel32
    CHECK_EQ(length_of({0xff, 0x15, 1, 2, 3, 4}), size_t(6));                 // call [imm32]
}

TEST(x86_refuses_unknown_and_truncated) {
    x86::Insn insn;
    std::string error;
    const uint8_t undefined[] = {0x0f, 0x04, 0x00, 0x00};
    CHECK(!x86::decode(undefined, sizeof(undefined), insn, error));
    CHECK(error.find("unknown opcode") != std::string::npos);
    const uint8_t amd_3dnow[] = {0x0f, 0x0f, 0xc0, 0x9e};
    CHECK(!x86::decode(amd_3dnow, sizeof(amd_3dnow), insn, error));
    const uint8_t truncated[] = {0x81, 0xec, 0x0c};
    CHECK(!x86::decode(truncated, sizeof(truncated), insn, error));
    CHECK(error.find("past the checked bytes") != std::string::npos);
    CHECK(!x86::decode(truncated, 0, insn, error));
    const uint8_t short_jcxz[] = {0x67, 0xe3, 0x00};  // jcxz: 16-bit
    CHECK(!x86::decode(short_jcxz, sizeof(short_jcxz), insn, error));
}

TEST(x86_decodes_newer_and_rarer_encodings) {
    CHECK_EQ(length_of({0x66, 0x0f, 0x38, 0x00, 0xc1}), size_t(5));        // pshufb xmm0, xmm1
    CHECK_EQ(length_of({0x66, 0x0f, 0x3a, 0x0f, 0xc1, 0x08}), size_t(6));  // palignr xmm0, xmm1, 8
    CHECK_EQ(length_of({0x66, 0x0f, 0x3a, 0x16, 0x44, 0x24, 0x04, 0x01}), size_t(8));  // pextrd [esp+4], xmm0, 1
    CHECK_EQ(length_of({0xc5, 0xf8, 0x77}), size_t(3));                    // vzeroupper
    CHECK_EQ(length_of({0xc5, 0xf9, 0x6f, 0x45, 0x08}), size_t(5));        // vmovdqa xmm0, [ebp+8]
    CHECK_EQ(length_of({0xc4, 0xe3, 0x79, 0x0f, 0xc1, 0x08}), size_t(6));  // vpalignr xmm0, xmm0, xmm1, 8
    CHECK_EQ(length_of({0xc4, 0xe2, 0x79, 0x00, 0xc1}), size_t(5));        // vpshufb
    CHECK_EQ(length_of({0xc5, 0x45, 0x08}), size_t(3));                    // lds eax, [ebp+8]: not VEX
    CHECK_EQ(length_of({0x67, 0x8b, 0x00}), size_t(3));                    // mov eax, [bx+si]
    CHECK_EQ(length_of({0x67, 0x8b, 0x06, 0x34, 0x12}), size_t(5));        // mov eax, [0x1234]
    CHECK_EQ(length_of({0x67, 0x8b, 0x80, 0x34, 0x12}), size_t(5));        // mov eax, [bx+si+0x1234]
    CHECK_EQ(length_of({0x67, 0xa1, 0x34, 0x12}), size_t(4));              // mov eax, [moffs16]
    CHECK_EQ(length_of({0x9a, 1, 2, 3, 4, 5, 6}), size_t(7));              // call far ptr16:32
    CHECK_EQ(length_of({0x0f, 0xae, 0xf0}), size_t(3));                    // mfence
    CHECK_EQ(length_of({0xf3, 0x0f, 0xb8, 0xc1}), size_t(4));              // popcnt eax, ecx
}

TEST(x86_relocates_rel32_branches) {
    // call +0x100 ; jz +0x20 ; nop
    const uint8_t code[] = {0xe8, 0x00, 0x01, 0x00, 0x00, 0x0f, 0x84, 0x20, 0x00, 0x00, 0x00, 0x90};
    std::vector<uint8_t> out;
    size_t consumed = 0;
    std::string error;
    const uint32_t from = 0x10001000, to = 0x20000000;
    CHECK(x86::relocate(code, sizeof(code), from, to, 5, out, consumed, error));
    CHECK_EQ(consumed, size_t(5));
    CHECK_EQ(out.size(), size_t(5));
    CHECK_EQ(to + 5 + read32(out, 1), from + 5 + 0x100u);  // still calls the same function

    CHECK(x86::relocate(code, sizeof(code), from, to, 6, out, consumed, error));
    CHECK_EQ(consumed, size_t(11));
    CHECK_EQ(out[5], uint8_t(0x0f));
    CHECK_EQ(to + 11 + read32(out, 7), from + 11 + 0x20u);
}

TEST(x86_widens_short_branches) {
    std::vector<uint8_t> out;
    size_t consumed = 0;
    std::string error;
    const uint32_t from = 0x1000, to = 0x7000;

    // test eax, eax; jz +4 (to +8, past the taken bytes); nop...
    const uint8_t short_jz[] = {0x85, 0xc0, 0x74, 0x04, 0x90, 0x90, 0x90, 0x90, 0xc3};
    CHECK(x86::relocate(short_jz, sizeof(short_jz), from, to, 5, out, consumed, error));
    CHECK_EQ(consumed, size_t(5));
    CHECK_EQ(out.size(), size_t(2 + 6 + 1));
    CHECK_EQ(out[2], uint8_t(0x0f));
    CHECK_EQ(out[3], uint8_t(0x84));
    CHECK_EQ(to + 8 + read32(out, 4), from + 8);

    // jmp short +3 (to +5): a function that starts with a jump.
    const uint8_t short_jmp[] = {0xeb, 0x03, 0xcc, 0xcc, 0xcc, 0x33, 0xc0, 0xc3};
    CHECK(x86::relocate(short_jmp, sizeof(short_jmp), from, to, 5, out, consumed, error));
    CHECK_EQ(consumed, size_t(5));  // the int3s after the jmp are padding
    CHECK_EQ(out[0], uint8_t(0xe9));
    CHECK_EQ(to + 5 + read32(out, 1), from + 5);

    // jecxz +1; nop; nop; nop: becomes jecxz +2; jmp short +5; jmp rel32.
    const uint8_t jecxz[] = {0xe3, 0x01, 0x90, 0x90, 0x90, 0x90};
    CHECK(x86::relocate(jecxz, sizeof(jecxz), from, to, 5, out, consumed, error));
    CHECK_EQ(consumed, size_t(5));
    CHECK_EQ(out.size(), size_t(9 + 3));
    CHECK(out[0] == 0xe3 && out[1] == 0x02 && out[2] == 0xeb && out[3] == 0x05 && out[4] == 0xe9);
    // Its target, +3, is one of the taken nops: it moves with the copy.
    CHECK_EQ(to + 9 + read32(out, 5), to + 10);
}

TEST(x86_branches_inside_the_taken_bytes) {
    std::vector<uint8_t> out;
    size_t consumed = 0;
    std::string error;
    const uint32_t from = 0x1000, to = 0x7000;
    // loop: dec ecx; jnz loop (back to +0): stays in the copy.
    const uint8_t loop[] = {0x49, 0x75, 0xfd, 0x90, 0x90, 0xc3};
    CHECK(x86::relocate(loop, sizeof(loop), from, to, 5, out, consumed, error));
    CHECK_EQ(out[1], uint8_t(0x0f));
    CHECK_EQ(to + 7 + read32(out, 3), to + 0);

    // A branch into the middle of a taken instruction can't be moved.
    const uint8_t middle[] = {0x74, 0x01, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3};  // jz +1, into the mov
    CHECK(!x86::relocate(middle, sizeof(middle), from, to, 5, out, consumed, error));
    CHECK(error.find("middle of an instruction") != std::string::npos);

    // A later branch back into the overwritten bytes (not their start) would
    // land inside the hook's jmp: xor eax, eax; inc eax (+2); ...; jnz +2.
    const uint8_t loop_back[] = {0x31, 0xc0, 0x40, 0x90, 0x90, 0x75, 0xfb, 0xc3};
    CHECK(!x86::relocate(loop_back, sizeof(loop_back), from, to, 5, out, consumed, error));
    CHECK(error.find("inside the bytes the hook overwrites") != std::string::npos);
}

TEST(x86_refuses_what_cant_move) {
    std::vector<uint8_t> out;
    size_t consumed = 0;
    std::string error;
    // xor eax, eax; ret; then another function: the jmp would overwrite it.
    const uint8_t tiny[] = {0x33, 0xc0, 0xc3, 0x55, 0x8b};
    CHECK(!x86::relocate(tiny, sizeof(tiny), 0x1000, 0x2000, 5, out, consumed, error));
    CHECK(error.find("ends at +2") != std::string::npos);

    // ... unless what follows is padding.
    const uint8_t padded[] = {0x33, 0xc0, 0xc3, 0xcc, 0xcc};
    CHECK(x86::relocate(padded, sizeof(padded), 0x1000, 0x2000, 5, out, consumed, error));
    CHECK_EQ(consumed, size_t(5));

    const uint8_t thunk[] = {0xe9, 0x10, 0, 0, 0, 0x90};  // jmp elsewhere
    CHECK(x86::relocate(thunk, sizeof(thunk), 0x1000, 0x2000, 5, out, consumed, error));
    CHECK_EQ(0x2000u + 5 + read32(out, 1), 0x1000u + 5 + 0x10);

    const uint8_t enough[] = {0x8b, 0x41, 0x08, 0x85, 0xc0, 0x74, 0x04};  // stops before the jz
    CHECK(x86::relocate(enough, sizeof(enough), 0x1000, 0x2000, 5, out, consumed, error));
    CHECK_EQ(consumed, size_t(5));
}

// Every mapped function can be hooked: its first instructions decode and
// move within the "bytes" the startup check covers. Wildcards are
// relocated immediates, so any value stands in for them.
TEST(x86_every_mapped_function_relocates) {
    for (size_t i = 0; i < mappings::kEmbeddedFileCount; ++i) {
        json::Value root;
        if (!json::parse(mappings::kEmbeddedFiles[i].text, root).ok) continue;
        mappings::Build b;
        if (!mappings::parse_build(root, b).ok) continue;
        for (const mappings::Symbol& sym : b.symbols) {
            if (sym.kind != mappings::Kind::Function) continue;
            std::vector<uint8_t> code;
            for (int16_t v : sym.bytes) code.push_back(v < 0 ? 0 : uint8_t(v));
            std::vector<uint8_t> out;
            size_t consumed = 0;
            std::string error;
            if (!x86::relocate(code.data(), code.size(), sym.value, 0x7f000000, 5, out, consumed, error)) {
                test::fail(__FILE__, __LINE__, b.name + " " + sym.name + ": " + error);
            }
        }
    }
}
