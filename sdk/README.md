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
| [`include/tome/tome_test.h`](include/tome/tome_test.h) | Tests for mods without the game: Tome against a fake game ([below](#testing-a-mod)). |
| `bin/`, `lib/` | In the SDK zip: `tome-check.exe`, which checks each mod as it's built, and `tome_test.dll` for tests ([below](#testing-a-mod)). |
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
  function or an import of game.dll, Tome's or one the mod maps itself
  (`mappings/<build>.json`, `docs/tome-mod-json.md`).
- **Settings and keybinds** [7]: pages in the Mods menu, generated from the
  mod's files.
- **Chat commands** [5]: `/name ...` in games.
- **Other mods** [11]: offer and use tables of functions (services), and
  publish and subscribe to messages.
- **The mapping database** [2]: addresses of game functions and data by name
  (`resolve`), so a mod needs no build-specific numbers.
- **Game files** [14]: read any file as the game would, through its archives
  and the mods' assets (`read_game_file`).
- **Game objects** [16]: units, items, destructables and players. Read and
  change their properties by name (life, mana, position, facing, move speed,
  armor, damage, attack cooldown, hero attributes, ability levels, gold,
  lumber, food...) on every game version Tome supports, list every unit,
  item or destructable in the game, turn JASS
  handle ids into objects and back, and call any JASS native by name
  (`jass_call`: `GetUnitName`, `SetUnitFacing`, `CreateUnit`...). Tome checks
  each object is still there before it touches it. In a multiplayer game
  or a replay, only `"sync": "required"` mods may change properties or run
  natives.
- **The game's host** [17]: which player created the current game
  (`host_player`), the same on every player's computer, also in replays.
- **Messages between players** [18]: send up to 1 KiB to your mod on every
  player's computer (`net_send`, `on_net_message`), all at the same point
  of the game, the replay included.
- **Timers** [19]: run a callback after a delay, once or again and again,
  on real time (for UI, on every screen) or game time (`TOME_CLOCK_GAME`:
  follows the game speed and pauses, and fires in the same game step on
  every player's computer, so gameplay can use it). `timer_start`,
  `timer_stop`.
- **Desync checks** [19]: give Tome a hash of your mod's game state
  (`on_checksum`, `tome_hash_*`); in a multiplayer game it compares it
  between the players every few seconds and tells them which mod differs
  from whose computer, instead of the game dropping players without saying
  why.
- **Versions and the installed mods** [20]: compare versions and check
  ranges by Tome's own rules, the ones `tome.mod.json` is checked with
  (`version_compare`, `version_match`; mods' semver and game versions),
  list every installed mod with its version, state and why it isn't running
  (`list_mods`), hear when that list changes (`TOME_EVENT_MODS_CHANGED`),
  find the folder mods are installed in (`mods_dir`), and check a release's
  `tome.mod.json` before installing it, as Tome will check it
  (`check_metadata`): what an updater or a mod browser needs.
- **Your mod's time**: Tome times every call into your mod. The Mods menu's
  Details page shows its time a frame and its slowest call, `/tome perf`
  the busiest mods.

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

## Testing a mod

Two tools in the SDK zip run Tome itself (the code in `tome.mix`) against a
fake Warcraft III 1.27b, so they find in seconds what would otherwise take a
start of the game. Both are Windows programs: on Linux they run under Wine,
which the toolchain file finds. Building without them works as before; set
`TOME_CHECK=ON` to make them required, or `OFF` to skip the check.

**Checks at build time.** `tome_add_mod`, `tome_add_rust_mod` and
`tome_add_wasm_mod` run `tome-check` on the mod folder after each build
that changed it. It installs the mod in a Tome folder of its own, allows it,
loads and turns it on, binds its natives as a map load does, then turns it
off and unloads it, and reports what Tome found on the way:

```
my-mod: error: tome.mod.json: needs "CUnit.wings", which isn't mapped for 1.27b
my-mod 0.1.0: warning: native MyForgotten is in tome.mod.json, but the mod didn't register it (register_native in tome_mod_enable): it returns 0
my-mod 0.1.0: ok (native, 2 natives)
```

An error fails the build: `tome.mod.json` or the settings schema, a
`requires` name that isn't mapped, a missing DLL or one without
`tome_mod_load`, a `tome_mod_load` that refuses, a crash in load, enable or
disable, an unsafe icon. Every warning Tome logged (the mod's own included)
is printed. A mod that needs others gets them with `CHECK_WITH <their
targets or folders>`; `NO_CHECK` skips one. By hand: `tome-check [--strict]
[--edition roc|tft] <mod folder or .tome> [<the mods it needs>...]`
(`--strict` fails on warnings too; `--edition` picks the game it runs as,
by default the mod's own `"edition"`, else The Frozen Throne).

**Tests.** [`tome_test.h`](include/tome/tome_test.h) drives a built mod as
the game would: it starts Tome with the mod installed and allowed, then a
test calls its natives, sends chat commands, raises events, presses its
keybinds and changes its settings, and checks what the mod did, mostly by
its log lines. The test is a small C or C++ program; the mod can be in any
language. `tome_add_mod_test` builds it and adds it to CTest:

```cmake
enable_testing()
tome_add_mod_test(my_mod_tests SOURCES test_mod.c MODS my_mod)
```

```c
static void adds(void) {
    uint32_t args[2] = {2, 3}, result = 0;
    TOME_EXPECT(tome_test_install(TOME_TEST_MODS_DIR "/my-mod"));
    TOME_EXPECT(tome_test_start());
    if (!TOME_EXPECT_STATE("my-mod", "active")) return;
    tome_test_load_map();
    TOME_EXPECT(tome_test_call("MyModAdd", args, 2, &result));
    TOME_EXPECT(result == 5);
}

static const tome_test_case cases[] = {{"adds", adds}};
int main(int argc, char** argv) { return TOME_TEST_MAIN(argc, argv, cases); }
```

`ctest --test-dir build` runs it; each case runs in a process of its own,
and a failed one keeps its Tome folder (with `tome.log`) and prints where.
The C template has one ([`templates/c/test_mod.c`](templates/c/test_mod.c)),
and the hello example tests most of what it does
([`examples/hello/test_hello.c`](examples/hello/test_hello.c)).

The fake game has JASS natives (strings, reals and code values too), chat
commands, events (the test fills in their fields), keybinds, settings,
assets, hooks (they install; the game's functions never run) and the mod
lifecycle with its crashes. It hasn't the game's own natives, frames and
screens, popups, units or files: API functions that need them return an
error, as in the game before a map loads. That part stays a test in the
game.

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

## Legal

Tome is unofficial and has nothing to do with Blizzard Entertainment;
neither do your mods. Worth keeping in mind when you publish one:

- **Don't ship the game's files.** A mod's `assets/` may replace the game's
  files, but put only your own work (or work you may share) in it, not
  files copied out of the game's MPQs, edited or not.
- **Don't suggest Blizzard is involved.** Warcraft and Blizzard are
  Blizzard Entertainment's trademarks: say your mod is *for* Warcraft III,
  don't name or brand it as an official product, and don't use their logos.
- **Say what your mod does.** Players approve your code before it runs,
  trusting your description: no hidden features, nothing that collects data
  or reaches the network without saying so.
- **No cheats for online play.** A mod that gives one player an advantage
  others can't see or agree to (map hacks, automation) hurts the
  communities Tome is for. Tome's handshake only compares mods marked
  `"sync": "required"`; it can't stop a cheat, so don't make one.

Tome and the SDK come with no warranty (`LICENSE`); neither do mods, unless
their authors say otherwise.

## License

Your mod may use any license, open or closed, and owes the SDK no notice.
What a mod is built from is under MIT No Attribution (`LICENSE-MIT-0`),
which asks nothing of copies: the templates (they carry no license line,
so a mod started from one takes yours), the headers in `include/`,
`support/fpreset.c` and `support/tome_wasm.c`, and the Rust crates `tome`,
`tome-sys` and `tome-wasm`. The rest of the SDK (its CMake helpers, tools,
docs and examples) is under the MIT license (`LICENSE`). Tome itself, the
loader that runs mods, is under the GNU GPL v3 or later, and so are the
tools in the SDK zip's `bin/` and `lib/` (`tome-check`, `tome_test.dll`:
Tome's loader on a fake game); the mod you ship links neither. The zip's
`licenses/` has the GPL and the licenses of the third-party code in those
tools (`licenses/NOTICES.md`).
