# Tome

A mod loader for Warcraft III: The Frozen Throne, in the spirit of Fabric for
Minecraft. Mods are built against Tome's stable API instead of each one
reverse-engineering the game.

Status: early development. Target build: **1.27b** (game.dll 1.27.1.7085).
See [`docs/ROADMAP.md`](docs/ROADMAP.md) for what's next and
[`docs/research.md`](docs/research.md) for the design.

What works today:

- **Drop-in loading:** `tome.mix` in the game folder is loaded by the game's
  sound library; no patched executables or injector.
- **Mods folder:** every `tome/mods/<name>/` with a `tome.mod.json` is discovered,
  validated and dependency-resolved; problems are reported per mod in the log.
- **Live toggling:** enable/disable mods by editing `tome/state.json` while the
  game runs. Changes apply on the game's main thread within about a second.
- **Hot reload:** rebuild a mod's DLL while the game runs; Tome reloads it.
- **Custom JASS natives:** mods add natives that map scripts can call. While a
  mod is disabled its natives return 0, so maps that use them keep working.
- **Mod menu:** a native-looking "Mods" button on the main menu and in the
  in-game menu (F10) opens a panel listing every mod with its status and an
  Enable/Disable button, details and problems (including a crashed mod's
  log line), settings pages generated from each mod's `config.schema.json`,
  everyone's keybinds (Ctrl+F10 opens the menu) and a safe-mode switch. It's built from the game's own
  UI frames; no game files are modified.

## Building

Requires CMake, Ninja (or make) and a 32-bit MinGW-w64 toolchain
(Warcraft III is a 32-bit process); Wine runs the tests. On Fedora:

```sh
sudo dnf install cmake ninja-build mingw32-gcc-c++ wine xorg-x11-server-Xvfb
```

On Debian or Ubuntu: `sudo dpkg --add-architecture i386 && sudo apt install
cmake ninja-build g++-mingw-w64-i686 wine wine32 xvfb`.

The `Makefile` wraps the usual steps (`make help` lists them all):

```sh
make            # configure (once) and build into build/
make test       # unit tests
make e2e        # the end-to-end test in the real game (see Testing)
```

Or with only a container engine (podman or docker), nothing else installed:
`make container-test` / `make container-e2e` build an image with the whole
toolchain, Wine and Xvfb (`tools/container/Containerfile`) and run the same
targets in it, building into `build-container/`.

By hand, configure and build:

```sh
cmake -S . -B build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-i686-mingw.cmake \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

That's a release build. The tests need a developer build: add `-DTOME_DEV=ON`
(see "Environment variables").

Outputs:

- `build/loader/tome.mix`: the loader
- `build/mods/hello/`: the example mod, ready to copy into `tome/mods/`
- `build/packages/hello.tome`: the same mod as one file

Add `-DTOME_BUILD_DEV_TOOLS=ON` to also build `tome-mpq` (MPQ list/extract/
replace, used by the end-to-end test). It downloads StormLib at configure time.

## Installing

Copy `tome.mix` into the Warcraft III folder (next to `war3.exe`) and put mods
in `tome/mods/` there (Tome creates the folders on its first start). To
remove Tome, delete `tome.mix`.

```
Warcraft III/
  tome.mix
  tome/            tome.log, state.json, keybinds.json, cache/, packages/
    mods/
      hello/
        tome.mod.json
        hello.dll
      other.tome   a packaged mod
      config/      written by Tome
        hello.json   the hello mod's settings
        hello/       files the mod keeps (config_dir in the API)
