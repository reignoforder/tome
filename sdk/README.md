# Tome SDK

Everything for writing Tome mods for Warcraft III 1.27b: the API, build
helpers, examples and starting templates in C, C++ and Rust.

| | |
|---|---|
| [`include/tome/tome.h`](include/tome/tome.h) | The API, in C. Every function is documented there. |
| [`include/tome/tome.hpp`](include/tome/tome.hpp) | C++17 helpers: lambdas for callbacks, exceptions reported as crashes ([guide](examples/hello-cpp/README.md)). |
| [`include/tome/utf8.h`](include/tome/utf8.h) | UTF-8 helpers (JASS strings are UTF-8). |
| [`rust/`](rust/README.md) | Rust: raw bindings (`tome-sys`) and a safe API (`tome`). |
| [`templates/`](templates) | The smallest useful mod in [C](templates/c), [C++](templates/cpp) and [Rust](templates/rust): copy one to start. |
| [`examples/`](examples) | The same full example in [C](examples/hello), [C++](examples/hello-cpp) and [Rust](rust/examples/hello-rs). |
| [`docs/tome-mod-json.md`](docs/tome-mod-json.md) | `tome.mod.json` and settings, field by field. |
| [`schemas/`](schemas) | JSON Schemas for both files, for your editor. |
| [`TomeMod.cmake`](TomeMod.cmake), [`cmake/`](cmake) | `tome_add_mod()`, `tome_add_rust_mod()`, `tome_package()`; the MinGW toolchain. |
| [`support/`](support) | Linked into C and C++ mods (`fpreset.c`); `make_blp.py` makes a placeholder icon (`make_blp.py icon.blp 32 3070d0`). |

## What a mod is

A mod is a folder in the game's `tome/mods/` (or a `.tome` file, a zip of
the folder). It holds:

- **`tome.mod.json`** (required): its id, version and supported game and
  Tome versions ([reference](docs/tome-mod-json.md)).
- **a DLL** (optional), 32-bit like the game. It exports `tome_mod_load`
  (required), plus `tome_mod_enable`, `tome_mod_disable` and
  `tome_mod_unload` if it needs them.
- **`assets/`** (optional): game files to add or replace (textures, models,
  FDF, scripts). Mods without code can be just this.
- **`config.schema.json`** (optional): settings, shown on the mod's page in
  the Mods menu.

Tome calls `tome_mod_load` once, then `tome_mod_enable` each time the mod is
turned on. Register everything there: natives, callbacks, hooks, commands.
Tome takes it all back when the mod is turned off, so `tome_mod_disable`
only has to undo what Tome can't, such as units the mod spawned.

## What mods can do

From the `tome_api` table a mod receives. The API version each part arrived
in is shown in brackets; check `api_version`, or let the C++ and Rust
helpers check for you.

- **JASS natives** [1, 6]: new natives that maps call. They take integer,
  real, boolean, handle, string and `code` values; a `code` value is a
  script function the mod can run later.
- **Game events** [5, 8, 12]: map load, game start and end, ticks, selection,
  chat, pause, every unit event (deaths, training, spells, damage, orders...),
  players, resources, day and night, saved games.
- **UI** [3, 5]: frames from the mod's own FDF files on every menu screen and
  in games, button clicks, popups [10].
- **Hooks** [4, 9]: run code before, after or instead of a mapped game
  function or an import of game.dll.
- **Settings and keybinds** [7]: pages in the Mods menu, generated from the
  mod's files.
- **Chat commands** [5]: `/name ...` in games.
- **Other mods** [11]: offer and use tables of functions (services), and
  publish and subscribe to messages.
- **The mapping database** [2]: addresses of game functions and data by name
  (`resolve`), so a mod needs no build-specific numbers.
- **Game files** [14]: read any file as the game would, through its archives
  and the mods' assets (`read_game_file`).

A mod built against an older `tome.h` keeps working with a newer Tome: the
API only ever grows.

