# Tome

A mod loader for Warcraft III: The Frozen Throne, in the spirit of Fabric for
Minecraft. Mods are built against Tome's stable API instead of each one
reverse-engineering the game.

Status: early development. Target build: **1.27b** (game.dll 1.27.1.7085).
See [`docs/research.md`](docs/research.md) for the design and roadmap.

What works today:

- **Drop-in loading:** `tome.mix` in the game folder is loaded by the game's
  sound library; no patched executables or injector.
- **Mods folder:** every `mods/<name>/` with a `tome.mod.json` is discovered,
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

Requires CMake, Ninja and a 32-bit MinGW-w64 toolchain (Warcraft III is a
32-bit process). On Fedora:

```sh
sudo dnf install cmake ninja-build mingw32-gcc-c++ wine
```

Configure and build:

```sh
cmake -S . -B build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-i686-mingw.cmake \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Outputs:

- `build/loader/tome.mix`: the loader
- `build/mods/hello/`: the example mod, ready to copy into `mods/`

Add `-DTOME_BUILD_DEV_TOOLS=ON` to also build `tome-mpq` (MPQ list/extract/
replace, used by the end-to-end test). It downloads StormLib at configure time.

## Installing

Copy `tome.mix` into the Warcraft III folder (next to `war3.exe`) and put mods
in a `mods/` folder there. To remove Tome, delete `tome.mix`.

```
Warcraft III/
  tome.mix
  mods/
    hello/
      tome.mod.json
      hello.dll
  tome/            created by Tome: tome.log, state.json, cache/
```

When the game folder is `./WarCraft III` (the default), this builds and copies
`tome.mix` in one step:

```sh
cmake --build build --target deploy
```

Use `-DTOME_GAME_DIR=/path/to/Warcraft III` to point it somewhere else.

Run the game (`wine war3.exe -window -opengl` on Linux). The main menu gets a
**Mods** button, the window title shows `[Tome 0.1.0]`, and `tome/tome.log`
lists every mod and its state. If the game
build isn't supported, Tome logs an error and stays idle, and the game runs
unmodified.

## Writing a mod

A mod is a folder with a `tome.mod.json` and, optionally, a 32-bit DLL. See
[`sdk/examples/hello`](sdk/examples/hello) for a complete example, and
[`sdk/include/tome/tome.h`](sdk/include/tome/tome.h) for the API.

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

The DLL exports `tome_mod_load` (required) plus optional `tome_mod_enable`,
`tome_mod_disable` and `tome_mod_unload`. Register natives in
`tome_mod_enable`; Tome detaches them automatically on disable. A map uses a
native by declaring it at the top of its script, exactly as in the metadata.

`tome_add_mod()` (from `sdk/TomeMod.cmake`) builds a mod and assembles its
folder under `build/mods/`. A mod in its own repository includes that file
(`include(<tome>/sdk/TomeMod.cmake)`); `fps-unlock`, which lifts the frame
rate above 60, is built that way and lives next to this repo.

## Environment variables

| Variable | Effect |
|----------|--------|
| `TOME_HOME` | Use this folder (a Windows path) for `mods/` and `tome/` instead of the game folder. |
| `TOME_DEBUG_OPEN_MOD_MENU=1` | Open the mod menu automatically once the main menu is up (screenshots). |
| `TOME_DEBUG_CLICKS` | Post left clicks to the game window: `x,y@ms;...`, with x/y as fractions of the window; `type:text@ms` types a chat line, `text:abc@ms` types characters (no Enter), `key:Ctrl+J@ms` presses a key. Used by the e2e test. |

## Testing

```sh
cd build && ctest                  # unit tests (run under Wine)
tools/e2e-test.sh                  # end-to-end in the real game
HEADLESS=1 tools/e2e-test.sh       # the same, off screen
```

With `HEADLESS=1` (for `tools/e2e-test.sh` and `tools/game.sh`), the game
runs inside [gamescope](https://github.com/ValveSoftware/gamescope)'s headless
backend: no window appears, and simulated key presses don't depend on the
game window having the focus. The virtual screen takes the game's resolution
setting (or `HEADLESS_SIZE=1280x720`); `tools/game.sh shot` then captures it
through `gamescopectl`.

The end-to-end test launches the game three times (about a minute each). It
runs Tome with `TOME_HOME` set to a temporary folder, so your own `mods/` and
`tome/` are never touched; it only writes `tome.mix` and, temporarily, a test
map under `Maps/TomeE2E/`. With `tome-mpq` built, the test map (made from one
of your melee maps) checks the JASS natives from inside a running map. The
last launch clicks through the mod menu. Close the game before running it.
