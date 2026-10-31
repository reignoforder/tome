#!/usr/bin/env python3
"""make_wasm_mod.py <folder>: writes the e2e test's WebAssembly mod (wasm-e2e)
into <folder>: tome.mod.json and mod.wasm, assembled here (no Wasm toolchain
needed). tests/wasm_builder.h does the same for the host tests.

The module logs "loaded" from tome_mod_load, "enabled" and registers /wasme2e
from tome_mod_enable, and logs a command's arguments; "/wasme2e trap" reads
past its memory, a trap that disables it. Any other /wasme2e also subscribes
to hello-wasm.greeted (subscribe works on the main thread only, not in a first
enable) and it logs "got a message" for each one.
"""
import json
import os
import sys

I32 = 0x7F


def uleb(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        out.append(b | (0x80 if v else 0))
        if not v:
            return bytes(out)


def sleb(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        done = (v == 0 and not b & 0x40) or (v == -1 and b & 0x40)
        out.append(b | (0 if done else 0x80))
        if done:
            return bytes(out)


def name(s):
    s = s.encode()
    return uleb(len(s)) + s


def vec(items):
    return uleb(len(items)) + b"".join(items)


def section(i, body):
    return bytes([i]) + uleb(len(body)) + body


def i32(v):
    return b"\x41" + sleb(v)


def call(f):
    return b"\x10" + uleb(f)


def local(i):
    return b"\x20" + uleb(i)


def func(*code):
    body = b"\x00" + b"".join(code) + b"\x0b"  # no locals
    return uleb(len(body)) + body


types = [
    b"\x60" + vec([bytes([I32])] * 3) + vec([]),            # 0: (i32 i32 i32) -> ()
    b"\x60" + vec([bytes([I32])] * 3) + vec([bytes([I32])]),  # 1: (i32 i32 i32) -> i32
    b"\x60" + vec([]) + vec([bytes([I32])]),                  # 2: () -> i32
    b"\x60" + vec([]) + vec([]),                              # 3: () -> ()
    b"\x60" + vec([bytes([I32])]) + vec([bytes([I32])]),      # 4: (i32) -> i32
]
LOG, REGISTER, SUBSCRIBE = 0, 1, 2  # imports
imports = [name("tome") + name("log") + b"\x00" + uleb(0),
           name("tome") + name("register_command") + b"\x00" + uleb(1),
           name("tome") + name("subscribe") + b"\x00" + uleb(1)]
TEXTS = {"loaded": 16, "enabled": 32, "wasme2e": 48, "trap": 64, "hello-wasm.greeted": 80, "got a message": 112}
MESSAGE = 6  # the subscription's callback id (the command's is 5)


def log_text(t):
    return i32(1) + i32(TEXTS[t]) + i32(len(t)) + call(LOG)


# tome_callback(id, ptr, len): "trap" (4 bytes equal to "trap") reads past
# the page; anything else is logged. Compared as one i32 load.
trap_word = int.from_bytes(b"trap", "little")
callback = func(
    local(0), i32(MESSAGE), b"\x46", b"\x04\x40", log_text("got a message"), b"\x0f", b"\x0b",  # if id == MESSAGE: log, return
    local(2), i32(4), b"\x46",                      # len == 4
    b"\x04\x40",                                     # if
    local(1), b"\x28\x00\x00", i32(trap_word), b"\x46",  # i32.load (align 1) of the text == "trap"
    b"\x04\x40", i32(70000), b"\x28\x02\x00", b"\x1a", b"\x0b",  # if: load past the page, drop
    b"\x0b",
    i32(80) + i32(len("hello-wasm.greeted")) + i32(MESSAGE) + call(SUBSCRIBE) + b"\x1a",
    i32(1), local(1), local(2), call(LOG),
)
functions = [  # (type, body); indices from 3, after the imports
    (2, func(log_text("loaded"), i32(0))),                                                          # 3 tome_mod_load
    (3, func(log_text("enabled"), i32(48), i32(7), i32(5), call(REGISTER), b"\x1a")),    # 4 tome_mod_enable
    (4, func(i32(1024))),                                                                           # 5 tome_buffer
    (0, callback),                                                                                  # 6 tome_callback
]
exports = [name("memory") + b"\x02" + uleb(0),
           name("tome_mod_load") + b"\x00" + uleb(3),
           name("tome_mod_enable") + b"\x00" + uleb(4),
           name("tome_buffer") + b"\x00" + uleb(5),
           name("tome_callback") + b"\x00" + uleb(6)]
data = [b"\x00" + i32(at) + b"\x0b" + name(t) for t, at in TEXTS.items()]

module = (b"\x00asm\x01\x00\x00\x00" + section(1, vec(types)) + section(2, vec(imports)) +
          section(3, vec([uleb(t) for t, _ in functions])) + section(5, vec([b"\x00" + uleb(1)])) +
          section(7, vec(exports)) + section(10, vec([b for _, b in functions])) + section(11, vec(data)))

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
with open(os.path.join(out, "mod.wasm"), "wb") as f:
    f.write(module)
with open(os.path.join(out, "tome.mod.json"), "w", encoding="utf-8") as f:
    json.dump({"schema": 1, "id": "wasm-e2e", "name": "Wasm e2e", "version": "0.1.0", "game": "1.27b",
               "tome": "*", "sync": "client", "wasm": "mod.wasm"}, f, indent=2)
    f.write("\n")
