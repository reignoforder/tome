# Tome: notes for working in this repo

Tome is a Fabric-style mod loader for Warcraft III **1.27b** (game.dll
1.27.1.7085, 32-bit). Start with `docs/ROADMAP.md` (what's next), then
`docs/architecture.md` (how it works) and `docs/reverse-engineering.md`
(game internals, addresses, methods). `docs/research.md` is the original
design document.

## Commands

```sh
make                                     # configure (once: Ninja, mingw toolchain, dev tools) and build
make test                                # unit tests (ctest, Wine)
make e2e                                 # real game, 6 launches (~5 min), off screen in Xvfb
tools/game.sh start [VAR=val] / wait <regex> / shot <png> / stop / log / dir
tools/fuzz.sh [seconds] [target...]      # libFuzzer + ASan/UBSan on the core parsers (host clang, build-fuzz/)
tools/gen-rust-bindings.sh               # after changing tome.h: regenerate sdk/rust/tome-sys (bindgen)
make sdk                                 # build/tome-sdk-<version>.zip, the SDK for mod makers
make container-test / container-e2e      # the same in a podman/docker image (tools/container/)
make deploy                              # copy tome.mix into GAME_DIR: only when asked to
```

The game for testing (`tools/test-env.sh`) runs from a folder of links to
`GAME_DIR` (default `./WarCraft III`, only read) in a Wine prefix of its own
(`build/test-env/wine`: 800x600, profile Tome). `HEADLESS=1` (the e2e
default) puts it in its own Xvfb; `tools/game.sh` shows it on screen unless
`HEADLESS=1`. Its folder is `build/test-env/game/` (`tools/game.sh dir`).

## Rules

- **Never commit game files.** `WarCraft III/` is the local install and is
  gitignored, along with game data and binary types. Don't describe where
  the game files came from in committed text.
- Store **RVAs**, not absolute addresses (ASLR). Add every new address to
  `mappings/<build>.json` with its calling convention and signature, and
  document it in `docs/reverse-engineering.md`. Tome's own uses also go
  through `BuildMappings`/`bind_build` in `loader/src/core/mappings.*`.
- Patch only after checking the target bytes (`patch_call`, `patch_pointer`).
- Mod lifecycle and anything touching game state runs on the **main thread**
  (`main_thread::post`).
- Portable logic goes in `loader/src/core/` with unit tests; Windows code goes in `loader/src/`.
- A `core/` parser of untrusted bytes (other players, maps, packages,
  `tome.mod.json`) gets a target in `fuzz/`; a crashing input goes into
  `fuzz/regressions/<target>/` with a unit test.
- Code style: C++20, no exceptions/RTTI, `TOME_INFO`/`WARN`/`ERROR` logging.
  The mod API in `sdk/include/tome/tome.h` is plain C and append-only.
  After changing it, run `tools/gen-rust-bindings.sh` (a test checks). The
  C++ helpers (`tome.hpp`, generic `Mod::call`) need no change; the Rust
  crate `sdk/rust/tome` gets a wrapper only when a mod needs one (until
  then `Tome::raw`). Released fields go into `tests/test_abi.cpp`.
- `sdk/` is for mod makers and must build on its own (the `sdk-dist` zip):
  nothing it builds may use files outside `sdk/`. Maintainer checks may
  (the Rust bindings test runs `tools/gen-rust-bindings.sh`), if they're
  skipped when the file isn't there. A new `tome.mod.json` or
  settings keyword also goes into `sdk/schemas/` and `sdk/docs/tome-mod-json.md`.

## Pitfalls (each one cost time already)

- **Never kill the game with `pkill -f`/`pgrep -f` and a pattern.** It also
  matches your own shell. Use `tools/game.sh stop`, which ends only the
  test prefix's programs (`wineserver -k`), never the user's own game.
- **One test game at a time** (the e2e test and `tools/game.sh` share the
  test prefix). The user's own game may run alongside, but takes the LAN port.
- **Tests must use `TOME_HOME`** (a temp folder), never the real `tome/mods/` folder.
- **New mods with a DLL don't run until allowed** (approval, `core/mod_state.*`).
  A test that installs one sets `TOME_DEBUG_APPROVE=<ids>` (or `*`); for
  `tools/game.sh start` pass it like any other variable.
- **Show/Hide:** vtable `+0xD4` is Show and `+0xD0` is Hide. Show also shows all children.
- **Don't reload all of FrameDef.toc after startup:** it crashes later map
  loads. Use LoadTocFiles for Tome's own TOC.
- **GCC warns about `__thiscall` on free functions but applies it.** The warning is silenced in `mod_menu.cpp`.
- **MinGW printf needs `__USE_MINGW_ANSI_STDIO=1`** (set globally) for `%zu`,
  and logging uses the `gnu_printf` format attribute.
- **Wine doesn't report file changes inside subfolders**; the watcher also polls there (only under Wine).
- **Stop the game before rewriting a map with `tome-mpq put`.** A running
  game keeps the map open, `put` fails, and the old map loads as if
  nothing changed.
- **UI opened from code** only shows once the menu has finished appearing (about 3 s after Setup).
- **Screenshots:** `tools/game.sh shot file.png` (ImageMagick `import` of the
  game's display). Simulate clicks with `TOME_DEBUG_CLICKS="x,y@ms;..."`
  (fractions of the window). Start each part with `wait:<log text>@0`
  (`Main menu set up`, `Screen: <name>`, `Game started: `): later times
  count from the wait, so slow software rendering doesn't break the script.
- **Check `ret N` before mapping a function's `sig`.** A hook pops what
  the signature says; one stack argument too few crashed the game.
- **Keys in tests:** `TOME_DEBUG_CLICKS` items `key:Ctrl+J@ms` press a key
  through `keybd_event` (real modifier state); the window needs the focus,
  which it always has in Xvfb (`HEADLESS=1`), not always on screen.
- **Mod callbacks run under the mod manager's lock** (enable, first load).
  API functions mods call there must not take it again (SRW locks aren't
  reentrant): settings have their own lock, keybinds get pushed their list.
- **Chat in tests:** `TOME_DEBUG_CLICKS` items `type:/text@ms` type a chat
  line. Tome takes the focus back from mods' in-game buttons after a click;
  other buttons that keep it take the next Enter instead of the chat box.
- **LAN games need an opponent:** set the second slot to a computer
  (clicks in `tools/e2e-test.sh` run 5).
- **The e2e test map has no opponents:** melee would declare victory at
  once (its dialog pauses the game), so the patched script starts the check
  from a 20 s timer. The dialog takes the keyboard: chat lines typed after
  it go nowhere. A trigger's `PauseGame` clears the selection, and a
  single player game pauses when its window loses the focus (timers stop).
- **Saved games from scripts:** `SaveGame`/`LoadGame` names need `.w3z`
  (nothing adds it, and a load without it silently goes back to the menu).
  The e2e test removes its save from `save/<profile>/CustomSaves/e2e`.
- **Leaving a game:** Quit Mission → score screen → OK returns to the Custom
  Game screen (two Cancels to the main menu), and InitNatives runs once more
  on the way out. The click path is in `tools/e2e-test.sh` run 4.
