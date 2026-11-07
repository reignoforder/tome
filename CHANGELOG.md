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

## Unreleased

API version 18.

- **Mods: messages between players** (`net_send`, `on_net_message`, also for
  WebAssembly mods): up to 1 KiB to the same mod on every player's computer,
  in the same game step, recorded in replays. From a game's first step on
  (not at `GAME_START`); in multiplayer, for `"sync": "required"` mods.
- **Mods: the game's host** (`host_player`, also for WebAssembly mods): the
  player slot of the player who created the current game.
- **The lobby handshake knows the host**: a client counts only the host's
  required mods and names any other player who sends a set. The host's
  router stamps every lobby line's sender, so a client can't pass for the
  host (checked in a LAN game).
- **lan-map-guard** (built-in patch): refuses a host's map path that would
  make the client open a file outside `Maps\` or a device.
- **Stop on Memory Corruption** (Tome > Settings, off by default): Windows'
  heap termination on corruption, from the next start.

## [0.1.0] - unreleased

The first public release. Warcraft III 1.27b (game.dll 1.27.1.7085) only;
API version 16.

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
- Update check, if you allow it (asked on the first start; Tome >
  Settings): once a day Tome asks GitHub for its latest release and says
  when there's a newer one (a popup with its release page, and the Tome
  page). Only releases signed with Tome's release key are announced, with
  the zip's SHA-256. **Update Now** in the popup downloads the zip, installs
  it only if its SHA-256 is the signed one, and offers a restart; the
  previous `tome.mix` stays as `tome.mix.old` and is put back if the new one
  doesn't reach the main menu twice. **Install Updates Automatically**
  (Tome > Settings, off by default) installs a signed new version as soon
  as it's found instead of asking; it runs from the next start. Tome's only
  connection of its own.
- Releases: reproducible builds (the same commit gives the same bytes),
  build provenance attestations, and an offline signature of `SHA256SUMS`
  (`docs/releasing.md`).

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
- Game objects (API version 16): units, items, destructables and players,
  checked before every use; properties read and changed by name on every
  supported game version (`get_property`, `set_property`, and keyed ones
  for abilities, weapons and hero attributes; `"requires": ["property:mana"]`):
  life, mana, position, facing, move speed, level, ranges, armor, damage,
  attack cooldown and range, hero attributes and experience, ability
  levels, a player's resources; every unit, item, destructable or player
  in the game (`list_objects`), with or without a handle; JASS handle ids to objects and back (`object_of`,
  `handle_of`); any JASS native by name (`jass_call`, for WebAssembly
  mods too), with handle arguments checked against the handle table and
  the parameter's type (texttags as the indices they are). In multiplayer
  games and replays only `"sync": "required"` mods may change properties
  or run natives.
- C++ helpers (`tome.hpp`), a Rust crate (`tome`), and sandboxed WebAssembly
  mods (experimental; C and Rust).
- `TomeMod.cmake`, templates and examples in each language, `tome-check`
  and `tome_test.dll` to test mods without the game, `tome_package()` and
  `tome_sign.py`. The SDK zip builds on its own.

### Downloads

- `tome-0.1.0.zip` for players, `tome-sdk-0.1.0.zip` for mod makers, and the
  WAMR source archive, with their SHA-256s.
