"""Find where a string is referenced in game.dll.

usage: python3 -I tools/re/xrefs.py <game.dll> <string> [<string> ...]

For each exact (NUL-terminated) string, prints its address and every place its
address appears as an immediate: in code (with the opcode byte before it, e.g.
68 = push imm32, b9/ba = mov ecx/edx, imm32) or in data (pointer tables, such
as the FDF keyword table).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import PE  # noqa: E402

OPCODES = {0x68: "push", 0xB8: "mov eax", 0xB9: "mov ecx", 0xBA: "mov edx", 0xBB: "mov ebx",
           0xBE: "mov esi", 0xBF: "mov edi"}


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    pe = PE(sys.argv[1])
    for text in sys.argv[2:]:
        hits = [o + 1 for o in pe.find_all(b"\0" + text.encode() + b"\0")]
        if not hits:
            print(f"{text!r}: not found")
            continue
        for off in hits:
            va = pe.offset_to_va(off)
            if va is None:
                continue
            refs = []
            for r in pe.find_all(struct.pack("<I", va)):
                rva = pe.offset_to_va(r)
                if rva is None:
                    continue
                sec = pe.section_of_va(rva)
                if sec == ".text":
                    op = pe.data[r - 1]
                    refs.append(f"code {rva - 1:#x} ({OPCODES.get(op, f'op {op:02x}')})")
                else:
                    refs.append(f"data {rva:#x} ({sec})")
            print(f"{text!r} at {va:#x} (rva {pe.rva(va):#x}):")
            for ref in refs or ["no references"]:
                print(f"    {ref}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
