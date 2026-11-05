# Building and testing Tome

This page is for people working on Tome itself. To install and use Tome,
see the [README](../README.md); to write a mod, see the
[SDK](../sdk/README.md).

## Building

Requires CMake, Ninja (or make) and a 32-bit MinGW-w64 toolchain
(Warcraft III is a 32-bit process); Wine runs the tests. On Fedora:

```sh
sudo dnf install cmake ninja-build mingw32-gcc-c++ wine xorg-x11-server-Xvfb
```

On Debian or Ubuntu: `sudo dpkg --add-architecture i386 && sudo apt install
cmake ninja-build g++-mingw-w64-i686 wine wine32 xvfb`.

The first configure downloads WAMR 2.4.5 (the WebAssembly interpreter, 6 MB,
checked against its SHA-256); offline, pass a copy of its archive:
`-DTOME_WAMR_ARCHIVE=<file>` (`loader/third_party/wamr/TOME.md`).

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

That's a release build (CI builds one too). The e2e test needs a developer
build: add `-DTOME_DEV=ON` (see "Environment variables" below).

Outputs:

- `build/loader/tome.mix`: the loader
- `build/mods/hello/`: the example mod, ready to copy into `tome/mods/`
- `build/packages/hello.tome`: the same mod as one file

Add `-DTOME_BUILD_DEV_TOOLS=ON` to also build `tome-mpq` (MPQ list/extract/
replace, used by the end-to-end test). It downloads StormLib at configure time.

## Installing a build

From a build: copy `tome.mix` into the Warcraft III folder (next to `war3.exe`) and put mods
in `tome/mods/` there (Tome creates the folders on its first start). To
remove Tome, delete `tome.mix`. What ends up there:

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

Copy `build/loader/tome.mix` into the game folder, or build and copy it in
one step:

```sh
make deploy INSTALL_DIR=/path/to/your/game   # or: -DTOME_INSTALL_DIR=... and --target deploy
```

There's no default: nothing copies into a game folder unless you name it.
`make game` and the tests run the build from a folder of their own and only
read the game's files from `input/` (see Testing).

Run the game (`wine war3.exe -window -opengl` on Linux). The window title
shows `[Tome <version>]` and `tome/tome.log` lists every mod and its state.

## Environment variables

The `TOME_DEBUG_*` variables are read only by developer builds: configure
with `-DTOME_DEV=ON` (`make` does; `make DEV=OFF` doesn't). Release builds
ignore them, since `TOME_DEBUG_APPROVE` would let anything that sets the
game's environment allow a mod's code. The e2e test needs a developer build;
the log's `Tome <version> starting` line says when it's one.

| Variable | Effect |
|----------|--------|
| `TOME_HOME` | Use this folder (a Windows path) for `tome/` (and so `tome/mods/`) instead of the game folder. |
| `TOME_DEBUG_APPROVE` | Allow these mods' code without turning them on in the menu: ids, comma-separated, or `*` for all. For tests. |
| `TOME_DEBUG_OPEN_MOD_MENU=1` | Open the mod menu automatically once the main menu is up (screenshots). |
| `TOME_DEBUG_UPDATE_URL` | The URL the update check asks instead of GitHub's (`http://` allowed), e.g. a local file server with a `{"tag_name": "v9.9.9"}` reply. Developer builds never ask or check without it, so tests don't see the first start's question. The release's `SHA256SUMS` and `.sig` are fetched from `<tag>/` next to it. |
| `TOME_DEBUG_RELEASE_KEY` | One more public key (base64) the update check accepts release signatures from, for a test server's releases. |
| `TOME_DEBUG_SKIP_MOVIES=1` | Skip each movie with Esc as it starts (a movie presents no frames, so `TOME_DEBUG_CLICKS` can't). Used by the e2e test. |
| `TOME_DEBUG_EXIT=1` | Exit the way the game's menu does (mods disabled and unloaded) once `tome/debug-exit` appears, so a `--coverage` build writes its counters (`tools/coverage.sh`). |
| `TOME_DEBUG_CLICKS` | Simulated input for tests, paced by the game rather than a clock: `click:FrameName` clicks a named button, `x,y;...` clicks at a position (fractions of the window), `type:text` types a chat line, `text:abc` types characters (no Enter), `key:Ctrl+J` presses a key, `wait:text` waits for a `tome.log` line containing the text, `until:text` repeats the last action until such a line, `frames:n` waits n frames, `settle` waits until no UI animation runs. See `loader/src/core/debug_script.h`. Used by the e2e test. |

## Testing

```sh
make test                          # unit tests, the loader without the game, and against game.dll (ctest, Wine)
make e2e                           # end-to-end in the real game (tools/e2e-test.sh, then tools/e2e-multiplayer.sh)
HEADLESS=0 make e2e                # the same, on your screen (the multiplayer part stays off screen)
make e2e-multiplayer               # only the two-instance LAN game
make game                          # the build in the game, to try things (tools/game.sh)
```

`make test` includes two deterministic suites that drive the real loader
in a test process, each test in a process of its own: `tome_host_tests`
against a fake game image (no game files), and `tome_game_tests` against the
real `game.dll` loaded but never run (skipped without the game in `input/1.27b/`). They take
seconds and give the same result every run; see `docs/architecture.md`,
"Testing".

The tests on `game.dll` and the end-to-end test need the files of a
Warcraft III 1.27b install in `input/1.27b/` (or `input/1.27b` as a link to
such a folder; `input/README.md`). They only read them. Other versions go
in their own folders and are picked by name: `make test 1.26a`. The game
runs as The Frozen Throne unless `roc` is named too (`make e2e roc`,
`make e2e 1.26a roc`: Reign of Chaos, `war3.exe -classic`). Each
launch runs from a temporary folder of links to those files, with the
build's `tome.mix`, a test map (made from one of your melee maps when
`tome-mpq` is built) and its own `tome/`, `save/` and `replay/`. The game runs in a Wine prefix of the
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

`tools/e2e-multiplayer.sh` then plays a LAN game between two instances of
the game, a host and a client, to test the multiplayer handshake: a lobby
whose required mods differ (Start refused), the client's mod fixed while it
waits, the game, the game lock, and the replay. The client gets a Wine
prefix of its own (`build/test-env/wine-client`), and each instance a
network namespace of its own inside an unprivileged user namespace
(`unshare -rn`): no root, and your own network and LAN games aren't
touched. Without unprivileged user namespaces (some containers) it says
it's skipped.

`make game` (`tools/game.sh start`) runs the build the same way, on your
screen, from `build/test-env/game/`; put mods to try in its `tome/mods/`.
`tools/game.sh shot file.png` takes a screenshot of the game's display.
