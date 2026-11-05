#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Signs Tome mods: writes tome.sig, the mod author's Ed25519 signature over a
mod's files (loader/src/core/signature.h in Tome's repository).

  tome_sign.py keygen <secret key file>     make a key; prints the public key
  tome_sign.py public <secret key file>     print the public key of a key
  tome_sign.py sign <mod folder> <secret key file>
                                            write <mod folder>/tome.sig
  tome_sign.py verify <mod folder>          check <mod folder>/tome.sig
  tome_sign.py selftest                     check the Ed25519 code (RFC 8032)

Sign the folder as it will be installed or packed (tome_package's SIGN_KEY
does both): any change to a file afterwards, tome.mod.json included, breaks
the signature. When a player allowed your mod, an update signed with the same
key is allowed without asking; with another key, or none, Tome asks again.
So keep the key file secret and safe, and publish the public key (the mod's
page, its repository) so players can compare it with the one Tome shows.

Plain Python 3, no packages. The Ed25519 code is RFC 8032's reference
implementation (section 6): correct, but not constant-time. Sign on your own
computer, not on a shared machine.
"""
import base64
import hashlib
import json
import os
import secrets
import sys

SIG_FILE = "tome.sig"
METADATA = "tome.mod.json"

# --- Ed25519 (RFC 8032, section 6) ------------------------------------------

p = 2**255 - 19
q = 2**252 + 27742317777372353535851937790883648493


def _inv(x):
    return pow(x, p - 2, p)


d = -121665 * _inv(121666) % p
_sqrt_m1 = pow(2, (p - 1) // 4, p)


def _add(P, Q):
    A = (P[1] - P[0]) * (Q[1] - Q[0]) % p
    B = (P[1] + P[0]) * (Q[1] + Q[0]) % p
    C = 2 * P[3] * Q[3] * d % p
    D = 2 * P[2] * Q[2] % p
    E, F, G, H = B - A, D - C, D + C, B + A
    return (E * F, G * H, F * G, E * H)


def _mul(s, P):
    Q = (0, 1, 1, 0)
    while s > 0:
        if s & 1:
            Q = _add(Q, P)
        P = _add(P, P)
        s >>= 1
    return Q


def _equal(P, Q):
    return (P[0] * Q[2] - Q[0] * P[2]) % p == 0 and (P[1] * Q[2] - Q[1] * P[2]) % p == 0


def _recover_x(y, sign):
    if y >= p:
        return None
    x2 = (y * y - 1) * _inv(d * y * y + 1)
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (p + 3) // 8, p)
    if (x * x - x2) % p != 0:
        x = x * _sqrt_m1 % p
    if (x * x - x2) % p != 0:
        return None
    if (x & 1) != sign:
        x = p - x
    return x


_gy = 4 * _inv(5) % p
_gx = _recover_x(_gy, 0)
G = (_gx, _gy, 1, _gx * _gy % p)


def _compress(P):
    zinv = _inv(P[2])
    x = P[0] * zinv % p
    y = P[1] * zinv % p
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")


def _decompress(s):
    if len(s) != 32:
        return None
    y = int.from_bytes(s, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    x = _recover_x(y, sign)
    if x is None:
        return None
    return (x, y, 1, x * y % p)


def _hash_q(data):
    return int.from_bytes(hashlib.sha512(data).digest(), "little") % q


def _expand(secret):
    h = hashlib.sha512(secret).digest()
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return a, h[32:]


def public_key(secret):
    a, _ = _expand(secret)
    return _compress(_mul(a, G))


def sign(secret, message):
    a, prefix = _expand(secret)
    A = _compress(_mul(a, G))
    r = _hash_q(prefix + message)
    R = _compress(_mul(r, G))
    s = (r + _hash_q(R + A + message) * a) % q
    return R + int.to_bytes(s, 32, "little")


def small_order(P):
    """Points anyone can "sign" for (Tome refuses such keys)."""
    return _equal(_mul(8, P), (0, 1, 1, 0))


def verify(public, message, signature):
    if len(public) != 32 or len(signature) != 64:
        return False
    A = _decompress(public)
    R = _decompress(signature[:32])
    if not A or not R or small_order(A):
        return False
    s = int.from_bytes(signature[32:], "little")
    if s >= q:
        return False
    h = _hash_q(signature[:32] + public + message)
    return _equal(_mul(s, G), _add(R, _mul(h, A)))


# --- mods --------------------------------------------------------------------


def _sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def code_hash(folder):
    """The SHA-256 Tome's approval records (core/mod_state.h, schema 3): of a
    sha256sum-style manifest of every file but the top tome.mod.json and
    tome.sig, sorted by path."""
    lines = []
    for root, dirs, files in os.walk(folder):
        dirs.sort()
        for name in files:
            path = os.path.join(root, name)
            rel = os.path.relpath(path, folder).replace(os.sep, "/")
            if rel.lower() in (METADATA, SIG_FILE):
                continue
            lines.append((rel.encode("utf-8"), _sha256_file(path)))
    lines.sort()
    manifest = "".join("%s  %s\n" % (sha, rel.decode("utf-8")) for rel, sha in lines)
    return hashlib.sha256(manifest.encode("utf-8")).hexdigest()


def message(folder):
    """What's signed (core/signature.h, message())."""
    meta_path = os.path.join(folder, METADATA)
    with open(meta_path, "rb") as f:
        meta = json.loads(f.read().decode("utf-8"))
    mod_id = meta.get("id")
    if not isinstance(mod_id, str) or not mod_id:
        raise SystemExit("%s: no \"id\"" % meta_path)
    text = "tome-signature-v1\n%s\n%s\n%s\n" % (mod_id, code_hash(folder), _sha256_file(meta_path))
    return mod_id, text.encode("utf-8")


