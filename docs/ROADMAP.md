# Tome roadmap

Status as of 2026-10-30. Design rationale: `research.md`. How it works:
`architecture.md`. Game internals: `reverse-engineering.md`.

## Done

| Milestone | Notes |
|-----------|-------|
| 1. Loader | `tome.mix` via Miles; build identification by SHA-256; idle on unknown builds |
| 2. Mod loading | `tome.mod.json` schema, dependency resolution, `state.json`, shadow-copied DLLs |
| 3. JASS natives | Mods add natives; disabled mods' natives return 0; real/integer ABI verified |
| 4 (part). Safe point | Lifecycle on the main thread via a window-message dispatcher; live toggling, hot reload |
| Mapping database | `mappings/1.27b.json` embedded in `tome.mix`; `resolve` in the mod API (version 2); `requires` checked before loading |
| Byte signatures | `bytes` on every function (mapping schema 2) plus call-site checks, verified before patching and in unit and e2e tests; a mismatch idles Tome and logs the symbol |
| 8 (part). Mod menu | Native Mods button sharing the bottom box with Quit; chained panel with Enable/Disable and paging |
| Frame API for mods | API version 3: `on_ui` (main menu built), `load_fdf` (from the mod folder), `load_toc`, get/create, SetPoint, SetText, Show/Hide, sprite animation, click callbacks on main menu buttons. JASS: `BlzGetFrameByName`, `BlzCreateFrame`, `BlzFrameSetPoint`, `BlzFrameSetText`, `BlzFrameSetVisible`, `BlzLoadTOCFile` (integer frame ids, not `framehandle`). Reads JASS string arguments. The hello example adds a main menu button |
| 5. Asset overlay | Enabled mods' `assets/` folders merged in load order into one archive at Storm priority 12 (above War3Patch.mpq, below maps), mounted from DllMain and swapped live; `reload` live/map/restart decides when changes apply. `load_fdf` goes into Tome's generated overlay. e2e: texture and `Blizzard.j` overrides, TOC/FDF from assets, live toggle, a menu → game → menu cycle for `map`/`restart` |
| General hook manager (§3.2) | API version 4: `hook`/`unhook`/`hook_call_next` on mapped functions. One inline patch per function (prologue moved by a small x86 decoder, bytes checked first), one chain per function: before/replace/after with priorities, revoked on disable, and a mod's DLL is freed only once none of its handlers runs. Layouts from the mapping's `conv` and `sig`, including float/double results, 64-bit and variadic arguments; short branches in the moved bytes are widened. Unit tests on real asm functions in every convention; e2e: two mods hook `RCString::GetText` and both run |
| Events (§3.2, §5.8) | API version 5: `on_event` for map load, game start (also for mods enabled mid-game), game end (also when the program exits, followed by every mod's disable and unload), a per-frame tick and a game-time tick (simulation steps; follows game speed and pause), with the map's name and path; `on_ui` for every menu screen, the loading and score screens and the in-game UI, with `ui_root`; `frame_on_click` on any screen and in games (through a `CObserver` of Tome's own); chat commands (`register_command`, `display_text`) and `/tome list\|enable\|disable\|reload`; `reload: map` assets also apply when the next map loads. All from Tome's own handlers on mapped functions through the hook manager. e2e: events on both start paths and in a LAN game, clicks on three screens, chat commands in a game; in-game clicks hand the keyboard back to the chat box |
| Native strings and code | API version 6: `jass_string` reads string arguments, `jass_string_create` makes string results (ids in the script's string table, through the game's own `CreateJassString`; invalid UTF-8 becomes U+FFFD), `jass_code_call` runs `code` arguments synchronously and returns their boolean (through `Condition`/`TriggerEvaluate`), `jass_code_execute` runs them as action threads that may wait (`TriggerExecute`, one trigger per code value), from natives and later from events; only code values the current map script passed to a Tome native are accepted (the thunks record them). `tome/utf8.h`: decode/encode, validate, length and offsets by code point, sanitize, UTF-16 conversion. Built-in `TomeLogString`; the hello example adds `HelloGreet`, `HelloCall`, `HelloRun`, `HelloOnGameStart`. e2e: string arguments and results (UTF-8, null), callbacks inside a native and at game start, waiting threads, an unknown code value refused |
| Crash isolation and safe mode (§5.7, §9) | Every call into a mod (lifecycle, natives, hooks, events, commands, UI callbacks, clicks) runs inside an SEH frame of Tome's own (MinGW has no `__try`): a fault unwinds the frames above it, the call is abandoned (natives return 0, a crashed REPLACE handler passes on), the log names the module and offset, and the mod is disabled with its dependents, its DLL kept loaded, until its DLL changes or it's turned off and on. Safe mode: `-tomesafe`, Shift held at launch or `tome/safe` start with no mods; mods can still be enabled one at a time. e2e: crashes in a native, a tick handler, a REPLACE hook, enable and load, with the game running on; a safe-mode launch |
| 8. Mod menu, part 2 | API version 7. A Details page per mod (metadata, status, every problem in full: metadata errors, a crash's log line and "called from" line, schema errors, warnings); per-mod Settings pages generated from `config.schema.json` (boolean, integer, number with range and step, choices, free text in a text box), saved in `<mod>/config/settings.json`, read and changed through `setting_*`/`set_setting_*`, `on_setting_changed`; Keybinds from `tome.mod.json` `"keybinds"`, one page for all mods with conflicts (each other and the game's known keys), saved in `tome/keybinds.json`, `on_keybind` (not while typing in chat), checked against `CustomKeys.txt` too; mod icons (`.blp`/`.tga`) on the list; Esc goes back a page; Tome's own keybind (Ctrl+F10) opens the menu, in games the Esc menu with Mods; a Tome page with versions, mods folder and a live safe-mode switch; the same pages in the in-game Esc menu (a Mods button beside Options, the panel in the Help panel's place). e2e: a setting, a typed text setting, a keybind set with simulated keys and run, the icon, Esc navigation, Ctrl+F10 on the main menu and in a game, the Esc menu's Mods and Done |
| More events | API version 8: `TOME_EVENT_SELECTION` (the local player's selection, compared each frame: every unit's type id, name, owner, building/hero/dead flags and its existing JASS handle id, which Tome looks up without ever making one, and the active unit), `TOME_EVENT_CHAT`, `TOME_EVENT_PAUSE`/`RESUME`; unit events for every unit whether or not the map listens (death with the killer, trained with the building, construction, upgrade, research, hero level, summon, item pickup/drop/use, attacked, rescued, construction/upgrade/training/research started and cancelled, hero skill, revivable and revival, orders of every kind with their target, loaded into a transport, units and items sold, items pawned, owner change), player victory/defeat/left, game save and saved game loaded with the path; `map_description`; `selection()`. e2e: a map script selects, pauses, levels a hero, uses items, kills, trains and cancels, orders, attacks, loads a transport, gives a unit away, revives, builds, upgrades, saves and loads, wins; the Esc menu's pause, LAN chat |
| Multiplayer handshake (§4) | API unchanged. Required mods are identified by `id version #hash` (tome.mod.json, DLL, assets). Lobby: the host sends its set to each joining player as hidden chat lines, Tome clients answer, the host refuses Start while a set differs or is missing, and each side gets notes in the lobby chat. Games: each player sends its set as SyncStoredInteger actions, so replays carry it; after 2 s every client (and a replay viewer) says who differs. Unit tests for the protocol, comparisons and lobby state; verified by hand in LAN games between two instances (matching, a refused Start, the in-game report, a replay) |
| Game lock (§5.7) | API unchanged. In a game with more than one person playing, or a replay, the running required mods and their dependencies stay as they were (no toggles, hot reloads or asset changes; only a crash stops one) and other required mods wait; toggles go into `state.json` and apply when the game ends. Notes in the mod list and `/tome` replies. Unit tests for the locked set, the notes and frozen assets; verified by hand in a replay (toggles wait, then apply on leaving); not yet in a two-instance game (part of next step 1) |
| Import hooks; FPS Unlock | API version 9: `import` symbols (an import address table slot of game.dll, checked against its import directory) can be hooked like functions. `fps-unlock`, a mod in its own repository next to Tome: a finer GetTickCount for game.dll (Windows' 15.6 ms steps held frames near 64), the frame limit through the game's own scheduler, vsync off for OpenGL and Direct3D, an FPS counter in a chosen corner; switching it on and off mid-game keeps the game's clock monotonic (checked: game speed stays 1.00) |
| Popups and restarting | API version 10: `popup` (title, message, any number of buttons; Enter/Esc options), `popup_close`, `restart_game`. One at a time on any screen (main menu, menu screens, games), queued, moved to the next screen; three short options in a row, more in pages of six. Restart: the game closes as usual (in a game, through the Esc menu's own Exit), a rundll32 helper starts it again once it has exited. The mod menu offers a restart when a toggle leaves changes waiting for one. The hello example's Ctrl+Shift+P shows them. Unit tests for layout, keys, FDF and the prompt; verified by hand on the main menu, over the mod menu, in a game, and a full restart |
| Inter-mod communication (§5.9 item 5) | API version 11: `service_register`/`service_get`/`on_service` (a mod offers a table of functions under a name and a version; others look it up, and hear when it appears or goes away in any load order; withdrawn on disable) and `subscribe`/`publish` (synchronous messages on named topics from the main thread; a subscriber that crashes is disabled and skipped; no loops on one topic). Registry in `core/intermod.*` with unit tests. Not yet in the e2e test or an example mod |
| Events, part 3 | API version 12: the remaining JASS events Tome can see for every unit and player whether or not the map listens. Spell channel, cast, effect, finish and end cast (`CUnit::OnSpell*`, with the ability and its target unit, item or destructable and point), damage with the amount and source, acquired targets, decay, detected and hidden, Esc and arrow keys (every player's, from the network actions; arrow keys only on maps that listen for them, the game doesn't send them otherwise), destructable and item deaths (`CWidget::OnDeath`), dialog clicks with the dialog's and button's handles; order targets that are destructables; `item_handle`/`destructable_handle`. e2e: spells, damage, a destructable and an item destroyed, a creep's acquired target, decay, a dialog click and an arrow key with simulated keys; Esc on a melee map |
| Events, part 4 | Still API version 12: the skill and build menus (`EVENT_GAME_SHOW_SKILL`, `BUILD_SUBMENU`, from their network actions, with the player), level end, units created (every unit, from `CreateUnitObject`) and removed (`RemoveUnit`), hero experience (the amount actually added), player resources (gold, lumber, food; polled every game tick) and day and night. e2e: B and O on a selected worker and hero, `RemoveUnit`, `AddHeroXP`, `SetPlayerState`, the time of day set across dusk and back to noon, `LoadGame` for the level end |
| Memhack guard | Built-in patch, not a mod: closes the 1.27b local/global name-shadowing typecast in the JASS compiler (`docs/known-bugs.md`). The map script is also scanned at map load: a warning in `tome.log`, a message to the player at game start, and API version 13 for mods (`tome_event_info.script_shadows`, `map_script_shadows`). e2e: shadowing locals keep their type to `endfunction`, the global gets its own back, the scan, the message and the hello example's report |
| Path sanitizer | Built-in patch (`docs/known-bugs.md`): refuses path traversal in all 38 natives that take a file path: the writes (`PreloadGenEnd`, `InitGameCache`, `SaveGame`, `CopySaveGame`, `RenameSaveDirectory`, `RemoveSaveDirectory`), rooted in the game folder on 1.27b so a map-controlled name could plant a file anywhere, and the reads (`Preloader`, `LoadGame`, `ChangeLevel`, AI scripts, models, sounds, music, images). Tome's `BlzLoadTOCFile` also checks its TOC and the FDF paths in it. Hooks `BindNative` and swaps in a sanitizing wrapper keyed off the native's name, so no write-native address is hardcoded. Portable decision logic unit-tested in `core/path_guard.*` |
| BLP texture guard | Built-in patch (`docs/known-bugs.md`): the 1.27b BLP1 loader reads mip spans and a JPEG texture's header out of the file using sizes from the file's own header without bounds-checking them, so a crafted `.blp` (a downloaded map's or a mod's) makes it read past the buffer -- a crash and an out-of-bounds read. Hooks `CBLPFile::Open` and, after it loads a file, validates the bytes (`core/blp.*`: magic, dimensions, the 16 mip offset/size slots, the JPEG-header size); an unsafe file is failed like a wrong-magic one, so it isn't decoded. Reserves the hook. Unit-tested and fuzzed; e2e feeds the menu a malformed mod icon and checks it's refused without a crash. Covers maps and mods |
| Approval of mod code | A mod with a DLL doesn't run (or even load) until the user turns it on once in the mod menu or with `/tome enable`, which records a SHA-256 of its files (everything but `tome.mod.json`) in `state.json` (schema 3). An asset-only mod counts too: game scripts (`.j`, `.ai`, `.pld`) run in every game, and other assets and the icon go through the game's own parsers, which trust them; nothing of an unapproved mod is mounted or decoded. A changed `.tome` asks again; a folder mod in its own folder hot-reloads without asking. Mods from an older `state.json` stay allowed if they ran then, and a schema 2 approval moves over if the code is the same. The menu says "New code" / "Code changed" ("New files" for assets) and shows the hash. `core/mod_state.*`, unit-tested; host tests for asset mods and the schema 2 upgrade; e2e: an unapproved DLL fixture is never loaded, an unapproved script-only one isn't mounted; `TOME_DEBUG_APPROVE` for tests |
| WebAssembly mods (experimental) | `"wasm"` in `tome.mod.json`: a module run sandboxed by WAMR 2.4.5's interpreter (downloaded at configure time, hash-checked, three MinGW patches), with only Tome's imports, each checked (`core/wasm_abi`); data copied into its memory in wasm32 layout (`core/wasm_layout`); frames as handles looked up by name on each use; natives with converted arguments and checked results; a per-call instruction budget; traps disable the mod like a crash. Almost the whole API (not hooks, `resolve`, services, paths). C SDK: `tome_wasm.h`, `support/tome_wasm.c`, `tome_add_wasm_mod` (clang + wasm-ld or rust-lld), `hello-wasm` example, template. Rust SDK: the `tome-wasm` crate (`sdk/rust/wasm`: closures, `export_mod!`, constants generated from `tome.h`), `hello-wasm-rs`, template, `tome_add_rust_mod(WASM)`. WAMR allocates from a capped heap (`core/wasm_alloc`). Approval says "runs sandboxed code". Unit, host and e2e tests; `fuzz_wasm_abi`, `fuzz_wasm_layout`, and `fuzz_wamr` (WAMR itself in Tome's configuration, for the host: 1.3M runs found 15-byte modules making WAMR allocate gigabytes, now refused by the cap); `docs/wasm-mods.md` |
| Signed mods | A `tome.sig` (Ed25519 over the mod's id, code hash and `tome.mod.json`, Monocypher; small-order keys refused) records its key with the approval in `state.json`; an update signed by the same key runs without asking, one signed by another key or unsigned asks with a red "Signer changed" and a warning; the details page shows "Signed by". `sdk/support/tome_sign.py` (keygen, sign, verify, RFC 8032 selftest; plain Python) and `tome_package(SIGN_KEY)`. Unit tests (RFC vectors, a Python-made signature, base64, weak keys), host tests (same key, other key, tampered, a Python-signed folder, a signed package), `fuzz_signature` |
| Fuzzing | `fuzz/`, `tools/fuzz.sh`: libFuzzer (ASan, UBSan) targets for the handshake, chat commands, zip and inflate, JSON/metadata/settings/keybinds, the JASS scan and native declarations, and paths. 4–7 min each found one bug: a `schema` past `INT_MAX` in `tome.mod.json` cast to `int` (fixed, regression test). Since then: a player's set is capped at `kMaxMods` (256) in the lobby and in games, and each player's handshake messages get a log budget (`LogBudget`: 100 lines per lobby, `kMaxMods` + 8 per game) |
| Folder layout | Mods in `tome/mods/`; settings by id in `tome/mods/config/<id>.json` and `config_dir` in `tome/mods/config/<id>/`, outside the mods' own files (ready for `.tome` packages). An old `mods/` folder and its `config/settings.json` files are moved on the first start (`migrate_layout`) |
| Packages (§5.9 item 11) | `.tome` files in `tome/mods/`: a zip of a mod folder (`tome.mod.json` at the top), unpacked into `tome/packages/<name>.<hash>/` and loaded from there like a folder mod; a changed file unpacks into a new copy and reloads the mod, unused copies are removed; a folder with the same id wins. `core/zip.*` (own reader and table-driven inflate; refuses unsafe names, names that are one file on NTFS, encryption, zip64; caps sizes) and `core/package.*`, unit-tested; files are created with `CREATE_NEW`. `tome_package()` in `TomeMod.cmake` packs a mod (`build/packages/hello.tome`). Details page says Package. e2e: unpack, folder wins, the package runs, an update in place, removal |
| Rust mods | API unchanged (`TOME_EXCEPTION_MOD_FAILED` added to `tome.h`, named in crash reports). `sdk/rust`: `tome-sys` (bindgen output of `tome.h`, committed; `tools/gen-rust-bindings.sh`, checked by a test) and `tome`, a safe layer over every API function: lifecycle via `export_mod!`, natives via `native!` (JASS types only), closures for callbacks (kept until the next enable or unload), plain functions for hooks, `Result`s, API version and table size checks, event fields checked against `size`. Panics are caught at every call from Tome, logged with their location and raised as a crash. `tome_add_rust_mod()` in `TomeMod.cmake`; `TOME_BUILD_RUST`. Unit tests against a mock table under Wine; e2e: `hello-rs` loads, resolves, runs natives (real, string, code), events, a hook, a chat command, a main menu button, and a panic disables it |
| C++ mods | API unchanged. `sdk/include/tome/tome.hpp` (C++17, header-only): `Mod::call(&tome_api::<fn>, ...)` reaches every API function generically (the table's size checked for that field, the mod's handle filled in, lambdas for callback + user pairs, kept until the next enable or unload, `std::string` for `const char*`), so new functions need no header change; `tome::native<fn>`, `Mod::text` for snprintf-style getters, `sized<T>()`, `has_field`; exceptions caught and reported as crashes; `TOME_EXPORT_MOD`. Unit tests against a mock table (no exceptions, C++20); e2e: `hello-cpp` (built as C++17 with exceptions) runs natives, events, a hook, a chat command, a main menu button, and a throw disables it |
| SDK cleanup | `sdk/README.md` (what a mod is and can do, starting, trying it in the game, the rules), `sdk/docs/tome-mod-json.md` (every field, ranges, key names, settings), JSON Schemas for `tome.mod.json` and `config.schema.json` (`$schema` accepted), templates in C, C++ and Rust (built here, and checked building out of the tree), the toolchain in `sdk/cmake/`, `TomeMod.cmake` checks for a 32-bit build and links the runtime per compiler, the `sdk-dist` zip (builds on its own: all examples and templates), `tests/test_abi.cpp` (every released field's offset, so `tome.h` stays append-only), `tome_has_services` |
| Testing mods without the game | API unchanged. `tome-check` (run by `TomeMod.cmake` after each mod build: metadata, `requires`, the DLL and its load/enable/disable/unload, natives declared but not registered, the icon, every warning Tome logs) and `tome_test.dll` with `sdk/include/tome/tome_test.h` (mods' own tests in C or C++ for mods in any language: natives with strings, reals and code values, chat commands, events, keybinds, settings, crashes; `tome_add_mod_test`, a case per process): the real loader against the fake game image of the host tests (moved to `loader/src/testing/`), shipped in the SDK zip's `bin/` and `lib/`. Tests for the hello example and the C template; `tome_mod_tools_tests` on the Rust and Wasm examples; `tome_check_<fixture>` on broken fixtures |
| Tooling | Unit tests, e2e test in the real game (off screen in Xvfb, own Wine prefix, a linked game folder: nothing written to the install; input waits for log lines), `Makefile`, a container with the whole toolchain, `tome-mpq`, `tools/re/*`, `tools/game.sh` |

## Next: public release (0.1.0), in order

Licensing is settled: GPL v3 or later for Tome (`LICENSE`), MIT for `sdk/`
(`sdk/LICENSE`) and MIT-0 for what mods compile in (`sdk/LICENSE-MIT-0`),
so mods may use any license and owe no notice. Third-party code in the
binaries and its licenses: `loader/third_party/NOTICES.md`.

Stability comes first: no compiler or link setting changes (LTO would cut
`tome.mix` from 2.3 MB to 1.4 MB, but `-O3` without it is what the tests
cover, and the hooks, SEH frames and asm depend on code layout).
`tome.mix` and C/C++ mods need Windows 7 or later (system DLLs only);
Rust mods need Windows 10 (`WaitOnAddress`; Rust's own minimum).

1. ~~**Debug switches out of release builds**~~ Done: the `TOME_DEV` CMake
   option (off by default, on in the Makefile's `DEV`) gates every
   `TOME_DEBUG_*` read; `tools/e2e-test.sh` refuses a build without it.
2. ~~**SPDX headers**~~ Done: `GPL-3.0-or-later` outside `sdk/`, `MIT` in
   it (not `sdk/templates/`, which become mod makers' own), added and
   checked by `tools/spdx.py` (test `tome_spdx_headers`); the generators
   write theirs. The SDK zip has `licenses/` (the GPL for its tools and the
   third-party licenses); the Rust crates are `publish = false` (`tome` is
   taken on crates.io).
3. **A player download:** a `dist` target making `tome-<version>.zip` with
   `tome.mix`, a short install/uninstall readme, `DISCLAIMER.md`,
   `licenses/` as in the SDK zip (`sdk/cmake/dist.cmake`) and an empty
   `tome/mods/` (maybe `hello.tome`), next to the SDK zip.
4. **CI:** GitHub Actions running `make container-test`, a short
   `tools/fuzz.sh` run and the Rust bindings check on every push; on a
   `v*` tag, build both zips (release builds, `TOME_DEV` off) and attach
   them to a release with their SHA-256s, and the WAMR archive the build
   used (part of the GPL's corresponding source; `loader/third_party/NOTICES.md`). The e2e test needs game files and stays local (say so in
   `CONTRIBUTING.md`).
5. **Real Windows:** everything so far ran under Wine with Mesa. One manual
   pass on Windows 10/11 with Direct3D and Defender on: install, Mods menu,
   approving a DLL mod, settings, keybinds, a LAN game, safe mode, uninstall.
6. ~~**Crash loops** (§9)~~ Done: starts that don't reach the main menu
   (or a game) are counted in `tome/startups`; after 3 in a row the next
   start is in safe mode, and a popup names the last crash and offers to
   keep safe mode (`tome/safe`). Faults outside mod calls get
   `tome/crashes/crash-<time>.txt` (summary, registers, likely callers, the
   mods, the log's tail) and a minidump, from a filter in front of
   game.dll's own, which still runs. Later: guard threads mods start
   through the API.
7. **The handshake in `tools/e2e-test.sh`:** a two-instance run (own Wine
   prefix and network namespace for the second, `architecture.md`, "Testing
   with two instances") with matching and mismatching `sync-test` mods, a
   refused Start, the game report and the replay, and the game lock (a
   toggle that waits, applied when the game ends). Find the host's net id.
   Joining another host's game and private servers need a manual test.
8. **Deterministic tests instead of e2e checks** (started): `tome_host_tests`
   (the loader against a fake game image, no game files) and
   `tome_game_tests` (against the real `game.dll`, loaded but never run)
   cover lifecycle, approval, natives, crash isolation, hooks, packages,
   safe mode and every startup patch, in seconds and the same every run.
   They already found two mapped byte patterns running into the next
   function (`TextTags::SetFadepoint`, `SetLifespan`; fixed, and a unit
   test keeps it so). Next: chat commands and events dispatch (needs the
   event hooks on the fake image, or the dispatch split from the hooks),
   settings and keybinds, the memhack guard on the real compiler
   (`reverse-engineering.md`, "Running the compiler outside a game"), and
   then trimming `tools/e2e-test.sh` to what needs the game running.
9. **Known flaky spots** fixed or listed under "Known issues" in the README:
   the popup that stayed hidden (1 in 13), the Options screen's left panel
   after the Mods panel was used. The e2e test is no longer timed
   (2026-10-30: event-driven input, headless and silent by default, about
   2 minutes); watch whether its occasional flaky check is gone.
10. **Docs and repository files:**
   - README: player install first (download, copy, uninstall), building
     later; a screenshot of the Mods menu; "1.27b only, idle on other
     builds" up front; status pointing at this file, not `research.md`;
   - `CHANGELOG.md`, and how the API version relates to the release version
     (`tests/test_abi.cpp` pins released fields);
   - `CONTRIBUTING.md` (the rules in `CLAUDE.md`, the local-only e2e test),
     issue templates asking for `tome/tome.log` and the game build
     (`SECURITY.md` is written: the threat model and where to report; turn
     on private vulnerability reporting in the GitHub settings);
   - `sdk/README.md`: check the C, C++ and Rust examples it promises all
     exist; a "first mod in ten minutes" walkthrough.
11. **Antivirus and anti-cheat:** a DLL loaded by the sound library that
    patches code looks like malware to heuristics. Code signing if
    affordable, otherwise published hashes and submitting the release to
    Microsoft/VirusTotal before announcing. Find out whether the private
    servers still playing 1.27 flag `tome.mix`, and say in the README
    which servers Tome is meant for.
12. **Mods at launch:** `enhanced-options` and one or two more, and a place
    to list mods (a curated list in its own repository is enough).
13. Tag `v0.1.0`.

## Later

- **WebAssembly mods, what's left** (`wasm-mods.md`): typed hooks for both
  kinds of mod, as mods ask for them.
- **More game builds** (§6): 1.26a first, then 1.27a, 1.24e, 1.28.x, and
  1.29–1.31, each with its own mapping file and signatures.
- **World Editor support:** load in `worldedit.exe`, expose Tome natives in GUI triggers.
- **SDK polish:** a docs site (from `sdk/README.md` and the headers), a
  template repository that fetches the SDK zip from a release, C headers
  generated from the mapping files (typed function pointers), testing a
  Visual Studio build. For `tome_test.h`: a Rust crate over it (`cargo
  test` for Rust mods; they can already be tested from C), reading mods'
  asset files through the fake Storm, and frames on a fake UI.
- **Porting major existing mods** (§10): UjAPI, WFE, RenderEdge, w3l,
  YDWE natives, DzAPI natives. Deferred by decision.
- **1.31 feature parity** (§11, last phase).

## Open questions

See `research.md` §13. The most relevant now:
- the asset archive is rebuilt whole on every change; per-mod archives need
  Storm's order among equal priorities (unknown) or one priority per mod;
- the second native table (`0x713450`);
- handle arguments: resolving handle ids to game objects (the handle table);
- the e2e test's occasional flaky check;
- the host's net id as a client sees it (the handshake answers every player who sends a set instead);
- whether the Options screen's own left panel still animates correctly after the Mods panel was used (not yet tested);
- `framehandle` for the Blz frame natives: 1.27b has no such type, so they
  use integer ids. Matching 1.31 sources exactly needs a handle type and
  handle-table entries;
- events: verified for single player (menus and `-loadfile`), hosted LAN
  games, saved games and replays. Joining another host's game and
  Battle.net games aren't tested (they need a second machine);
- chat commands work in games only, not in Battle.net or lobby chat, and
  chat in replays is untested;
- more events: hover; alliance changes (not found); buffs and missiles
  (no single entry point found); a corpse that finishes decaying (not
  `CUnit::Remove`); regions and ranges (need a registration's values); the
  synced selection events (the game only syncs a selection while a trigger listens);
  detected and hidden, defeat, leaving, research, rescue, sales, most
  cancels and the menus' save/load are mapped but not yet seen in a test;
- popups: once, a frame set made right after a mod menu click stayed hidden
  (1 in 13 tries, cause unknown); a shown popup is shown again every 250 ms.
  Popups and restarting aren't in `tools/e2e-test.sh` yet;
- destroying frames (vtable `+0xD8` looks like a deleting destructor); for now
  a disabled mod's frames are hidden and go away with their screen.
