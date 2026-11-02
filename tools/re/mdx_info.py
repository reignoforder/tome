# SPDX-License-Identifier: GPL-3.0-or-later
"""Print a Warcraft III model's (MDX) animation sequences and textures.

usage: python3 -I tools/re/mdx_info.py <model.mdx> [...]

Extract the model first, e.g.:
  wine build/tools/mpq/tome-mpq.exe extract input/War3x.mpq \\
      'UI\\Glues\\SpriteLayers\\Expansion\\TopLeftPanel-Expansion.mdx' panel.mdx

Sequence names are what SetAnimation's tag sets select, e.g. the tags
{stand, options, alternate} pick "Options Stand Alternate".
"""
import struct
import sys


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    for path in sys.argv[1:]:
        d = open(path, "rb").read()
        if d[:4] != b"MDLX":
            print(f"{path}: not an MDX file")
            continue
        print(f"== {path}")
        i = 4
        while i + 8 <= len(d):
            tag = d[i:i + 4]
            size = struct.unpack_from("<I", d, i + 4)[0]
            if tag == b"SEQS":
                for k in range(size // 132):
                    o = i + 8 + k * 132
                    name = d[o:o + 80].split(b"\0")[0].decode("latin-1")
                    start, end = struct.unpack_from("<II", d, o + 80)
                    print(f"   seq {k:2d}  {name:40s} {start}-{end} ({end - start} ms)")
            elif tag == b"TEXS":
                for k in range(size // 268):
                    o = i + 8 + k * 268
                    print(f"   tex      {d[o + 4:o + 264].split(b'\0')[0].decode('latin-1')}")
            i += 8 + size
    return 0


if __name__ == "__main__":
    sys.exit(main())
