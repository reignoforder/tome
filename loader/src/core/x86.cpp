#include "x86.h"

#include <cstdio>
#include <cstring>

namespace tome::x86 {
namespace {

// Immediate operand sizes. Z is 4 bytes, or 2 with an operand-size prefix.
enum Imm : uint8_t { I0, I8, I16, IZ, I24 /* enter: 16 + 8 */, IA /* moffs: address size */, IP /* far ptr: 6 */ };

struct Op {
    bool known;
    bool modrm;
    Imm imm;
    bool falls_through;
    Branch branch;
};

constexpr Op kBad = {false, false, I0, true, Branch::None};
constexpr Op op(bool modrm, Imm imm = I0) {
    return {true, modrm, imm, true, Branch::None};
}
constexpr Op end(bool modrm, Imm imm = I0) {
    return {true, modrm, imm, false, Branch::None};
}
constexpr Op branch(Branch b, Imm imm) {
    return {true, false, imm, b != Branch::Jmp32 && b != Branch::Jmp8, b};
}

// One-byte opcodes (prefixes and 0x0F are handled before the table).
Op one_byte(uint8_t b) {
    if (b < 0x40) {
        switch (b & 7) {
        case 0: case 1: case 2: case 3: return op(true);  // ALU r/m, r and r, r/m
        case 4: return op(false, I8);                      // ALU al, imm8
        case 5: return op(false, IZ);                      // ALU eax, imm
        default:
            // push/pop es/cs/ss/ds and daa/das/aaa/aas; segment prefixes and
            // 0x0F never get here.
            return op(false);
        }
    }
    if (b < 0x62) return op(false);  // inc/dec/push/pop reg, pusha, popa
    switch (b) {
    case 0x62: case 0x63: return op(true);       // bound, arpl
    case 0x68: return op(false, IZ);             // push imm
    case 0x69: return op(true, IZ);              // imul r, r/m, imm
    case 0x6A: return op(false, I8);             // push imm8
    case 0x6B: return op(true, I8);              // imul r, r/m, imm8
    case 0x6C: case 0x6D: case 0x6E: case 0x6F: return op(false);  // ins/outs
    case 0x80: case 0x82: case 0x83: return op(true, I8);
    case 0x81: return op(true, IZ);
    case 0x9A: return op(false, IP);             // call far ptr16:32 (absolute)
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: return op(false, IA);  // mov moffs
    case 0xA8: return op(false, I8);             // test al, imm8
    case 0xA9: return op(false, IZ);             // test eax, imm
    case 0xC0: case 0xC1: return op(true, I8);   // shifts by imm8
    case 0xC2: return end(false, I16);           // ret imm16
    case 0xC3: return end(false);                // ret
    case 0xC4: case 0xC5: return op(true);       // les, lds (VEX is handled in decode)
    case 0xC6: return op(true, I8);              // mov r/m8, imm8
    case 0xC7: return op(true, IZ);              // mov r/m, imm
    case 0xC8: return op(false, I24);            // enter
    case 0xC9: return op(false);                 // leave
    case 0xCA: return end(false, I16);           // retf imm16
    case 0xCB: case 0xCC: case 0xCF: return end(false);  // retf, int3, iret
    case 0xCD: return op(false, I8);             // int imm8
    case 0xCE: return op(false);                 // into
    case 0xD0: case 0xD1: case 0xD2: case 0xD3: return op(true);  // shifts
    case 0xD4: case 0xD5: return op(false, I8);  // aam, aad
    case 0xD6: case 0xD7: return op(false);      // salc, xlat
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: return branch(Branch::Loop8, I8);
    case 0xE4: case 0xE5: case 0xE6: case 0xE7: return op(false, I8);  // in/out imm8
    case 0xE8: return branch(Branch::Call32, IZ);
    case 0xE9: return branch(Branch::Jmp32, IZ);
    case 0xEA: return end(false, IP);            // jmp far ptr16:32
    case 0xEB: return branch(Branch::Jmp8, I8);
    case 0xEC: case 0xED: case 0xEE: case 0xEF: return op(false);  // in/out dx
    case 0xF1: case 0xF5: return op(false);      // icebp, cmc
    case 0xF4: return end(false);                // hlt
    case 0xF6: case 0xF7: return op(true);       // test has an immediate; see decode
    case 0xF8: case 0xF9: case 0xFA: case 0xFB: case 0xFC: case 0xFD: return op(false);
    case 0xFE: case 0xFF: return op(true);       // inc/dec/call/jmp/push r/m; see decode
    default: break;
    }
    if (b >= 0x70 && b <= 0x7F) return branch(Branch::Jcc8, I8);
    if (b >= 0x84 && b <= 0x8F) return op(true);   // test, xchg, mov, lea, pop r/m
    if (b >= 0x90 && b <= 0x99) return op(false);  // nop/xchg, cwde, cdq
    if (b >= 0x9B && b <= 0x9F) return op(false);  // wait, pushf, popf, sahf, lahf
    if (b >= 0xA4 && b <= 0xAF) return op(false);  // string instructions
    if (b >= 0xB0 && b <= 0xB7) return op(false, I8);
    if (b >= 0xB8 && b <= 0xBF) return op(false, IZ);
    if (b >= 0xD8 && b <= 0xDF) return op(true);   // x87
    return kBad;
}

// Two-byte opcodes (after 0x0F), also VEX map 1.
Op two_byte(uint8_t b) {
    if (b >= 0x80 && b <= 0x8F) return branch(Branch::Jcc32, IZ);
    if (b <= 0x03) return op(true);                // sldt.., lgdt.., lar, lsl
    if (b >= 0x10 && b <= 0x17) return op(true);   // SSE moves
    if (b >= 0x18 && b <= 0x1F) return op(true);   // prefetch, hint nops
    if (b >= 0x20 && b <= 0x23) return op(true);   // mov cr/dr
    if (b >= 0x28 && b <= 0x2F) return op(true);   // SSE moves, conversions, compares
    if (b >= 0x30 && b <= 0x35) return op(false);  // wrmsr, rdtsc, rdmsr, rdpmc, sysenter, sysexit
    if (b >= 0x40 && b <= 0x4F) return op(true);   // cmovcc
    if (b >= 0x50 && b <= 0x6F) return op(true);   // SSE/MMX arithmetic
    if (b >= 0x70 && b <= 0x73) return op(true, I8);  // pshuf*, shifts by imm8
    if (b >= 0x74 && b <= 0x76) return op(true);   // pcmpeq*
    if (b >= 0x78 && b <= 0x79) return op(true);   // vmread, vmwrite
    if (b >= 0x7C && b <= 0x7F) return op(true);   // haddps, hsubps, movd/movq
    if (b >= 0x90 && b <= 0x9F) return op(true);   // setcc
    if (b >= 0xC8 && b <= 0xCF) return op(false);  // bswap
    if (b >= 0xD0) return op(true);                // SSE/MMX arithmetic
    switch (b) {
    case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: return op(false);  // syscall, clts, sysret, invd, wbinvd
    case 0x0B: return end(false);                  // ud2
    case 0x0D: return op(true);                    // prefetchw
    case 0x77: return op(false);                   // emms
    case 0xA0: case 0xA1: case 0xA2: case 0xA8: case 0xA9: case 0xAA: return op(false);  // push/pop fs/gs, cpuid, rsm
    case 0xA3: case 0xAB: case 0xB3: case 0xBB: return op(true);  // bt, bts, btr, btc
    case 0xA4: case 0xAC: return op(true, I8);     // shld/shrd imm8
    case 0xA5: case 0xAD: case 0xAE: case 0xAF: return op(true);  // shld/shrd cl, fxsave.., imul
    case 0xB0: case 0xB1: case 0xB2: case 0xB4: case 0xB5: return op(true);  // cmpxchg, lss, lfs, lgs
    case 0xB6: case 0xB7: case 0xB8: case 0xB9: return op(true);  // movzx, popcnt, ud1
    case 0xBA: return op(true, I8);                // bt* imm8
    case 0xBC: case 0xBD: case 0xBE: case 0xBF: return op(true);  // bsf, bsr, movsx
    case 0xC0: case 0xC1: case 0xC3: case 0xC7: return op(true);  // xadd, movnti, cmpxchg8b
    case 0xC2: case 0xC4: case 0xC5: case 0xC6: return op(true, I8);  // cmpps, pinsrw, pextrw, shufps
    default: return kBad;
    }
}

bool fail(std::string& error, const char* what, const uint8_t* code, size_t offset) {
    char buf[96];
    snprintf(buf, sizeof(buf), "%s (byte 0x%02x at +%zu)", what, code[offset], offset);
    error = buf;
    return false;
}

bool truncated(std::string& error) {
    error = "instruction runs past the checked bytes";
    return false;
}

} // namespace

bool decode(const uint8_t* code, size_t available, Insn& out, std::string& error) {
    size_t i = 0;
    bool operand16 = false;
    bool address16 = false;
    // Legacy prefixes: lock, rep, segment overrides, operand and address size.
    for (;; ++i) {
        if (i >= available) return truncated(error);
        uint8_t b = code[i];
        if (b == 0x66) operand16 = true;
        else if (b == 0x67) address16 = true;
        else if (b == 0xF0 || b == 0xF2 || b == 0xF3 || b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E ||
                 b == 0x64 || b == 0x65) {
        } else break;
        if (i >= 14) return fail(error, "too many prefixes", code, i);
    }

    const size_t opcode_at = i;
    Op o;
    int map = 0;  // 0: one-byte, 1: 0F, 2: 0F 38, 3: 0F 3A
    // VEX (AVX): C5/C4 are lds/les unless the next byte has both top bits set.
    if ((code[i] == 0xC4 || code[i] == 0xC5) && i + 1 < available && (code[i + 1] & 0xC0) == 0xC0) {
        if (code[i] == 0xC5) {
            map = 1;
            i += 2;
        } else {
            if (i + 2 >= available) return truncated(error);
            map = code[i + 1] & 0x1F;
            if (map < 1 || map > 3) return fail(error, "unknown VEX map", code, i + 1);
            i += 3;
        }
        if (i >= available) return truncated(error);
        o = map == 1 ? two_byte(code[i]) : op(true, map == 3 ? I8 : I0);
        if (map == 1 && o.branch != Branch::None) return fail(error, "invalid VEX opcode", code, i);
    } else if (code[i] == 0x0F) {
        if (++i >= available) return truncated(error);
        if (code[i] == 0x38 || code[i] == 0x3A) {
            // Three-byte opcodes (SSSE3, SSE4): all take ModRM; 0F 3A also imm8.
            map = code[i] == 0x38 ? 2 : 3;
            if (++i >= available) return truncated(error);
            o = op(true, map == 3 ? I8 : I0);
        } else {
            map = 1;
            o = two_byte(code[i]);
        }
    } else {
        o = one_byte(code[i]);
    }
    if (!o.known) return fail(error, "unknown opcode", code, i);
    const uint8_t opcode = code[i++];

    if (o.modrm) {
        if (i >= available) return truncated(error);
        const uint8_t modrm = code[i++];
        const uint8_t mod = modrm >> 6, reg = (modrm >> 3) & 7, rm = modrm & 7;
        if (address16) {
            if (mod == 0 && rm == 6) i += 2;
            else if (mod == 1) i += 1;
            else if (mod == 2) i += 2;
        } else {
            if (mod != 3 && rm == 4) {
                if (i >= available) return truncated(error);
                const uint8_t sib = code[i++];
                if (mod == 0 && (sib & 7) == 5) i += 4;
            }
            if (mod == 0 && rm == 5) i += 4;
            else if (mod == 1) i += 1;
            else if (mod == 2) i += 4;
        }

        if (map == 0) {
            if ((opcode == 0xF6 || opcode == 0xF7) && reg <= 1) o.imm = opcode == 0xF6 ? I8 : IZ;  // test r/m, imm
            if (opcode == 0xFF && (reg == 4 || reg == 5)) o.falls_through = false;  // jmp r/m, jmp far
            if ((opcode == 0xFE && reg >= 2) || (opcode == 0xFF && reg == 7)) {
                return fail(error, "invalid instruction", code, opcode_at);
            }
        }
    }

    const size_t imm_at = i;
    switch (o.imm) {
    case I0: break;
    case I8: i += 1; break;
    case I16: i += 2; break;
    case IZ: i += operand16 ? 2 : 4; break;
    case I24: i += 3; break;
    case IA: i += address16 ? 2 : 4; break;
    case IP: i += operand16 ? 4 : 6; break;
    }
    if (i > available) return truncated(error);
    if (o.branch != Branch::None && (operand16 || address16)) {
        return fail(error, "16-bit branches aren't supported", code, opcode_at);
    }

    out = Insn();
    out.length = i;
    out.falls_through = o.falls_through;
    out.branch = o.branch;
    if (o.branch != Branch::None) {
        if (o.imm == I8) {
            out.displacement = static_cast<int8_t>(code[imm_at]);
        } else {
            std::memcpy(&out.displacement, code + imm_at, 4);
        }
    }
    return true;
}

namespace {

struct Taken {
    size_t at;      // offset in the source
    Insn insn;
    size_t new_at;  // offset in the copy
    size_t new_length;
};

size_t widened_length(const Insn& insn) {
    switch (insn.branch) {
    case Branch::Jmp8: return 5;   // E9 rel32
    case Branch::Jcc8: return 6;   // 0F 8x rel32
    case Branch::Loop8: return 9;  // op +2; jmp short +5; jmp rel32
    default: return insn.length;
    }
}

bool is_padding(uint8_t b) {
    return b == 0xCC || b == 0x90;
}

} // namespace

bool relocate(const uint8_t* code, size_t available, uint32_t from, uint32_t to, size_t min_length,
              std::vector<uint8_t>& out, size_t& consumed, std::string& error) {
    out.clear();
    std::vector<Taken> taken;
    size_t at = 0;
    while (at < min_length) {
        Insn insn;
        std::string why;
        if (!decode(code + at, available - at, insn, why)) {
            error = "can't decode the instruction at +" + std::to_string(at) + ": " + why;
            return false;
        }
        taken.push_back({at, insn, 0, widened_length(insn)});
        at += insn.length;
        if (!insn.falls_through && at < min_length) {
            // The function ends here; what follows up to min_length has to be
            // padding nothing runs, or the jmp would overwrite another function.
            for (; at < min_length; ++at) {
                if (at >= available || !is_padding(code[at])) {
                    error = "the function ends at +" + std::to_string(taken.back().at) + ", before " +
                            std::to_string(min_length) + " bytes, and isn't followed by padding";
                    return false;
                }
            }
        }
    }
    consumed = at;

    // The rest of the checked bytes, as far as they decode: a branch back
    // into the first min_length bytes (other than the start) would land in
    // the middle of the hook's jmp. Taken bytes after those stay intact.
    for (size_t next = at; next < available;) {
        Insn insn;
        std::string why;
        if (!decode(code + next, available - next, insn, why)) break;
        if (insn.branch != Branch::None) {
            const int64_t target = int64_t(next) + int64_t(insn.length) + insn.displacement;
            if (target > 0 && target < int64_t(min_length)) {
                error = "a branch at +" + std::to_string(next) + " targets +" + std::to_string(target) +
                        ", inside the bytes the hook overwrites";
                return false;
            }
        }
        next += insn.length;
        if (!insn.falls_through) break;
    }

    size_t new_at = 0;
    for (Taken& t : taken) {
        t.new_at = new_at;
        new_at += t.new_length;
    }

    // Where a branch ends up: unchanged outside the taken bytes; inside them,
    // the copy of the instruction it targets.
    auto resolve = [&](const Taken& t, uint32_t& target_out) {
        const int64_t target = int64_t(t.at) + int64_t(t.insn.length) + t.insn.displacement;
        // A branch back to the start stays in the copy: it's part of this
        // call, not a new one, so it mustn't run the hook chain again.
        if (target < 0 || target >= int64_t(consumed)) {
            target_out = from + static_cast<uint32_t>(target);
            return true;
        }
        for (const Taken& u : taken) {
            if (int64_t(u.at) == target) {
                target_out = to + static_cast<uint32_t>(u.new_at);
                return true;
            }
        }
        // Anything else inside the taken bytes (the middle of an instruction,
        // or trailing padding) is overwritten by the hook's jmp.
        error = "a branch at +" + std::to_string(t.at) + " targets the middle of an instruction";
        return false;
    };
    auto put_rel32 = [&](uint32_t target) {
        const uint32_t rel = target - (to + static_cast<uint32_t>(out.size()) + 4);
        uint8_t bytes[4];
        std::memcpy(bytes, &rel, 4);
        out.insert(out.end(), bytes, bytes + 4);
    };

    for (const Taken& t : taken) {
        const uint8_t* src = code + t.at;
        uint32_t target = 0;
        if (t.insn.branch != Branch::None && !resolve(t, target)) return false;
        switch (t.insn.branch) {
        case Branch::None:
            out.insert(out.end(), src, src + t.insn.length);
            break;
        case Branch::Call32: case Branch::Jmp32: case Branch::Jcc32:
            // Prefixes and opcode as they were, then the new displacement.
            out.insert(out.end(), src, src + t.insn.length - 4);
            put_rel32(target);
            break;
        case Branch::Jmp8:
            out.push_back(0xE9);
            put_rel32(target);
            break;
        case Branch::Jcc8:
            out.push_back(0x0F);
            out.push_back(static_cast<uint8_t>(0x80 | (src[t.insn.length - 2] & 0x0F)));
            put_rel32(target);
            break;
        case Branch::Loop8:
            out.push_back(src[t.insn.length - 2]);  // the same loop/jecxz, to the jmp below
            out.push_back(0x02);
            out.push_back(0xEB);  // not taken: skip it
            out.push_back(0x05);
            out.push_back(0xE9);
            put_rel32(target);
            break;
        }
    }
    return true;
}

} // namespace tome::x86
