#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Packs a zip that's the same bytes for the same files, wherever and whenever
it's made (the release zips: cmake/dist.cmake, sdk/cmake/dist.cmake).

  repro_zip.py <out.zip> <folder> <name>...

Each name (a file or folder in <folder>) goes in with everything under it:
sorted, one fixed time, the usual permissions (0644, folders 0755), no extra
fields (CMake's zip writer stores each file's access and change times, which
nothing can set), deflated at level 9. Static and import libraries (.a) get
their members' times, owners and modes zeroed as `ar D` would (ld writes the
link time into an import library's members).
"""
import os
import sys
import zipfile

TIME = (2000, 1, 1, 0, 0, 0)


def deterministic_ar(data):
    """An ar archive with every member's time, uid and gid 0 and mode 644."""
    if not data.startswith(b"!<arch>\n"):
        return data
    out = bytearray(data)
    at = 8
    while at + 60 <= len(out):
        header = out[at:at + 60]
        if header[58:60] != b"`\n":
            return data  # not an archive after all: leave it
        size = int(header[48:58].decode("ascii").strip() or "0")
        out[at + 16:at + 48] = b"0".ljust(12) + b"0".ljust(6) + b"0".ljust(6) + b"644".ljust(8)
        at += 60 + size + (size & 1)
    return bytes(out)


def entries(folder, name):
    path = os.path.join(folder, name)
    if not os.path.isdir(path):
        return [name]
    out = [name + "/"]
    for root, dirs, files in os.walk(path):
        rel = os.path.relpath(root, folder).replace(os.sep, "/")
        out += [rel + "/" + d + "/" for d in dirs]
        out += [rel + "/" + f for f in files]
    return out


def main(argv):
    if len(argv) < 4:
        sys.stderr.write(__doc__)
        return 2
    out, folder, names = argv[1], argv[2], argv[3:]
    paths = sorted(set(p for n in names for p in entries(folder, n.rstrip("/"))))
    tmp = out + ".tmp"
    with zipfile.ZipFile(tmp, "w") as z:
        for name in paths:
            info = zipfile.ZipInfo(name, TIME)
            info.create_system = 3  # Unix, for the permissions
            if name.endswith("/"):
                info.external_attr = (0o40755 << 16) | 0x10
                z.writestr(info, b"")
            else:
                info.external_attr = 0o100644 << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                with open(os.path.join(folder, name), "rb") as f:
                    data = f.read()
                if name.endswith(".a"):
                    data = deterministic_ar(data)
                z.writestr(info, data, compresslevel=9)
    os.replace(tmp, out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
