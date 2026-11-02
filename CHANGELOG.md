# Changelog

What changed in each release of Tome, newest first. Release versions follow
[Semantic Versioning](https://semver.org); while they start with 0, a minor
version (0.2.0) may change what mods or players rely on, and says so here.

## Versions

Tome has three version numbers, which move on their own:

- **The release version** (`0.1.0`: the project version in `CMakeLists.txt`,
  the tag `v0.1.0`, the zips' names, `[Tome 0.1.0]` in the window title).
- **The API version** (`TOME_API_VERSION` in `sdk/include/tome/tome.h`, the
  `api_version` a mod receives). It goes up by one whenever `tome.h` gains
  functions or fields, never for anything else. `tome.h` is append-only:
  nothing released is moved, removed or changed, and `tests/test_abi.cpp`
  pins every released field's offset, so a mod built against an older API
  version keeps working with every later Tome. A mod that needs something
  newer checks `api_version` (or `size`, for a struct's fields) first.
- **The mapping schema** (`"schema"` in `mappings/<build>.json`): the format
  of the game build's address and signature database, embedded in
  `tome.mix`.

A release lists the API version it ships. A release that adds API keeps the
API version of each addition in `tome.h` and `sdk/README.md` ("[14]").

## [0.1.0] - unreleased

The first public release. Warcraft III 1.27b (game.dll 1.27.1.7085) only;
API version 14.

### Loader

- `tome.mix` next to `war3.exe`, loaded by the game's sound library: no
  patched executable or injector. The build is identified by SHA-256 and by
  checking the bytes of every function before patching it; on any other
  build Tome logs it and stays idle.
- Mods in `tome/mods/`, as folders or `.tome` packages; `tome.mod.json`
  checked against its schema, dependencies resolved, problems reported per
  mod. Live toggling and hot reloading on the game's main thread.
- Mods with code run only after the user allows them once (a SHA-256 of
  their files); a changed package asks again unless signed by the same
  author (`tome.sig`, Ed25519).
- Crash isolation: a mod that faults is stopped with its dependents and the
  game runs on. Safe mode (Shift at start, `-tomesafe`, or after three
  starts that crash), crash reports in `tome/crashes/`.
- Built-in fixes for the game: the memhack guard (the JASS compiler's
  local/global typecast), the path sanitizer (path traversal in 38 natives),
  and the BLP texture guard; widescreen (Tome > Settings), muting in the
  background, the day and night ambient sounds; under Wine, movies that
  play instead of crashing or hanging, and Alt+Tab in fullscreen (OpenGL
  textures, Direct3D lights).
- Multiplayer: required mods compared in the lobby (the host refuses to
  start while they differ), sent as game actions (replays carry them), and
  locked during a game.

### Mods menu

- A Mods button on the main menu and in the game's menu (Ctrl+F10): every
  mod with its status and problems, details, settings pages from
  `config.schema.json`, keybinds with conflict checks, mod icons, and a
  Tome page with a safe-mode switch. Popups and restarting for mods.

### Mod API and SDK (MIT)

- A C API (`tome.h`): JASS natives, hooks on mapped game functions and
  imports, events (game, units, players, UI, chat, selection, and more),
  frames, chat commands, settings, keybinds, services and messages between
  mods, the game's files, UTF-8 helpers.
- C++ helpers (`tome.hpp`), a Rust crate (`tome`), and sandboxed WebAssembly
  mods (experimental; C and Rust).
- `TomeMod.cmake`, templates and examples in each language, `tome-check`
  and `tome_test.dll` to test mods without the game, `tome_package()` and
  `tome_sign.py`. The SDK zip builds on its own.

### Downloads

- `tome-0.1.0.zip` for players, `tome-sdk-0.1.0.zip` for mod makers, and the
  WAMR source archive, with their SHA-256s.
