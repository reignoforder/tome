#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Signs and checks Tome's releases: SHA256SUMS.sig, an Ed25519 signature over
a release's SHA256SUMS (loader/src/core/update_check.h; docs/releasing.md).

  release_sign.py keygen <secret key file>   make a release key; prints its public key
  release_sign.py sign <SHA256SUMS> <secret key file>
                                             write SHA256SUMS.sig next to it
  release_sign.py verify <SHA256SUMS> [<SHA256SUMS.sig>]
                                             check it against loader/release-keys.txt
  release_sign.py check-keys                 loader/release-keys.txt has a key (CI)
  release_sign.py selftest                   check the signing code (the tests run it)

The version comes from SHA256SUMS (its tome-<version>.zip line) and is part
of what's signed, so a signature can't be moved to another version. sign
refuses a key that isn't in loader/release-keys.txt: Tome wouldn't accept it.

Sign on your own computer, with the secret key kept offline (never in the
repository or a GitHub secret), and only SHA256SUMS that CI made: compare it
with the hashes in the release job's log and the attestations first.
"""
import base64
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "sdk", "support"))
import tome_sign  # noqa: E402  (the Ed25519 code: RFC 8032's reference implementation)

KEYS_FILE = os.path.join(ROOT, "loader", "release-keys.txt")
ZIP_LINE = re.compile(r"^[0-9a-f]{64} [ *]tome-([0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?)\.zip$")


def b64(data):
    return base64.b64encode(data).decode("ascii")


def read_keys(path=KEYS_FILE):
    keys = []
    with open(path, "r", encoding="ascii") as f:
        for number, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            try:
                key = base64.b64decode(line, validate=True)
            except ValueError:
                key = b""
            if len(key) != 32:
                raise SystemExit("%s, line %d: not a base64 public key" % (path, number))
            keys.append(key)
    return keys


def version_of(sums):
    versions = []
    for line in sums.decode("utf-8").split("\n"):
        m = ZIP_LINE.match(line.rstrip("\r"))
        if m:
            versions.append(m.group(1))
    if len(versions) != 1:
        raise SystemExit("SHA256SUMS must list exactly one tome-<version>.zip (found %d)" % len(versions))
    return versions[0]


def message(version, sums):
    return b"tome-release-v1\n" + version.encode("ascii") + b"\n" + sums


def cmd_keygen(path):
    tome_sign.cmd_keygen(path)
    print("Add the public key to loader/release-keys.txt; keep the secret key offline, with a backup.")


def cmd_sign(sums_path, key_path):
    secret = tome_sign._read_secret(key_path)
    public = tome_sign.public_key(secret)
    if public not in read_keys():
        raise SystemExit("%s isn't in loader/release-keys.txt: Tome wouldn't accept this signature" % b64(public))
    with open(sums_path, "rb") as f:
        sums = f.read()
    version = version_of(sums)
    sig = tome_sign.sign(secret, message(version, sums))
    out = sums_path + ".sig"
    with open(out, "w", encoding="ascii", newline="\n") as f:
        json.dump({"schema": 1, "key": b64(public), "signature": b64(sig)}, f, indent=2)
        f.write("\n")
    print("Signed Tome %s's %s with %s: %s" % (version, os.path.basename(sums_path), b64(public), out))


def cmd_verify(sums_path, sig_path=None):
    with open(sums_path, "rb") as f:
        sums = f.read()
    with open(sig_path or sums_path + ".sig", "r", encoding="utf-8") as f:
        data = json.load(f)
    try:
        public = base64.b64decode(data["key"], validate=True)
        sig = base64.b64decode(data["signature"], validate=True)
    except (KeyError, TypeError, ValueError):
        raise SystemExit("SHA256SUMS.sig: malformed")
    if data.get("schema") != 1 or len(public) != 32 or len(sig) != 64:
        raise SystemExit("SHA256SUMS.sig: malformed")
    if public not in read_keys():
        raise SystemExit("signed with %s, which isn't in loader/release-keys.txt" % b64(public))
    version = version_of(sums)
    if not tome_sign.verify(public, message(version, sums), sig):
        raise SystemExit("the signature doesn't match SHA256SUMS")
    print("Tome %s: SHA256SUMS signed by %s" % (version, b64(public)))


def cmd_selftest():
    # RFC 8032's first test key; tests/test_update_check.cpp checks the same
    # signature in C++, so both implementations agree on the message.
    secret = bytes.fromhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
    sums = b"a" * 64 + b"  tome-0.2.0.zip\n" + b"b" * 64 + b"  tome-sdk-0.2.0.zip\n"
    expected = ("IJMzA9ZjeAREI7UyI1cmUdEWpUqMUGNMAjQqZOhi9fEnv6uLgnrTf6/w+b91fLCFwsNHFhbn+6jPv5TJU3xxBg==")
    version = version_of(sums)
    sig = tome_sign.sign(secret, message(version, sums))
    public = tome_sign.public_key(secret)
    ok = (version == "0.2.0" and b64(sig) == expected and tome_sign.verify(public, message(version, sums), sig)
          and not tome_sign.verify(public, message("0.3.0", sums), sig))
    for bad in (b"", b"a" * 64 + b"  tome-sdk-0.2.0.zip\n", sums + b"c" * 64 + b"  tome-0.3.0.zip\n"):
        try:
            version_of(bad)
            ok = False
        except SystemExit:
            pass
    if not ok:
        raise SystemExit("selftest failed")
    print("selftest passed")


def cmd_check_keys():
    keys = read_keys()
    if not keys:
        raise SystemExit("loader/release-keys.txt has no key: releases can't be signed or announced")
    print("%d release key(s)" % len(keys))


def main(argv):
    commands = {"keygen": (cmd_keygen, (1, 1)), "sign": (cmd_sign, (2, 2)), "verify": (cmd_verify, (1, 2)),
                "check-keys": (cmd_check_keys, (0, 0)), "selftest": (cmd_selftest, (0, 0))}
    if len(argv) < 2 or argv[1] not in commands:
        sys.stderr.write(__doc__)
        return 2
    fn, (low, high) = commands[argv[1]]
    if not low <= len(argv) - 2 <= high:
        sys.stderr.write(__doc__)
        return 2
    fn(*argv[2:])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
