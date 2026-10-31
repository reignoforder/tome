#!/usr/bin/env python3
"""make_signed_fixture.py <built crash mod> <out folder> <key file>: a copy of
the crash fixture with files in subfolders and non-ASCII names, signed by
sdk/support/tome_sign.py with the key (tests/host/test-signing.key, for tests
only and public: never sign a real mod with it). The host test
host_python_signature_is_accepted checks Tome hashes the folder the way the
signer does.
"""
import os
import shutil
import sys

sys.dont_write_bytecode = True  # no __pycache__ in sdk/support from builds
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "sdk", "support"))
import tome_sign  # noqa: E402

src, out, key = sys.argv[1], sys.argv[2], sys.argv[3]
shutil.rmtree(out, ignore_errors=True)
shutil.copytree(src, out)
for rel, data in [("assets/Sub/b.txt", b"b"), ("assets/a.txt", b"a"), ("assets/Ünïcode.txt", b"u"),
                  ("zz/x.bin", b"\x00\x01"), ("Upper.txt", b"U")]:
    path = os.path.join(out, *rel.split("/"))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
tome_sign.cmd_sign(out, key)