def _read_secret(path):
    with open(path, "r", encoding="ascii") as f:
        text = f.read().strip()
    try:
        secret = base64.b64decode(text, validate=True)
    except ValueError:
        secret = b""
    if len(secret) != 32:
        raise SystemExit("%s: not a key made by tome_sign.py keygen" % path)
    return secret


def _b64(data):
    return base64.b64encode(data).decode("ascii")


def cmd_keygen(path):
    if os.path.exists(path):
        raise SystemExit("%s exists; not overwriting a key" % path)
    secret = secrets.token_bytes(32)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w", encoding="ascii") as f:
        f.write(_b64(secret) + "\n")
    print("Secret key: %s (keep it secret; back it up)" % path)
    print("Public key: %s" % _b64(public_key(secret)))


def cmd_public(path):
    print(_b64(public_key(_read_secret(path))))


def cmd_sign(folder, key_path):
    secret = _read_secret(key_path)
    mod_id, text = message(folder)
    public = public_key(secret)
    sig = sign(secret, text)
    with open(os.path.join(folder, SIG_FILE), "w", encoding="ascii", newline="\n") as f:
        json.dump({"schema": 1, "key": _b64(public), "signature": _b64(sig)}, f, indent=2)
        f.write("\n")
    print("Signed %s with %s" % (mod_id, _b64(public)))


def cmd_verify(folder):
    with open(os.path.join(folder, SIG_FILE), "r", encoding="utf-8") as f:
        data = json.load(f)
    try:
        public = base64.b64decode(data["key"], validate=True)
        sig = base64.b64decode(data["signature"], validate=True)
    except (KeyError, TypeError, ValueError):
        raise SystemExit("%s: malformed" % SIG_FILE)
    mod_id, text = message(folder)
    if data.get("schema") != 1 or not verify(public, text, sig):
        raise SystemExit("%s: the signature doesn't match the files" % mod_id)
    print("%s: signed by %s" % (mod_id, _b64(public)))


def cmd_selftest():
    # RFC 8032, section 7.1, tests 1-3.
    vectors = [
        ("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
         "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"),
        ("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
         "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"),
        ("c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
         "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
         "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"),
    ]
    for secret, public, msg, sig in vectors:
        secret, public, msg, sig = (bytes.fromhex(x) for x in (secret, public, msg, sig))
        if public_key(secret) != public or sign(secret, msg) != sig or not verify(public, msg, sig):
            raise SystemExit("selftest: FAILED")
        if verify(public, msg + b"x", sig):
            raise SystemExit("selftest: FAILED (accepted a changed message)")
    if verify(bytes(32), b"", bytes(64)):
        raise SystemExit("selftest: FAILED (accepted a key of small order)")
    print("selftest: ok")


def main(argv):
    commands = {"keygen": (cmd_keygen, 1), "public": (cmd_public, 1), "sign": (cmd_sign, 2),
                "verify": (cmd_verify, 1), "selftest": (cmd_selftest, 0)}
    if len(argv) < 2 or argv[1] not in commands or len(argv) - 2 != commands[argv[1]][1]:
        sys.stderr.write(__doc__)
        return 2
    commands[argv[1]][0](*argv[2:])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
