# Tome SDK

Everything for writing Tome mods for Warcraft III 1.27b: the API, build
helpers, examples and starting templates in C, C++ and Rust.

| | |
|---|---|
| [`include/tome/tome.h`](include/tome/tome.h) | The API, in C. Every function is documented there. |
| [`include/tome/tome.hpp`](include/tome/tome.hpp) | C++17 helpers: lambdas for callbacks, exceptions reported as crashes ([guide](examples/hello-cpp/README.md)). |
| [`include/tome/utf8.h`](include/tome/utf8.h) | UTF-8 helpers (JASS strings are UTF-8). |
| [`rust/`](rust/README.md) | Rust: raw bindings (`tome-sys`) and a safe API (`tome`). |
| [`include/tome/tome_wasm.h`](include/tome/tome_wasm.h) | **Experimental:** sandboxed WebAssembly mods in C ([below](#webassembly-mods)). |
| [`templates/`](templates) | The smallest useful mod in [C](templates/c), [C++](templates/cpp), [Rust](templates/rust) and WebAssembly ([C](templates/wasm), [Rust](templates/rust-wasm)): copy one to start. |
| [`examples/`](examples) | The same full example in [C](examples/hello), [C++](examples/hello-cpp) and [Rust](rust/examples/hello-rs); a WebAssembly one in [C](examples/hello-wasm) and [Rust](rust/wasm/examples/hello-wasm-rs). |
| [`docs/tome-mod-json.md`](docs/tome-mod-json.md) | `tome.mod.json` and settings, field by field. |
| [`schemas/`](schemas) | JSON Schemas for both files, for your editor. |
| [`TomeMod.cmake`](TomeMod.cmake), [`cmake/`](cmake) | `tome_add_mod()`, `tome_add_rust_mod()`, `tome_package()`; the MinGW toolchain. |
| [`support/`](support) | Linked into C and C++ mods (`fpreset.c`); `make_blp.py` makes a placeholder icon (`make_blp.py icon.blp 32 3070d0`); `tome_sign.py` signs mods ([below](#sign-your-releases)). |

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
   mod on. A new mod asks once (a DLL runs with your computer's full access,
   and the game's loaders trust assets), so Tome records its hash and asks
   again if a `.tome`'s files change, unless the new version is signed with
   the same key ([below](#sign-your-releases)).
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
  Only a developer build of Tome reads it (`-DTOME_DEV=ON`, the default of
  `make` in Tome's repository); released builds ignore it.
- `-tomesafe` on the command line, or Shift held at launch, starts with
  every mod off.

## WebAssembly mods

**Experimental.** A mod can be a WebAssembly module instead of a DLL. It runs
sandboxed inside Tome: it can only call Tome's functions, reads and writes
only its own memory, and gets handles instead of pointers. A bug in it
disables the mod instead of crashing the game. Players still allow it once,
but they're told it "runs sandboxed code", not that it gets full access to
their computer.

It can use almost all of the API: settings, chat commands, game events,
screens, frames and clicks, keybinds, popups, the selection, messages
between mods, JASS natives and code values, the game's files, and files of
its own (`tome_config_read`/`tome_config_write` instead of a folder path).
It can't hook game functions, resolve addresses or reach memory: if a mod
needs those, it's a DLL mod.

```sh
cp -r templates/wasm my-mod && cd my-mod
cmake -S . -B build -DTOME_SDK_DIR=<sdk>    # no MinGW, no toolchain file
cmake --build build                         # build/mods/my-wasm-mod/, build/packages/my-wasm-mod.tome
```

It needs clang (any recent version) and a WebAssembly linker: LLVM's
`wasm-ld`, or the `rust-lld` that comes with every Rust toolchain (found in
`~/.rustup` by itself). There's no libc: `tome_wasm.h` has what a mod needs
(`tome_append`, `tome_append_int` for text), and the runtime
(`support/tome_wasm.c`, compiled in by `tome_add_wasm_mod`) provides
`memcpy` and `memset`. C++ works without exceptions, RTTI or a standard
library.

The differences from DLL mods, in short (all in `tome_wasm.h`):

- callbacks are plain functions with a `void* user`, registered with the
  wrappers (`tome_register_command`, `tome_on_event`, ...); data they get is
  valid during the call;
- frames are `tome_frame_t` handles, looked up by name on every use: one for
  a frame that's gone just fails;
- natives get one 32-bit word per argument (`tome_real`, `tome_string`) and
  return one (`tome_real_bits`, `tome_string_result`); a handle result must
  be one of the call's own arguments;
- each call into the mod may run about half a second (200M instructions);
  longer is a trap.

In Rust, the `tome-wasm` crate ([`rust/wasm`](rust/wasm/tome-wasm)) is the
same with closures, `Result`s and `String`s, and needs nothing but
`rustup target add wasm32-unknown-unknown`:

```sh
cp -r templates/rust-wasm my-mod && cd my-mod   # point tome-wasm at the SDK in Cargo.toml
cargo build --release                            # target/wasm32-unknown-unknown/release/my_rust_wasm_mod.wasm
```

A panic is a trap (logged first), so it disables the mod like a crash.
[`rust/README.md`](rust/README.md#webassembly-mods) has more.

## Sign your releases

Players allow each new mod once, and a `.tome` again whenever its files
change. Signed releases skip the second prompt: when a player allowed a
version signed with your key, a later one signed with the same key is
allowed without asking. One signed with another key, or not at all, asks
with a warning that it may not be from the same author. The mod's details
page shows the key ("Signed by").

```sh
python3 support/tome_sign.py keygen ~/my-mods.key   # once; prints the public key
```

Keep the key file out of your repository and back it up: losing it means
players are asked again (and warned) for your next release. Publish the
public key where players get the mod, so they can compare it.

Then sign the package in CMake:

```cmake
tome_package(my_mod SIGN_KEY $ENV{HOME}/my-mods.key)
```

or a folder by hand, after the last change to any file in it
(`tome_sign.py sign <folder> <key>`; `tome_sign.py verify <folder>` checks
one). The signature covers every file and `tome.mod.json`, so editing
anything afterwards breaks it, and Tome treats the mod as unsigned. Mod
folders in `tome/mods/` don't need signing: a folder never asks again once
allowed, so rebuilding into it keeps hot reloading.

A signature says who made a mod, not that it's safe.

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
- **Your first `enable` may run on another thread.** At startup Tome loads
  and enables mods on a thread of its own that ends once the game runs;
  after that, everything runs on the game's main thread, one call at a
  time. Don't keep callbacks or their state in thread-local storage (it's
  freed when that thread ends); the C++ and Rust helpers don't.
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

## License

The SDK (everything in this folder) is under the MIT license (`LICENSE`).
Your mod may use any license, open or closed. Tome itself, the loader that
runs mods, is under the GNU GPL v3.
