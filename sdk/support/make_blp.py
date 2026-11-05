# SPDX-License-Identifier: MIT
"""Writes a solid-color texture in the BLP1 format the game's UI uses.

usage: python3 make_blp.py <out.blp> <size> <rrggbb>

Paletted, no alpha, with the full mipmap chain, like the game's own 32x32
glue button textures: a placeholder icon for a mod (tome.mod.json "icon").
The hello example and Tome's e2e test make theirs with this instead of
shipping a binary.
"""
import struct
import sys


def main() -> int:
    out, size, color = sys.argv[1], int(sys.argv[2]), int(sys.argv[3], 16)
    r, g, b = (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF

    mips = []
    s = size
    while True:
        mips.append(bytes(s * s))  # every pixel is palette index 0
        if s == 1:
            break
        s //= 2

    header_size = 4 + 6 * 4 + 16 * 4 + 16 * 4 + 256 * 4
    offsets, sizes, at = [], [], header_size
    for mip in mips:
        offsets.append(at)
        sizes.append(len(mip))
        at += len(mip)
    offsets += [0] * (16 - len(offsets))
    sizes += [0] * (16 - len(sizes))

    # compression 1 (paletted), 0 alpha bits, width, height, picture type 5, has mipmaps
    data = b"BLP1" + struct.pack("<6I", 1, 0, size, size, 5, 1)
    data += struct.pack("<16I", *offsets) + struct.pack("<16I", *sizes)
    data += struct.pack("<4B", b, g, r, 0) + bytes(255 * 4)  # BGRA palette
    data += b"".join(mips)
    open(out, "wb").write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
