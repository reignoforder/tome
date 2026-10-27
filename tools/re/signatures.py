"""Generate or check the "bytes" signatures in a mapping file.

usage: python3 -I tools/re/signatures.py <game.dll> <mappings/build.json> [--write]

For every function symbol, prints the pattern Tome checks at startup: the
first bytes of the function, at least MIN_LEN and extended (up to MAX_LEN)
until the pattern occurs only once in its section. Bytes the loader rewrites
when it relocates game.dll (ASLR) are "??". Each existing "bytes" field is
compared with the generated one; --write adds or replaces them in the file
(a text edit that keeps the file's formatting).
"""
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import PE  # noqa: E402

MIN_LEN = 16
MAX_LEN = 64


def relocated_rvas(pe):
    """RVAs of every byte covered by a HIGHLOW base relocation."""
    d = pe.data
    nt = struct.unpack_from("<I", d, 0x3C)[0]
    opt = nt + 24
    reloc_rva, reloc_size = struct.unpack_from("<II", d, opt + 96 + 5 * 8)
    out = set()
    pos = pe.va_to_offset(pe.image_base + reloc_rva)
    end = pos + reloc_size
    while pos < end:
        page, size = struct.unpack_from("<II", d, pos)
        if size < 8:
            break
        for i in range(8, size, 2):
            entry = struct.unpack_from("<H", d, pos + i)[0]
            if entry >> 12 == 3:  # IMAGE_REL_BASED_HIGHLOW
                rva = page + (entry & 0xFFF)
                out.update(range(rva, rva + 4))
        pos += size
    return out


def matches(data, off, pattern):
    for i, b in enumerate(pattern):
        if b is not None and data[off + i] != b:
            return False
    return True


def count_in_section(pe, rva, pattern):
    """How many times `pattern` (bytes or None) occurs in rva's section."""
    name = pe.section_of_va(pe.image_base + rva)
    _, sva, _, raw, raw_size = pe.section(name)
    data = pe.data[raw:raw + raw_size]
    anchor = bytes(pattern[:next((i for i, b in enumerate(pattern) if b is None), len(pattern))])
    count, i = 0, data.find(anchor)
    while i != -1:
        if i + len(pattern) <= len(data) and matches(data, i, pattern):
            count += 1
            if count > 1:
                break
        i = data.find(anchor, i + 1)
    return count


def signature(pe, relocs, rva):
    off = pe.va_to_offset(pe.image_base + rva)
    pattern = []
    for i in range(MAX_LEN):
        pattern.append(None if rva + i in relocs else pe.data[off + i])
        if len(pattern) >= MIN_LEN and count_in_section(pe, rva, pattern) == 1:
            break
    return pattern


def fmt(pattern):
    return " ".join("??" if b is None else f"{b:02x}" for b in pattern)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 2:
        print(__doc__)
        return 2
    write = "--write" in sys.argv
    pe = PE(args[0])
    path = args[1]
    text = open(path, encoding="utf-8").read()
    symbols = json.loads(text)["symbols"]
    relocs = relocated_rvas(pe)

    status = 0
    for name, sym in symbols.items():
        if sym["kind"] != "function":
            continue
        rva = int(sym["rva"], 16) if isinstance(sym["rva"], str) else sym["rva"]
        pattern = signature(pe, relocs, rva)
        sig = fmt(pattern)
        unique = count_in_section(pe, rva, pattern) == 1
        old = sym.get("bytes")
        state = "ok" if old == sig else ("new" if old is None else "CHANGED")
        if old is not None and old != sig:
            status = 1
        note = "" if unique else "  (not unique)"
        print(f"{state:7} {name:28} {sig}{note}")
        if write and old != sig:
            text, n = replace_bytes(text, name, sig)
            if n != 1:
                print(f"        could not place \"bytes\" for {name}; edit it by hand")
                status = 1
    if write:
        open(path, "w", encoding="utf-8").write(text)
    return status


def replace_bytes(text, name, sig):
    """Sets "bytes" in the entry for `name`: replaces it, or adds it after "sig"."""
    start = text.find(json.dumps(name) + ":")
    if start < 0:
        return text, 0
    end = text.find("}", start)
    entry = text[start:end]
    if '"bytes"' in entry:
        new = re.sub(r'"bytes": "[^"]*"', f'"bytes": "{sig}"', entry, count=1)
    else:
        m = re.search(r'("sig": "(?:[^"\\]|\\.)*")', entry)
        if not m:
            return text, 0
        # On its own line, indented like the line holding "sig".
        line_start = entry.rfind("\n", 0, m.start()) + 1
        indent = re.match(r"[ \t]*", entry[line_start:]).group(0)
        sep = ",\n" + indent
        new = entry[:m.end()] + sep + f'"bytes": "{sig}"' + entry[m.end():]
    return text[:start] + new + text[end:], 1


if __name__ == "__main__":
    sys.exit(main())
