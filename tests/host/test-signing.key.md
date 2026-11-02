# test-signing.key: a public test key, not a secret

`test-signing.key` next to this file is an Ed25519 secret key **for tests
only**, published on purpose: its seed is 32 bytes of `B` (0x42). The host
tests sign fixtures with it (`tests/CMakeLists.txt`,
`make_signed_fixture.py`) to check that Tome and `sdk/support/tome_sign.py`
agree on what's signed.

Tome trusts no key by default: a signature only lets a later version of a
mod run without asking if the user already allowed one signed by the same
key (`SECURITY.md`). Anyone can sign with this key, so never sign a real mod
with it. Make your own with `python3 sdk/support/tome_sign.py keygen <file>`.

(The key file holds only the key: `tome_sign.py` reads nothing else in it.)
