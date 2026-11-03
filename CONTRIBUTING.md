# Contributing to Tome

Thanks for helping. Bug reports, mods, docs and code are all welcome.

- **A security problem** (a way around mod approval, a map or another
  player making Tome run code or write files): report it privately, as
  `SECURITY.md` says, not in an issue.
- **A bug:** open an issue with the bug template. Attach `tome/tome.log` and,
  after a crash, the report in `tome/crashes/`, and say which game build
  and mods you use.
- **A feature or a larger change:** open an issue first, so we can agree on
  the approach before you spend time on it. `docs/ROADMAP.md` says what's
  planned.

## Building and testing

`README.md` ("Building", "Testing") has the details. In short, with CMake,
Ninja, a 32-bit MinGW-w64 toolchain and Wine (or only podman or docker):

```sh
make                 # build (a developer build: reads the TOME_DEBUG_* test switches)
make test            # unit tests, the loader on a fake game, and on game.dll if you have it
make container-test  # the same in a container with the whole toolchain (what CI runs)
make e2e             # the real game, off screen: needs your own game files
```

**The end-to-end test runs only on your machine.** It needs the files of a
Warcraft III 1.27b install in `input/1.27b/` (`input/README.md`), which this
repository doesn't have and CI can't use. Run it before a pull request
that touches anything the game runs (patches, hooks, events, the UI, the
handshake), and say in the pull request whether it passed. Without the game,
`make test` still runs everything that doesn't need it.

`docs/architecture.md` explains how Tome works, and
`docs/reverse-engineering.md` the game's internals it relies on.

## Rules

`CLAUDE.md` holds the full list, with the pitfalls that already cost time.
The ones every change meets:

- **Never commit the game's files** (or anything taken from them), and
  don't describe in committed text where game files came from.
- **Addresses are RVAs** in `mappings/<build>.json`, each with its calling
  convention, signature and bytes, and documented in
  `docs/reverse-engineering.md`. Code is patched only after its bytes are
  checked.
- **Game state on the main thread** (`main_thread::post`), mods' calls
  included.
- **Tests:** portable logic goes in `loader/src/core/` with unit tests;
  prefer a deterministic test (`tests/host/`, `tests/game/`) over an e2e
  check. A parser of untrusted bytes (other players, maps, packages,
  `tome.mod.json`) gets a fuzz target in `fuzz/`.
- **The mod API (`sdk/include/tome/tome.h`) is plain C and append-only.**
  Never move, remove or change a released field; a new one goes at the end
  and into `tests/test_abi.cpp`, and the API version goes up
  (`CHANGELOG.md`, "Versions"). Then run `tools/gen-rust-bindings.sh` and
  `tools/gen-wasm-consts.py` (tests check both).
- **`sdk/` builds on its own** (the SDK zip): nothing in it may use files
  outside `sdk/`.
- **Style:** C++20 without exceptions or RTTI, logging with
  `TOME_INFO`/`TOME_WARN`/`TOME_ERROR`; match the code around yours.
- **License lines:** every source file starts with an
  `SPDX-License-Identifier` line; `python3 -I tools/spdx.py` adds the
  right one, and a test checks.

## Licenses of contributions

Tome is under the GNU GPL v3 or later (`LICENSE`); `sdk/` is under the MIT
license (`sdk/LICENSE`), so mods may use any license. What you contribute is
under the license of the files it goes into, as each file's
`SPDX-License-Identifier` line says. Third-party code that ends up in the
binaries needs a license compatible with that, its license text in
`loader/third_party/`, and a row in `loader/third_party/NOTICES.md`.

## Pull requests

- One topic per pull request, with tests for what it changes.
- `make test` passes (CI runs it in the container, with a short fuzz run);
  say whether you ran `make e2e`.
- Update the docs it affects: `README.md`, `sdk/README.md` and
  `sdk/docs/tome-mod-json.md` for mod makers, `CHANGELOG.md` for anything
  users or mod makers notice.