## Start a mod

Pick a template, copy its folder, and rename its id (the folder's
`tome.mod.json` and build file).

**C or C++.** You need CMake and MinGW's i686 cross compiler. On Fedora that's
`mingw32-gcc` and `mingw32-gcc-c++`; on Debian, `gcc-mingw-w64-i686` and
`g++-mingw-w64-i686`.

```sh
cmake -S . -B build -DTOME_SDK_DIR=<sdk> -DCMAKE_TOOLCHAIN_FILE=<sdk>/cmake/toolchain-i686-mingw.cmake
cmake --build build      # build/mods/<id>/ and build/packages/<id>.tome
```

On Windows, Visual Studio (`-A Win32`) should work as well, but it isn't
tested yet.

**Rust.** You need rustup's `i686-pc-windows-gnu` target and the same MinGW
compiler, which Rust links with. Point the template's `tome` dependency at
`<sdk>/rust/tome`, then:

```sh
cargo build --release    # target/i686-pc-windows-gnu/release/<name>.dll
```

`<sdk>` is this folder: a Tome checkout's `sdk/`, or the SDK zip unpacked.
To make the zip, run `cmake --build build --target sdk-dist` in Tome's build;
the result is `build/tome-sdk-<version>.zip`. The SDK also builds on its own
(`cmake -S <sdk> -B build -DCMAKE_TOOLCHAIN_FILE=...`), which builds every
example and template.

## Try it in the game

1. Install Tome (`tome.mix` next to `war3.exe`), and put the mod's folder or
   `.tome` into `tome/mods/`.
2. Start the game. Open **Mods** on the main menu (or Ctrl+F10) and turn the
   mod on. A mod with code asks once: it runs with your computer's full
   access, so Tome records its hash and asks again if a `.tome`'s code
   changes.
3. Watch `tome/tome.log`. Every line the mod logs is tagged with its id, and
   so are crashes, with the module and offset.
4. Change the code and rebuild into the mod's folder while the game runs.
   Tome reloads the mod, and `reload` in `tome.mod.json` decides when asset
   changes apply. In a game's chat, `/tome list`, `/tome enable <id>`,
   `/tome disable <id>` and `/tome reload` do the same as the menu.

For your own test setups:

- `TOME_HOME=<a Windows path>` keeps `tome/` (and so the mods) outside the
  game folder.
- `TOME_DEBUG_APPROVE=<ids>` (or `*`) allows mods' code without the menu.
- `-tomesafe` on the command line, or Shift held at launch, starts with
  every mod off.

## Rules worth knowing

- **Main thread.** Tome calls mods on the game's main thread: lifecycle,
  natives, events, UI, commands. Hooks are the exception, and run on the
  thread that called the game function. The game isn't thread-safe: change
  it only from those calls. Each function in `tome.h` says where it may be
  called.
- **Before sound starts.** The first mod load (each mod's `tome_mod_load`
  and `tome_mod_enable`) finishes before the game opens its sound device
  (Miles' `AIL_open_digital_driver`): Tome holds the game until then (at
  most 10 s). Code that must run before that, such as choosing the device,
  goes in `tome_mod_load`.
- **Crashes stay contained.** A fault in a mod's code, or a C++ exception or
  Rust panic the helpers catch, stops that call, logs where it happened, and
  disables the mod. The game keeps running. Threads a mod starts itself
  aren't guarded.
- **Multiplayer.** A mod that changes the game's simulation needs
  `"sync": "required"`, so every player runs the same version. Changing the
  game from an event on one client only desyncs the game.
- **No build-specific numbers.** Use `resolve` and list what you need in
  `"requires"`, so Tome refuses the mod on builds where a name isn't
  mapped. Don't hardcode addresses.
- **Versioned structs.** Structs Tome fills start with `size`. Check it
  before reading a field added in a later API version (`tome::has_field` in
  C++); the Rust API does this for you.