```

A mod with code (a DLL) runs native code with full access to your computer,
and one whose assets include game scripts (`.j`, `.ai`, `.pld`) runs JASS in
every game, so it stays off until you turn it on once in the Mods menu (or with
`/tome enable <id>` in chat); its details page shows a SHA-256 of its code.
A `.tome` package whose code changes asks again; a mod folder doesn't, so
rebuilding a mod into `tome/mods/` keeps hot reloading.

A mod can also come as one file, `<name>.tome` (a zip of its folder): put it
in `tome/mods/` as it is. Tome unpacks it into `tome/packages/` and reloads
the mod when the file changes. `tome_package()` makes one when building
(`build/packages/hello.tome`).

An older install's `mods/` folder next to `war3.exe` is moved to `tome/mods/`
on the first start, with each mod's `config/settings.json`.

When the game folder is `./WarCraft III` (the default), this builds and copies
`tome.mix` in one step:

```sh
make deploy          # or: cmake --build build --target deploy
```

Use `make deploy GAME_DIR=/path/to/Warcraft III` (or
`-DTOME_GAME_DIR=...`) to point it somewhere else. Nothing else copies into
your install: `make game` and the tests run the build from a folder of their
own (see Testing).

Run the game (`wine war3.exe -window -opengl` on Linux). The main menu gets a
**Mods** button, the window title shows `[Tome 0.1.0]`, and `tome/tome.log`
lists every mod and its state. If the game
build isn't supported, Tome logs an error and stays idle, and the game runs
unmodified.

## Writing a mod

Start with the SDK: [`sdk/README.md`](sdk/README.md). It covers what a mod
is and can do, templates to copy in C, C++ and Rust, a full example in each,
the [`tome.mod.json` reference](sdk/docs/tome-mod-json.md), and how to try
a mod in the game.

A mod is a folder with a `tome.mod.json` and, optionally, a 32-bit DLL:

```json
{
  "schema": 1,
  "id": "hello",
  "version": "1.0.0",
  "game": "1.27b",
  "tome": ">=0.1.0 <0.2.0",
  "entry": "hello.dll",
  "sync": "client",
  "natives": ["native HelloLog takes integer value returns nothing"]
}
```

The SDK builds without the rest of this repository too:
`cmake --build build --target sdk-dist` makes `build/tome-sdk-<version>.zip`.
`enhanced-options`, which adds what the game's Options screen is missing
(window modes, refresh rate, frame rates above 60 with vsync and an FPS
counter), is a mod built against the SDK in its own repository next to
this one. It replaces the earlier `fps-unlock`.

## Environment variables

The `TOME_DEBUG_*` variables are read only by developer builds: configure
with `-DTOME_DEV=ON` (`make` does; `make DEV=OFF` doesn't). Release builds
ignore them, since `TOME_DEBUG_APPROVE` would let anything that sets the
game's environment allow a mod's code. The tests need a developer build;
the log's `Tome <version> starting` line says when it's one.

| Variable | Effect |
|----------|--------|
| `TOME_HOME` | Use this folder (a Windows path) for `tome/` (and so `tome/mods/`) instead of the game folder. |
| `TOME_DEBUG_APPROVE` | Allow these mods' code without turning them on in the menu: ids, comma-separated, or `*` for all. For tests. |
| `TOME_DEBUG_OPEN_MOD_MENU=1` | Open the mod menu automatically once the main menu is up (screenshots). |
| `TOME_DEBUG_CLICKS` | Simulated input for tests, paced by the game rather than a clock: `click:FrameName` clicks a named button, `x,y;...` clicks at a position (fractions of the window), `type:text` types a chat line, `text:abc` types characters (no Enter), `key:Ctrl+J` presses a key, `wait:text` waits for a `tome.log` line containing the text, `until:text` repeats the last action until such a line, `frames:n` waits n frames, `settle` waits until no UI animation runs. See `loader/src/core/debug_script.h`. Used by the e2e test. |

## Testing

```sh
make test                          # unit tests, the loader without the game, and against game.dll (ctest, Wine)
make e2e                           # end-to-end in the real game (tools/e2e-test.sh)
HEADLESS=0 make e2e                # the same, on your screen
make game                          # the build in the game, to try things (tools/game.sh)
```

`make test` includes two deterministic suites that drive the real loader
in a test process, each test in a process of its own: `tome_host_tests`
against a fake game image (no game files), and `tome_game_tests` against the
real `game.dll` loaded but never run (skipped without `GAME_DIR`). They take
seconds and give the same result every run; see `docs/architecture.md`,
"Testing".

The end-to-end test needs a Warcraft III 1.27b folder: `./WarCraft III`, or
`GAME_DIR=/path/to/it`. It only reads it. Each launch runs from a temporary
folder of links to the install's files, with the build's `tome.mix`, a test
map (made from one of your melee maps when `tome-mpq` is built) and its own
`tome/`, `save/` and `replay/`. The game runs in a Wine prefix of the
test's own, made once in `build/test-env/wine` with fixed settings (800x600,
a profile called Tome), so it looks the same on every machine whatever your
own prefix's settings are.

By default the game runs in its own X server (Xvfb) with Mesa's software
OpenGL and with its sound going to ALSA's null device: no window appears,
nothing plays, no GPU or sound server is needed, the game keeps the
keyboard focus whatever you do, and its simulated clicks don't move your
mouse (`HEADLESS=0` and `SOUND=1` change that). Nothing in the test is
timed: the simulated input (`TOME_DEBUG_CLICKS`) waits for log lines (a
screen being ready, a page of the mod menu, a mod's reply), counts the
game's frames and waits for the UI to stop moving, and the checks wait for
their log lines, so a slow machine only makes the test slower. The test
launches the game six times.

`make game` (`tools/game.sh start`) runs the build the same way, on your
screen, from `build/test-env/game/`; put mods to try in its `tome/mods/`.
`tools/game.sh shot file.png` takes a screenshot of the game's display.

## License

Tome itself is under the GNU GPL v3 (`LICENSE`). The SDK in `sdk/` (the mod
API headers, CMake helpers, templates, examples and Rust crates) is under the
MIT license (`sdk/LICENSE`), so mods built against it may use any license.

Tome is not affiliated with or endorsed by Blizzard Entertainment. It
includes no game files; you need your own copy of Warcraft III.
