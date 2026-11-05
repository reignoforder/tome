# SPDX-License-Identifier: GPL-3.0-or-later
"""List a C++ class's vtables in game.dll using MSVC RTTI.

usage: python3 -I tools/re/rtti.py <game.dll> <Class> [--base CFrame] [--slots N]

game.dll keeps RTTI, so every polymorphic class (CMainMenu, CTextFrame,
CSpriteFrame, ...) can be found by name. For each vtable this prints its
address and this-offset (secondary vtables appear for multiple inheritance;
frames have one at +0xB4 for CLayoutFrame). With --base, it prints only the
slots that differ from the base class's primary vtable: the overrides.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import PE  # noqa: E402


def vtables(pe, cls):
    """[(vtable_va, this_offset)] for class `cls`."""
    name = b".?AV" + cls.encode() + b"@@\0"
    out = []
    for off in pe.find_all(name):
        type_descriptor = pe.offset_to_va(off - 8)
        for ref in pe.find_all(struct.pack("<I", type_descriptor)):
            col = pe.offset_to_va(ref - 12)  # CompleteObjectLocator: sig, offset, cdOffset, pTypeDescriptor
            if col is None or pe.section_of_va(col) != ".rdata" or pe.u32(col) != 0:
                continue
            this_offset = pe.u32(col + 4)
            for col_ref in pe.find_all(struct.pack("<I", col)):
                vt = pe.offset_to_va(col_ref)
                if vt is not None and pe.section_of_va(vt) == ".rdata":
                    out.append((vt + 4, this_offset))
    return sorted(set(out))


def slots(pe, vt, limit):
    text = pe.section(".text")
    result = []
    for i in range(limit):
        v = pe.u32(vt + i * 4)
        if v is None or not (text[1] <= v < text[1] + text[2]):
            break
        result.append(v)
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dll")
    ap.add_argument("cls")
    ap.add_argument("--base")
    ap.add_argument("--slots", type=int, default=128)
    args = ap.parse_args()
    pe = PE(args.dll)

    base = []
    if args.base:
        primary = [v for v, off in vtables(pe, args.base) if off == 0]
        if not primary:
            print(f"base class {args.base} not found")
            return 1
        base = slots(pe, primary[0], args.slots)

    found = vtables(pe, args.cls)
    if not found:
        print(f"class {args.cls} not found")
        return 1
    for vt, this_offset in found:
        entries = slots(pe, vt, args.slots)
        print(f"{args.cls}: vtable {vt:#x} (rva {pe.rva(vt):#x}), this-offset {this_offset:#x}, {len(entries)} slots")
        for i, v in enumerate(entries):
            if args.base and i < len(base) and base[i] == v:
                continue
            note = " (new)" if args.base and i >= len(base) else ""
            print(f"   slot {i:3d} (+{i * 4:#05x})  {v:#x}  rva {pe.rva(v):#x}{note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
