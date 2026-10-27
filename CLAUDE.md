# Tome: notes for working in this repo

Tome is a Fabric-style mod loader for Warcraft III **1.27b** (game.dll
1.27.1.7085, 32-bit). Start with `docs/ROADMAP.md` (what's next), then
`docs/architecture.md` (how it works) and `docs/reverse-engineering.md`
(game internals, addresses, methods). `docs/research.md` is the original
design document.

## Commands

```sh
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-i686-mingw.cmake \
      -DCMAKE_BUILD_TYPE=Release -DTOME_BUILD_DEV_TOOLS=ON
cmake --build build --target deploy    # build and copy tome.mix into "WarCraft III/"
cd build && ctest                        # unit tests (Wine)
tools/e2e-test.sh                        # real game, 4 launches (~5 min); close the game first
tools/game.sh start [VAR=val] / wait <regex> / shot <png> / stop / log
```

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
- Code style: C++20, no exceptions/RTTI, `TOME_INFO`/`WARN`/`ERROR` logging.
  The mod API in `sdk/include/tome/tome.h` is plain C and append-only.

## Pitfalls (each one cost time already)

- **Never kill the game with `pkill -f`/`pgrep -f` and a pattern.** It also
  matches your own shell. Use `pkill -x war3.exe` (or `tools/game.sh stop`).
  SIGTERM sometimes isn't enough, so follow up with `-9`.
- **Only one game instance at a time.** A second one can't open `tome.log`.
- **Tests must use `TOME_HOME`** (a temp folder), never the real `mods/` folder.
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
- **Screenshots:** `spectacle -b -n -f -o file.png` (KDE). Simulate clicks with
  `TOME_DEBUG_CLICKS="x,y@ms;..."` (fractions of the window).
- **Check `ret N` before mapping a function's `sig`.** A hook pops what
  the signature says; one stack argument too few crashed the game.
- **Keys in tests:** `TOME_DEBUG_CLICKS` items `key:Ctrl+J@ms` press a key
  through `keybd_event` (real modifier state); the window needs the focus.
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
  from a 12 s timer. A trigger's `PauseGame` clears the selection, and a
  single player game pauses when its window loses the focus (timers stop).
- **Saved games from scripts:** `SaveGame`/`LoadGame` names need `.w3z`
  (nothing adds it, and a load without it silently goes back to the menu).
  The e2e test removes its save from `save/<profile>/CustomSaves/e2e`.
- **Leaving a game:** Quit Mission → score screen → OK returns to the Custom
  Game screen (two Cancels to the main menu), and InitNatives runs once more
  on the way out. The click path is in `tools/e2e-test.sh` run 4.
