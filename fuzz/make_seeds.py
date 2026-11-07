#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes the fuzz targets' seed inputs: make_seeds.py <out dir>.

Creates <out>/<target>/ for every target in fuzz/. Inputs that once crashed
a target are checked in under fuzz/regressions/<target>/ and copied too.
"""
import glob
import io
import os
import shutil
import sys
import zipfile
import struct
import subprocess
import tempfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
out = sys.argv[1]

seeds = {}


def add(target, data):
    if isinstance(data, str):
        data = data.encode()
    seeds.setdefault(target, []).append(data)


# Lobby chat lines and game cache keys (the first byte of a line picks the sender).
for text in [
    "[Tome] Required mods 1/1: hello 1.0.0 #1a2b3c4d, other 2.0.0 #00ff00ff",
    "[Tome] Required mods 1/2: hello 1.0.0 #1a2b3c4d\n[Tome] Required mods 2/2: other 2.0.0 #00ff00ff",
    "[Tome] My mods 1/1: none",
    "A[Tome] My mods 1/1: hello 1.0.0 #1a2b3c4d\nB[Tome] Required mods 1/1: none",
    "hello 1.0.0 #1a2b3c4d\nother 2.0.0 #00ff00ff",
    "[Tome] Required mods 1/1: hello 1.2.3-beta.1+build #deadbeef",
]:
    add("handshake", text)

# Mods' messages: "<sender><value as 4 bytes><mission>\t<key>" per action.
def netmsg_line(sender, part, count, mission, key):
    return bytes([sender]) + struct.pack("<i", part | count << 8) + mission.encode() + b"\t" + key.encode()
add("netmsg", netmsg_line(1, 0, 1, "1 1 hello", "aGVsbG8="))
add("netmsg", netmsg_line(1, 0, 2, "1 2 my-mod", "A" * 252) + b"\n" + netmsg_line(1, 1, 2, "1 2 my-mod", "AAAA"))
add("netmsg", netmsg_line(2, 0, 1, "1 3 hello", ""))

for text in ["/tome list", "/tome enable hello", "/tome disable my-mod", "/tome reload", "/tome logs warn 20", "/Hello  some  args ",
             "/x\targ", "/tome help"]:
    add("chat", text)

# tome.log text, for /tome logs (the same target).
add("chat", "ed line\r\n[12:00:01.000] [  100] [INFO ] Tome starting\r\n"
            "[12:00:03.000] [ 2048] [WARN ] [hello] a | pipe\r\n"
            "[12:00:04.000] [ 2048] [ERROR] crashed\r\n  called from x.dll+0x10\r\n")


def zip_bytes(files, method):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", method) as z:
        for name, data in files:
            z.writestr(name, data)
    return buf.getvalue()


mod = [("tome.mod.json", '{"schema": 1, "id": "x", "version": "1.0.0"}'),
       ("assets/", ""), ("assets/a.txt", "hello hello hello hello"), ("x.dll", b"MZ" + bytes(64))]
add("zip", zip_bytes(mod, zipfile.ZIP_STORED))
add("zip", zip_bytes(mod, zipfile.ZIP_DEFLATED))
add("zip", zip_bytes([("tome.mod.json", "{}")], zipfile.ZIP_DEFLATED))

for data in [b"", b"a", b"hello hello hello hello hello", bytes(range(256)) * 4, bytes((i * 131 + 7) % 256 for i in range(300))]:
    for level in (0, 1, 9):
        c = zlib.compressobj(level, zlib.DEFLATED, -15)
        add("inflate", len(data).to_bytes(2, "little") + c.compress(data) + c.flush())

for path in sorted(glob.glob(os.path.join(ROOT, "sdk/examples/*/*.json")) +
                   glob.glob(os.path.join(ROOT, "tests/e2e/mods/*/tome.mod.json"))):
    add("json", open(path, "rb").read())
add("json", '{"a": [1, 2.5e3, -0, true, false, null, "\\u00e9\\n"], "b": {}}')
# A mod's own mapping file (tome.mod.json "mappings").
add("json", '{"schema": 2, "name": "1.27b", "symbols": {'
            '"CUnit::Foo": {"kind": "function", "rva": "0x100", "conv": "thiscall", "sig": "int(void* this)", '
            '"bytes": "55 8b ec ?? 83"}, "Foo.size": {"kind": "offset", "value": 8}, '
            '"Foo.call": {"kind": "call", "rva": "0x200", "target": "CUnit::Foo"}}}')

add("jass", "globals\nunit G\nendglobals\nfunction f takes nothing returns nothing\n"
            "    local integer G\n    set G = 1\nendfunction\n")
add("jass", "integer bj_x\nfunction f takes nothing returns nothing\n  local unit bj_x // c\n"
            "  call BJDebugMsg(\"local real G\")\nendfunction\n")
add("jass", "native HelloLog takes integer value returns nothing")
add("jass", "constant native F takes code c, real r, unit u returns boolean")

for text in ["Maps\\a.w3m", "..\\x", "C:\\x", "x::$DATA", "UI\\FrameDef\\a.toc", "a\\\\b", "con.txt",
             "UI\\FrameDef\\Glue\\x.fdf\r\nUI\\a.fdf\r\n", "ui/mod.fdf"]:
    add("paths", text)

def blp(compression=0, w=8, h=8, has_mips=0, jpeg_header=0, mip_off=0x9C + 4, mip_size=16, total=None):
    b = bytearray(0x9C + 4)
    b[0:4] = b"BLP1"
    struct.pack_into("<I", b, 0x04, compression)
    struct.pack_into("<I", b, 0x0C, w)
    struct.pack_into("<I", b, 0x10, h)
    struct.pack_into("<I", b, 0x18, has_mips)
    struct.pack_into("<I", b, 0x1C, mip_off)
    struct.pack_into("<I", b, 0x5C, mip_size)
    struct.pack_into("<I", b, 0x9C, jpeg_header)
    if total is None:
        total = max(len(b), mip_off + mip_size if mip_size and mip_off < 1 << 24 else len(b))
    b += bytes(max(0, total - len(b)))
    return bytes(b[:total]) if total < len(b) else bytes(b)


add("blp", blp())                                   # valid JPEG, one mip
add("blp", blp(compression=1, total=0x9C + 0x400 + 8))  # valid paletted
add("blp", blp(mip_size=0x7fffffff, total=0xA4))    # mip past EOF
add("blp", blp(jpeg_header=0x7fffffff, total=0xA4)) # JPEG header past EOF
add("blp", b"BLP1" + bytes(8))                       # too short for the header

# tome.sig files (RFC 8032's first test key and signature) and base64.
KEY = "11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo="
SIG = "5VZDAMNgrHKQhuLMgG6CioSHfx645dl02HPgZSJJAVVfuIIVkKM7rMYeOXAc+bRr0lv18FlbviRlUUFDjnoQCw=="
add("signature", '{"schema": 1, "key": "%s", "signature": "%s"}' % (KEY, SIG))
add("signature", '{"schema": 2, "key": "%s", "signature": "%s"}' % (KEY, SIG))
add("signature", '{"schema": 1, "key": "AAAA", "signature": ""}')
add("signature", KEY)
add("signature", "QQ==")

# The update check: GitHub's latest-release reply and tome/update.json.
add("update", '{"tag_name": "v0.2.0", "name": "Tome 0.2.0", "draft": false, "prerelease": false, '
              '"html_url": "https://github.com/reignoforder/tome/releases/tag/v0.2.0"}')
add("update", '{"tag_name": "v1.0.0-rc.1", "prerelease": true}')
add("update", '{"last_check": 1793750400, "latest": "0.2.0", "tag": "v0.2.0", "notified": "0.1.0"}')
add("update", "a" * 64 + "  tome-0.2.0.zip\n" + "b" * 64 + "  tome-sdk-0.2.0.zip\n\0"
              '{"schema": 1, "key": "11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo=", "signature": '
              '"IJMzA9ZjeAREI7UyI1cmUdEWpUqMUGNMAjQqZOhi9fEnv6uLgnrTf6/w+b91fLCFwsNHFhbn+6jPv5TJU3xxBg=="}')
add("update", "# release keys\n11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo=\n")

# A native's signature, then argument words and strings.
add("wasm_layout", b"(Hunit;RS)Hunit;\n" + bytes([5, 0, 16, 0]) + b"abcde" + bytes(8) + b"xyz")
add("wasm_layout", b"(ISC)V\n" + bytes(12))
add("wasm_layout", b"()C")

# WebAssembly modules (fuzz_wamr): small ones made here for what Tome's
# configuration turns on (loops for the budget, memory growth, tables and
# reference types, bulk memory, recursion, an import), the e2e test's mod
# and the SDK's built examples.
def uleb(v):
    b = bytearray()
    while True:
        x = v & 0x7F
        v >>= 7
        b.append(x | (0x80 if v else 0))
        if not v:
            return bytes(b)


def wvec(items):
    return uleb(len(items)) + b"".join(items)


def wname(t):
    return uleb(len(t)) + t.encode()


def wsection(i, body):
    return bytes([i]) + uleb(len(body)) + body


def wbody(code, locals_=b"\x00"):
    body = locals_ + code + b"\x0b"
    return uleb(len(body)) + body


def wmodule(types, funcs, exports, imports=(), memory=None, table=None, elems=(), datacount=None, data=()):
    m = b"\x00asm\x01\x00\x00\x00" + wsection(1, wvec(types))
    if imports:
        m += wsection(2, wvec(list(imports)))
    m += wsection(3, wvec([uleb(t) for t, _ in funcs]))
    if table is not None:
        m += wsection(4, wvec([table]))
    if memory is not None:
        m += wsection(5, wvec([memory]))
    m += wsection(7, wvec([wname(n) + bytes([k]) + uleb(i) for n, k, i in exports]))
    if elems:
        m += wsection(9, wvec(list(elems)))
    if datacount is not None:
        m += wsection(12, uleb(datacount))
    m += wsection(10, wvec([b for _, b in funcs]))
    if data:
        m += wsection(11, wvec(list(data)))
    return m


VOID, RET_I32 = b"\x60\x00\x00", b"\x60\x00\x01\x7f"
LOAD = ("tome_mod_load", 0, 0)
add("wamr", b"\x00asm\x01\x00\x00\x00")
add("wamr", wmodule([RET_I32], [(0, wbody(b"\x03\x40\x0c\x00\x0b\x41\x00"))], [LOAD]))  # loop forever
add("wamr", wmodule([RET_I32], [(0, wbody(b"\x10\x00"))], [LOAD]))  # recursion
add("wamr", wmodule([RET_I32], [(0, wbody(b"\x41\x01\x40\x00\x1a\x41\xf0\xa2\x04\x41\x05\x36\x02\x00\x41\x00"))],
                    [LOAD, ("memory", 2, 0)], memory=b"\x01\x01\x02"))  # grow, store past the first page
add("wamr", wmodule([RET_I32, b"\x60\x01\x7f\x01\x7f"],
                    [(0, wbody(b"\x41\x07\x41\x01\x11\x01\x00")), (1, wbody(b"\x20\x00"))],
                    [LOAD], table=b"\x70\x00\x02",
                    elems=[b"\x00\x41\x01\x0b" + wvec([uleb(1)])]))  # call_indirect
add("wamr", wmodule([RET_I32], [(0, wbody(b"\xd0\x70\xd1\x1a\xd2\x00\xd1"))], [LOAD],
                    elems=[b"\x03\x00" + wvec([uleb(0)])]))  # ref.null, ref.is_null, ref.func (declared)
add("wamr", wmodule([RET_I32], [(0, wbody(b"\x41\x00\x41\x2a\x41\x10\xfc\x0b\x00\x41\x20\x41\x00\x41\x10"
                                         b"\xfc\x0a\x00\x00\x41\x00\x41\x00\x41\x04\xfc\x08\x00\x00\xfc\x09\x00\x41\x00"))],
                    [LOAD, ("memory", 2, 0)], memory=b"\x00\x01", datacount=1,
                    data=[b"\x01" + wname("tome")]))  # memory.fill, memory.copy, memory.init, data.drop
add("wamr", wmodule([b"\x60\x03\x7f\x7f\x7f\x00", RET_I32, b"\x60\x02\x7e\x7c\x01\x7d"],
                    [(1, wbody(b"\x41\x01\x41\x00\x41\x04\x10\x00\x41\x00")), (2, wbody(b"\x43\x00\x00\x80\x3f"))],
                    [LOAD, ("memory", 2, 0), ("f", 0, 2)],
                    imports=[wname("tome") + wname("log") + b"\x00" + uleb(0)], memory=b"\x00\x01",
                    data=[b"\x00\x41\x00\x0b" + wname("seed")]))  # an import; i64 and f64 parameters
with tempfile.TemporaryDirectory() as tmp:
    subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tests/e2e/make_wasm_mod.py"), tmp], check=True)
    with open(os.path.join(tmp, "mod.wasm"), "rb") as f:
        add("wamr", f.read())
for path in sorted(glob.glob(os.path.join(ROOT, "build/mods/*/*.wasm"))):
    with open(path, "rb") as f:
        add("wamr", f.read())

for target in [os.path.basename(f)[5:-4] for f in glob.glob(os.path.join(ROOT, "fuzz/fuzz_*.cpp"))]:
    d = os.path.join(out, target)
    os.makedirs(d, exist_ok=True)
    for i, data in enumerate(seeds.get(target, [])):
        with open(os.path.join(d, "seed-%02d" % i), "wb") as f:
            f.write(data)
    for path in glob.glob(os.path.join(ROOT, "fuzz/regressions", target, "*")):
        shutil.copy(path, d)
