"""Minimal PE32 reader shared by the reverse-engineering helpers.

Reads section headers from the file itself, so the helpers work on any build
of game.dll (no hard-coded section addresses).
"""
import struct


class PE:
    def __init__(self, path):
        self.path = path
        self.data = open(path, "rb").read()
        d = self.data
        if d[:2] != b"MZ":
            raise ValueError(f"{path}: not a PE file")
        nt = struct.unpack_from("<I", d, 0x3C)[0]
        if d[nt:nt + 4] != b"PE\0\0":
            raise ValueError(f"{path}: bad PE signature")
        machine, nsections = struct.unpack_from("<HH", d, nt + 4)
        if machine != 0x14C:
            raise ValueError(f"{path}: not a 32-bit x86 image")
        self.timestamp = struct.unpack_from("<I", d, nt + 8)[0]
        opt_size = struct.unpack_from("<H", d, nt + 20)[0]
        opt = nt + 24
        self.image_base = struct.unpack_from("<I", d, opt + 28)[0]
        self.size_of_image = struct.unpack_from("<I", d, opt + 56)[0]
        self.sections = []  # (name, va, vsize, raw_offset, raw_size)
        sec = opt + opt_size
        for i in range(nsections):
            name = d[sec:sec + 8].rstrip(b"\0").decode()
            vsize, rva, raw_size, raw_off = struct.unpack_from("<IIII", d, sec + 8)
            self.sections.append((name, self.image_base + rva, max(vsize, raw_size), raw_off, raw_size))
            sec += 40

    def section(self, name):
        for s in self.sections:
            if s[0] == name:
                return s
        raise KeyError(name)

    def va_to_offset(self, va):
        for name, sva, size, raw, raw_size in self.sections:
            if sva <= va < sva + raw_size:
                return va - sva + raw
        return None

    def offset_to_va(self, off):
        for name, sva, size, raw, raw_size in self.sections:
            if raw <= off < raw + raw_size:
                return off - raw + sva
        return None

    def section_of_va(self, va):
        for name, sva, size, raw, raw_size in self.sections:
            if sva <= va < sva + size:
                return name
        return None

    def u32(self, va):
        off = self.va_to_offset(va)
        return None if off is None else struct.unpack_from("<I", self.data, off)[0]

    def cstr(self, va, limit=256):
        off = self.va_to_offset(va)
        if off is None:
            return None
        end = self.data.find(b"\0", off, off + limit)
        if end < 0:
            return None
        return self.data[off:end].decode("latin-1")

    def rva(self, va):
        return va - self.image_base

    def find_all(self, needle):
        """File offsets of every occurrence of `needle` (bytes)."""
        out, i = [], self.data.find(needle)
        while i != -1:
            out.append(i)
            i = self.data.find(needle, i + 1)
        return out
