# Tome architecture

How the loader works today. For plans see `ROADMAP.md`, for game internals
`reverse-engineering.md`, and for the original design discussion `research.md`.

## Repository layout

```
CMakeLists.txt            top level: options, MinGW stdio, subprojects
cmake/                    i686 MinGW toolchain (sets Wine as the test runner),
                          embed_mappings.cmake
mappings/                 the mapping database: one JSON file per game build
loader/
  src/core/               portable logic, no Windows APIs, unit-tested
    json.*                JSON DOM parser and serializer
    hash.h                FNV-1a (change fingerprints, generated names)
    version.*             semver + game-version ranges and aliases
    metadata.*            tome.mod.json schema validation
    resolver.*            dependencies, breaks, provides, cycles, load order
    jass.*                native declaration parser → engine signature
    mappings.*            mapping file parser; binds the symbols Tome needs
    verify.*              checks byte signatures and call sites against an image
    rtti.*                reads MSVC RTTI: an object's class and base classes
    frames.*              frame id table for JASS, mod FDF archive paths
    chat.*                chat command parsing, /tome subcommands
    handshake.*           multiplayer handshake: mod-set lines, comparison, lobby state, game report
    game_lock.*           which required mods a multiplayer game locks, and the notes
    command_line.*        splits the command line like argv (for -tomesafe)
    hooks.*               argument layouts from mapping signatures; hook chain order and run
    x86.*                 instruction length decoder; moves a function's first instructions
    pe.*                  lays out a PE file like the loader (for the test)
    embedded_mappings.h   mappings/*.json compiled in (generated .cpp)
    mpq.*                 MPQ v1 writer (overlay archives), streaming for large ones
    assets.*              asset archive paths; which mods' assets to mount, per reload mode
    main_menu_fdf.*       generates TomeModMenu.fdf (main menu and Esc menu layouts)
    mod_menu_model.*      the mod menu's pages and what their buttons do
    mod_status.h          ModStatus, one row of the mod list
    settings.*            config.schema.json subset, settings values, menu steps
    keys.*                key names, declared keybinds, keybinds.json, conflicts
    popup.*               popup layout (rows, paged columns), keys, TomePopup.fdf
  src/                    Windows side (tome.mix)
    dllmain.cpp           pins the module, calls start_bootstrap
    bootstrap.*           startup sequence (see below)
    mappings.*            loads the embedded mapping files, finds the build
    game_build.*, sha256.*  build identification
    patch.*               call-site and pointer patching, verified before writing
    natives.*             native registry, thunks, InitNatives hook, JASS strings and code
    hooks.*               inline hooks on mapped functions: patch, entry stub, dispatcher
    mod_manager.*         discovery, state.json, lifecycle, hot reload, crashed mods, safe mode
    mod_handle.h          tome_mod, the handle mods get
    guard.*               crash isolation: an SEH frame around every call into a mod
    api.*                 the tome_api table given to mods
    main_thread.*         window-message dispatcher (the main-thread safe point)
    watcher.*             change notifications (+ timestamp polling under Wine: every 1.5 s, or 50x as long as a poll takes)
    storm.*               Storm.dll file/archive functions by ordinal
    overlay.*             the generated overlay and the mod assets archive
    frames.*              game frame functions, the frame API for mods, Blz* natives
    events.*              game events, screens and chat commands (Tome's own hooks)
    handshake.*           the multiplayer handshake in the lobby and in games
    mod_menu.*            the mod menu on the main menu and in the Esc menu (frames, hooks, animation)
    settings_api.*        the settings part of the API (values live in the mod manager)
    keybinds.*            keybinds: the keyboard hook, tome/keybinds.json, the API
    popups.*              the popup queue and its frames on the current screen; the API
    restart.*             restarting the game (WM_CLOSE, then a rundll32 helper)
    debug_input.*         TOME_DEBUG_CLICKS: clicks, keys, typed text and chat lines (tests)
    log.*, paths.*        tome.log, game/mods/tome folders (TOME_HOME)
sdk/
  include/tome/tome.h     the public C API for mods
  include/tome/utf8.h     UTF-8 helpers (header-only; also used by Tome itself)
  CMakeLists.txt          includes TomeMod.cmake, builds the example
  TomeMod.cmake           tome_add_mod() helper (mods outside this repo include it too)
  examples/hello/         example mod: natives (numbers, strings, callbacks), buttons on three screens, events, /hello,
                          settings (config.schema.json) and a keybind
tests/                    unit tests (run under Wine by CTest)
  test_hook_runtime.cpp   the hook engine on real functions (a second test executable)
  e2e/                    fixture mods (hook-a and hook-b are built) and the map-script patcher
tools/
  e2e-test.sh             end-to-end test in the real game
  game.sh                 start/stop/wait/screenshot helper
  mpq/                    tome-mpq (StormLib), dev-only
  re/                     reverse-engineering scripts
docs/
```

## Startup sequence

1. **Miles loads `tome.mix`** while game.dll initializes, on the game's main
   thread. game.dll is loaded; `FrameDef.toc` and the startup InitNatives
   call have already run.
2. **`DllMain` → `start_bootstrap`** (still on the main thread, loader lock held):
   - resolves paths (`TOME_HOME` support) and deletes archives left in
     `tome/cache` by the previous session, except its last assets archive
     (see "Asset overlay");
   - checks for safe mode (see "Crash isolation and safe mode");
   - identifies the build from the in-memory PE header;
   - checks the build's byte signatures and call sites against the loaded
     module (`verify_loaded_build`); on any mismatch nothing is patched;
   - installs the hooks, each checking the target bytes before patching:
     the 3 InitNatives call sites, the FrameDef.toc call site, two
     CMainMenu vtable slots, and the event hooks (`events::install`, through
     the hook manager);
   - builds and mounts the generated overlay and loads Tome's frame
     definitions (`mod_menu::reload_frame_definitions`);
   - mounts the assets of the mods that will run
     (`ModManager::mount_startup_assets`: discovery, state and resolution,
     no DLLs), so they're in place before the main menu loads its textures;
   - starts the bootstrap thread.
3. **The bootstrap thread:**
   - opens `tome/tome.log` and verifies game.dll by SHA-256 (refuses unknown builds);
   - logs each signature mismatch by symbol name and stays idle if there was
     one (if game.dll wasn't loaded at attach, the signatures are checked
     here instead, before the late hooks);
   - logs the hook results;
   - runs the **first mod load** (`ModManager::refresh`) off the main thread.
     The main thread keeps running meanwhile, so a handler a mod registers
     (a TICK, a hook) could be called before the mod is set up: while a
     mod's `tome_mod_load` or `tome_mod_enable` runs, guarded calls into it
     from other threads are skipped (`tome_mod::starting_on`). Then it
     signals `natives::set_mods_ready()`, which InitNatives waits on (≤ 10 s);
   - finds the game window and installs the main-thread dispatcher;
   - starts the file watcher.
4. **Later changes** (state.json edits, DLL rebuilds, menu clicks) always run
   on the main thread: the watcher only compares file timestamps, then posts
   a refresh to the window-message dispatcher. Menu clicks are already on the
   main thread.

## Mapping database

Every game address, struct offset and build-specific constant lives in
`mappings/<build>.json`. CMake compiles the files into `tome.mix`
(`cmake/embed_mappings.cmake`), so Tome still ships as one file. They're
parsed on first use, in DllMain (memory only, no I/O). A file with any
error is skipped, its build counts as unsupported, and the bootstrap thread
logs each error with the symbol's name.

```json
{
  "schema": 2, "name": "1.27b", "file_version": "1.27.1.7085",
  "pe_timestamp": "0x584a499e", "size_of_image": "0x00e6e000", "sha256": "…",
  "symbols": {
    "CTextFrame::SetText": {"kind": "function", "rva": "0x000fde90", "conv": "thiscall",
                            "sig": "void(CTextFrame* this, const char* text)",
                            "bytes": "55 8b ec 53 56 57 8b 7d 08 8b f1 85 ff 74 5f 57"},
    "InitNatives.call.startup": {"kind": "call", "rva": "0x0004542b", "target": "InitNatives"},
    "CMainMenu.vtable": {"kind": "data", "rva": "0x00ab1894"},
    "CFrame.layout": {"kind": "offset", "value": "0xb4"},
    "AnimTag.options": {"kind": "constant", "value": 23}
  }
}
```

- Kinds: `function` (needs `conv` = cdecl/stdcall/fastcall/thiscall, `sig`
  and `bytes`), `data`, `call` (an `E8 rel32`; `target` names a function), `offset`
  (bytes, including vtable slots), `constant`, `import` (an import address
  table slot: `conv`, `sig`, and `target` = `"module!function"`, e.g.
  `"kernel32.dll!GetTickCount"`). Functions, data, calls and imports have
  an `rva` inside the image; offsets and constants have a `value`. Numbers
  are JSON integers or `"0x…"` strings. `doc` is an optional note.
- `bytes` is the function's first 5–64 bytes as lowercase hex, `??` for
  bytes the Windows loader relocates (ASLR). `tools/re/signatures.py`
  generates them: at least 16 bytes, extended until the pattern is unique in
  its section. At startup `verify_code` checks every function's `bytes` and
  every call's `E8 rel32` and target against the loaded module, and finds
  every import's slot in the import directory (the names come from the
  lookup table, which the loader leaves alone), before anything is patched. A mismatch logs the symbol and leaves the game
  unmodified. A unit test runs the same check against the local
  `WarCraft III/game.dll` (skipped when there's none).
- Names are stable across builds: `Class::Method` for functions,
  `Class.field` / `Class.vtable.Method` for offsets, `Thing.call.where` for
  call sites.
- `bind_build` (core) copies the symbols Tome itself uses into the typed
  `BuildMappings` struct; a missing one rejects the build. A unit test runs
  this on every embedded file.
- Mods call `api->resolve(name)` (or `tome_resolve(api, name)`), which
  returns a `tome_symbol` with the absolute address. Names listed in
  `tome.mod.json` `"requires"` are checked before the DLL loads; names with
  a single `:` (`event:frame`) are capabilities and aren't checked yet.
- Adding an address: add it to the JSON with its calling convention and
  signature, run `tools/re/signatures.py … --write` to fill in `bytes`, then
  document it in `reverse-engineering.md`. If Tome itself
  uses it, add a field to `BuildMappings` and a line to `bind_build`.

## Mods

- Folder: `<home>/mods/<folder>/tome.mod.json` (+ optional DLL). `<home>` is
  the game folder unless `TOME_HOME` is set.
- State: `<home>/tome/state.json` (`{"schema":1,"mods":{"id":{"enabled":bool}}}`).
  New mods start enabled. Entries for mods that aren't installed are kept.
- Turning a mod off (`ModManager::set_enabled`, from the mod menu or
  `/tome disable`) also turns off the enabled mods that depend on it,
  directly or not, in `state.json` (`dependents_of` in `core/resolver.*`).
  It compares what the resolver lets run with and without the mod, so a
  mod stays on if another mod that runs still provides what it needs, and
  disabling a mod that's already off changes nothing else.
  Turning the mod back on doesn't bring them back.
- `ModManager::refresh` on every run:
  1. discovers mods and declares their natives;
  2. reads the state and resolves dependencies;
  3. stops mods that should no longer run, plus their dependents, in reverse
     enable order; unloads changed or removed DLLs;
  4. loads (from a **shadow copy** in `tome/cache/`) and enables, in load order;
  5. remembers load failures per DLL timestamp, so they aren't retried every time.
- Lifecycle exports: `tome_mod_load` (required), `tome_mod_enable`,
  `tome_mod_disable`, `tome_mod_unload`. Every registration a mod makes
  through the API records a revoker. On disable, Tome runs them in reverse.
- Hot reload: a changed DLL is disabled, unloaded, reloaded and re-enabled.
  Its dependents are bounced too.
- Crashes: see "Crash isolation and safe mode".

## Crash isolation and safe mode

`loader/src/guard.cpp` and `ModManager`. Every call Tome makes into a mod
goes through `guard::run(mod, what, detail, fn)`: the four lifecycle exports,
natives, hook handlers, event handlers, chat commands, UI callbacks and
clicks. Tome's own handlers (no mod) aren't guarded.

- **The guard.** GCC can't compile `__try` for 32-bit Windows, so
  `tome_guard_enter` (assembly) does what MSVC's `__try` does: it links an
  SEH registration record of its own into `fs:[0]`, saves esp, ebp, ebx,
  esi, edi, the x87 control word and MXCSR, and calls the function. Only a
  fault nothing inside the call handled reaches its handler: inner SEH
  frames (the mod's own `__try`, the game's) are asked first. The handler
  takes errors (severity `11`, which includes MSVC's C++ exception
  `0xE06D7363`), breakpoints and misaligned data; anything else is passed on.
  It copies the exception and the top 64 words of the stack, then
  `tome_guard_resume` calls `RtlUnwind` up to its record (running the unwind
  handlers of the frames above it: the game's `__finally` blocks and C++
  destructors) and jumps back into `tome_guard_enter` with the saved esp
  and ebp. That unlinks the record, clears the direction flag, empties the x87
  stack (`fninit`) and restores the control words, and the call returns
  false. The handler is in `tome.mix`'s image, which has no SafeSEH table
  (and no `NO_SEH` flag), so Windows accepts it.
- **After a stack overflow**, the guard page is gone; the guard puts it back
  at the bottom of the committed stack (like `_resetstkoflw`), so a second
  overflow on the thread is caught too.
- **Thread state:** a crash skips the rest of whatever ran inside the call,
  including hook chains entered from it, which keep thread-local records
  (`t_running`, `t_dispatches` in `core/hooks.cpp`) and busy counts. The
  guard takes `hooks::thread_state()` before the call and restores it after
  a crash, releasing those gates. Memory the skipped frames owned leaks, and a
  lock one of them held stays held; that's the price of not crashing.
- **The log:** `<mod> crashed in <what> <detail>: <exception> at
  <module>+0x<offset>; Tome stopped the call and disables the mod`, e.g.
  `crashed in native CrashNative: access violation writing 0x00000000 at
  crash.3.dll+0x14a2` (shadow copies are named `<id>.<n>.dll`, laid out like
  the original), then `called from (likely): …`: the copied stack words that
  are return addresses (in a module, right after a call instruction).
- **What a crashed call returns:** natives 0, as if disabled. In a hook
  chain the crashed handler counts as skipped: BEFORE and AFTER go on with
  the next handler; a REPLACE handler that hadn't called `hook_call_next`
  passes on to the next one or the original (`Dispatch::handed_on` counts
  hand-ons, so the original never runs twice). A crashed chat command still
  counts as handled, so the line isn't sent.
- **The mod:** the first crash is recorded in its `tome_mod` (`faulted`,
  `fault`); from then on its guarded calls return at once without calling it
  (except `tome_mod_disable`/`unload`, `guard::kEvenIfFaulted`). The fault sink
  posts a refresh to the main thread, or, if the crash happened inside a
  refresh on this thread (`tome_mod_enable`, or a callback run meanwhile),
  that refresh takes another pass before it returns. The refresh treats a
  crashed mod like a changed DLL: its dependents are disabled first, then
  it gets `tome_mod_disable`, its registrations are revoked, it gets
  `tome_mod_unload`, and it's unloaded **without** `FreeLibrary`: a thread it
  started, a patch of its own or a pointer the game holds may still lead into
  the DLL. Its assets are removed. The crash becomes its load failure
  (`failures_`, `crashed`), shown as its status (`error: crashed in …`) and
  in the mod menu.
- **Retrying:** a crashed mod stays off until its DLL changes (hot reload
  loads the new one), or the user turns it off (state.json, the mod menu,
  `/tome disable`) and on again; `set_enabled(id, true)` also retries it
  directly. A crash in `tome_mod_load` is a load failure with the same rules.
- **Not covered:** threads a mod starts itself, a fault in the game's own
  code outside any mod call, and process-ending errors that bypass SEH
  (`__fastfail`, `/GS` failures, heap corruption detection).

### Safe mode

Checked in DllMain (`safe_mode_reason` in `bootstrap.cpp`), before the
startup assets: the `-tomesafe` argument (`core/command_line`, any case), Shift
held while the game starts (`GetAsyncKeyState`), or a `tome/safe` file. The
log says which. In safe mode `ModManager::wanted_by_user` is false for every
mod, so no DLL loads and no assets mount; natives are still declared (maps
load, and they return 0). `state.json` isn't changed. Mods enabled during the
session (mod menu, `/tome enable`; `set_enabled` keeps a set) run as usual,
so one can find a bad mod by enabling them one at a time; disabling writes
`state.json` as usual. Their status reason is `safe mode`; the mod menu shows
"Disabled (safe mode)" and "Safe mode" in its summary.

The mod menu's Tome page turns safe mode on and off during a session
(`ModManager::set_safe_mode_now`): on stops every mod; off forgets the
session's choices, starts the mods `state.json` wants, and deletes
`tome/safe` so the next start is normal too.

## Natives

- Each declared native gets a permanent slot plus a 48-byte thunk that
  jumps to `target` (`mov eax,[target]; test eax,eax; jz ret0; jmp eax;
  ret0: xor eax,eax; ret`), after recording `code` arguments (below).
  The engine binds the thunk; `register_native` sets the target. A disabled
  mod's natives return 0, so maps keep loading.
- Natives are declared for **every installed mod that targets this build,
  enabled or not**, from `tome.mod.json` `"natives"`.
- After each InitNatives, Tome binds all of them with the game's BindNative.
- Built-ins: `TomeIsLoaded`, `TomeLogInteger`, `TomeLogString`, and the
  frame natives below.

### Strings and code (API version 6)

- **String arguments** arrive as `RCString*`; `jass_string` returns the
  text through the mapped `RCString::GetText`.
- **String results** are ids in the map script's string table, made by the
  game's own helper `CreateJassString` (what `I2S`, `SubString` and
  `GetPlayerName` return through). `jass_string_create` calls it on the main
  thread while a `CGameWar3` exists (the helper doesn't check), after
  replacing invalid UTF-8 with U+FFFD (`tome/utf8.h`).
- **Code** arguments are run with the game's own natives, called from C as
  the VM calls them (`Native.*` in the mapping): `CreateTrigger`,
  `TriggerAddCondition(t, Condition(code))`, `TriggerEvaluate`,
  `DestroyTrigger`. That's vJass's `.evaluate()`: the function runs to
  completion before `jass_code_call` returns, and its boolean comes back (a
  function returning nothing counts as false). `Condition` keeps one
  conditionfunc per code value, so it isn't destroyed; destroying the
  trigger frees its triggercondition. A fresh trigger per call keeps
  nested calls (a callback calling another one) independent. It works from
  inside natives and from events, clicks and chat commands in a game.
- **Code that waits:** `jass_code_execute` uses `TriggerAddAction` and
  `TriggerExecute` instead, so the function runs on an action thread that
  may sleep, and the call returns at its first wait. The trigger has to
  outlive the wait, so each code value gets one for the rest of the map
  script (`g_action_triggers`), reused by later calls; each TriggerExecute
  starts its own thread. The game destroys them with the script's other
  handles.
- **Stale code values:** a code value is an index into the script's code
  table, which starts over with every map, so an old value would run a
  different function. Every native thunk first checks its slot's
  `code_mask` (from the declaration, `NativeDecl::code_mask`); if the
  native takes `code`, it calls `note_codes`, which records those
  arguments. Both code calls accept only recorded values. The record and
  the action triggers are cleared at every InitNatives.

  ```
  cmp dword [slot.code_mask], 0 ; je target
  lea eax, [esp+4] ; push eax ; push slot ; call note_codes ; add esp, 8
  target: mov eax, [slot.target] ; test eax, eax ; jz ret0 ; jmp eax
  ret0: xor eax, eax ; ret
  ```

## Frames for mods

`loader/src/frames.cpp`, API version 3, extended in 5 (`sdk/include/tome/tome.h`). Mod
menu code uses the same helpers.

- **When:** mods register `on_ui(screen, fn)` in `tome_mod_enable`. The
  CMainMenu Setup hook runs Tome's own menu code, then every main menu
  callback; the other screens come from `events.cpp` (`frames::on_screen`,
  see "Events"). Each registration runs once per instance of its screen (a
  generation counter per screen); registering while the screen exists runs
  it right away on the main thread. A screen exists while it's the glue
  manager's current screen (main menu: while `MainMenuFrame` is the same
  frame), and the in-game UI while `CGameUI::Get(0, 0)` returns it.
  `ui_root(screen)` gives the screen's frame then, NULL otherwise.
  The first mod load is on the bootstrap thread, so the bootstrap posts
  `frames::catch_up` once the dispatcher is installed.
- **Threads:** every frame function checks it's on the main thread (the
  DllMain thread) and otherwise fails with `TOME_ERR_WRONG_THREAD`.
- **Class checks:** SetText needs a `CTextFrame`, animations a
  `CSpriteFrame`, clicks a `CControl`. Tome reads the class from the
  game's RTTI (`core/rtti`, reads limited to game.dll's image) and refuses
  anything else instead of corrupting memory.
- **FDF files:** `load_fdf(mod, "ui/x.fdf")` reads the file from the mod's
  folder, adds it with a one-line TOC to the generated overlay under
  `UI\FrameDef\Tome\<mod id>\` (see "Asset overlay") and calls
  LoadTocFiles. Each (mod, path) is loaded once per session; the game
  never replaces a definition, so changes need a restart.
- **Clicks:** Tome has one `CObserver` of its own: a static object with a
  copy of the game's CObserver vtable whose OnEvent (slot 3) is Tome's, and
  a reference count started so high it never reaches zero.
  `frame_on_click` registers it on the button with `CControl::SetListener`
  and a command id of Tome's (the same button keeps its id; registering
  again replaces the callback). A click calls OnEvent with the id at
  `event+8`, which looks up the callback, so it works on any screen and in
  games without hooking each screen's event handler. Tome remembers the
  name and context of every frame it hands out (`core/frames` FrameTable)
  and drops clicks whose button the game no longer finds. In a game, a
  clicked button keeps the keyboard focus and would take the next Enter
  instead of the chat box, so after a mod's callback Tome takes the focus
  back (`CFrame::SetFocus(0)` if `FocusedFrame` is that button).
- **Names:** `frame_create` (and `BlzCreateFrame`) refuse a name and
  context the game already finds: two live frames under one name crash the
  game when their screen goes. A mod enabled again on the same screen gets
  its hidden frame back with `frame_get`.
- **Disable:** revokers remove the UI callbacks and the clicks (a click on a
  disabled mod's button does nothing) and hide the frames the mod created,
  if the game still finds them under their names. Frames aren't destroyed;
  their screen owns them.
- **JASS:** `BlzGetFrameByName`, `BlzCreateFrame`, `BlzFrameSetPoint`,
  `BlzFrameSetText`, `BlzFrameSetVisible`, `BlzLoadTOCFile`, with 1.31's
  names but integer frame ids (the FrameTable index) instead of
  `framehandle`. Every use checks that the game still finds that frame
  under its name and context, so a stale or made-up id does nothing.

## Events

`loader/src/events.cpp` and `core/chat.cpp`, API version 5 (`on_event`,
`register_command`, `display_text`, `ui_root`); the selection, chat,
pause, unit, item, player result and saved game events and `selection` are
API version 8 (`loader/src/units.cpp`,
`core/units.cpp`); spells, damage, acquired targets, decay, detection, Esc
and arrow keys, destructable and item deaths and dialog clicks are API
version 12. Every event comes from one
of Tome's own handlers on a mapped function, added through the hook manager
(below) with no mod and no gate, at the highest priority in BEFORE and the
lowest in AFTER. `events::install` runs from DllMain after `frames::init`,
so even a `-loadfile` map's first load is seen; it resolves all its targets
before adding any handler. Everything runs on the main thread.

| Hook | Does |
|------|------|
| `CGlueMgr::SetScreen` (after) | maps the GlueScreen id to a `tome_ui_event` and calls `frames::on_screen` with `[mgr+CGlueMgr.screen]`; the main menu keeps its Setup hook. The loading screen is the map load: it reads the map's name and path, applies `reload: map` asset changes (`ModManager::on_map_load`, `assets::Moment::MapLoad`) and fires `TOME_EVENT_MAP_LOAD`. The natives are bound by then (CGameWar3::Reset), the map's files aren't read yet. The score screen ends the game. |
| `CGameWar3::Reset` (before and after) | ends a game that started and never showed a score screen: `TOME_EVENT_GAME_END`; while it runs (it removes every unit left, also after a load that was abandoned), no `UNIT_CREATED` or `UNIT_REMOVED` |
| `CGlueMgr::EnterGame` (after) | `TOME_UI_GAME` for `CGameUI::Get(0, 0)`, then `TOME_EVENT_GAME_START` |
| `PresentFrame` (before) | counts frames and measures the time between them (QueryPerformanceCounter); `TOME_EVENT_TICK` only if a handler is registered |
| `OnNetGameTick` (before) | in a game: adds the step (`CNetEventGameTick.step`, game seconds) to the game time and fires `TOME_EVENT_GAME_TICK`, only if a handler is registered. Follows the game speed, stops while paused. |
| `GameShutdown` (before) | ends a game that's still running when the program exits (exiting from a game normally runs Reset first, so this is a fallback), then disables and unloads the mods |
| `CGameUI::HandleSlashCommand` (before) | `/tome ...` and mods' commands: runs the handler, then skips the game's function and returns 1, so the game closes the chat box and sends nothing |
| `CGameUI::DisplayChatMessage` (before) | in a game: `TOME_EVENT_CHAT` with the sender's slot and name (`CPlayerWar3.name`, read without `GetName`, which writes a default), the text and the recipient |
| `CPlayerWar3::On*` (before): 32 of the JASS player-unit events (death, attacked, rescued, construction/upgrade/training/research start, cancel and finish, hero level, skill, revivable and revival, summon, items, sales, owner change, decay, detected, hidden), and `CUnit::OnLoaded` | the game's own functions for its JASS player-unit events, hooked before they check whether a trigger listens, so they fire for every unit whether or not the map listens. One handler driven by a table (`kUnitHooks`: for each function, which argument is the unit, the other unit, a third unit, the item, the type id and the level, plus two special cases); `bind_build` checks the names (`kEventFunctions`). From the loading screen on (map scripts set units up while loading); `tome_unit`s built only if a handler is registered |
| `CUnit::OnOrder`, `OnPointOrder`, `OnTargetOrder` (before) | `TOME_EVENT_UNIT_ORDER` with `COrder.id`, the point and the target (a unit, an item or a destructable by vtable, `units::classify_widget`). The player functions are only called when a trigger listens, so the `CUnit` ones are hooked |
| `CUnit::OnSpellChannel`, `OnSpellCast`, `OnSpellEffect`, `OnSpellFinish`, `OnSpellEndCast` (before) | the five spell events, for the same reason: unit, the ability's type (`CAbility.type_id`), its target widget and point through the spell ability's virtual functions (`CAbilitySpell.vtable.*`, as `GetSpellTargetUnit` and `GetSpellTargetX` call them) |
| `CUnit::OnDamaged`, `OnAcquireTarget` (before) | JASS has these as unit events only (no player version); the `CUnit` functions check the registration themselves and are called unconditionally. Damage: the amount from the `float*` argument, before the unit's life goes down |
| `CPlayerWar3::OnEndCinematic`, `OnArrowKey` (before) | `TOME_EVENT_ESC_PRESSED` and `TOME_EVENT_ARROW_KEY` (key and direction from the event id, `units::arrow_key`), from the network action handlers, so every player's keys reach every client. Esc is always sent; a player's arrow keys only while the map has a trigger on them (the sender checks) |
| `CWidget::OnDeath` (before) | `TOME_EVENT_DESTRUCTABLE_DEATH` and `TOME_EVENT_ITEM_DEATH` by the widget's vtable; units are left to `UNIT_DEATH` |
| `CScriptDialog::OnClick` (before) | `TOME_EVENT_DIALOG_CLICK` with the dialog's and button's handle ids and the player, from the network action handler |
| `OnNetShowSkillMenu`, `OnNetShowBuildMenu` (before) | `TOME_EVENT_SKILL_MENU` and `BUILD_MENU` with the player (`CNetEvent.sender`): the network action handlers, since the command card sends the action on every click |
| `HandleManager::FireEndLevel` (before) | `TOME_EVENT_LEVEL_END`, in a started game |
| `CreateUnitObject` (after), `CUnit::Remove` (before) | `TOME_EVENT_UNIT_CREATED` with the new unit (the result; its JASS handle is made after this, so `unit->handle` is 0), `UNIT_REMOVED` with `this`; from the loading screen on, not inside `CGameWar3::Reset` |
| `CAbilityHero::AddXP` (before and after) | `TOME_EVENT_HERO_XP`: the hero ability's experience (`CAbilityHero.xp`) is kept before the call (a small stack) and compared after it, so only what was really added is reported; the hero is `CAbility.owner` |
| every game tick, while a mod listens | `TOME_EVENT_PLAYER_RESOURCE`: gold, lumber, food cap and food used of the 16 player slots (`CPlayerWar3.states`, `SyncedInt::Get`, scaled by `units::player_state_scale`), compared with the last tick's (`units::ResourceWatch`; the first reading after a game starts or a mod starts listening only sets the baseline). `TOME_EVENT_DAY` and `NIGHT`: the time of day (`GetFloatGameState`) crossing 6:00 or 18:00 (`units::day_change`) |
| `CPlayerWar3::RemovePlayer` (before), `OnLeave` (before) | victory and defeat (results 0 and 1, only while `CPlayerWar3.game_state` is 1) and leaving; each once per player and game (`units::Results`) |
| `CGameWar3::SaveGame` (before) | `TOME_EVENT_GAME_SAVE` with the path, before the game is written; both the `SaveGame` native and the Esc menu's Save Game end here |
| `HandleManager::FireGameLoaded` (before) | inside the saved game loader, before EnterGame: `TOME_EVENT_GAME_LOADED` with `CGameWar3.load_path` (or the map's path if the load request didn't leave one) |
| `CGameUI::SetPaused` (after) | in a game: paused is `CGameUI.paused_by_player` or `paused_by_trigger`; when that differs from the last call's, `TOME_EVENT_PAUSE` or `RESUME` with the player (-1 for 0xFF and for a trigger's `PauseGame`, a5 = 1, which passes the local player) |

- **Selection:** the PresentFrame handler, in a game, reads the local
  player's local selection (`CGameWar3.local_player` → `GetPlayer` →
  `CPlayerWar3.selection` → `CSelectionWar3.local_units`, a unit group, and
  `local_leader`) and compares it with the last frame's: unit pointers and
  agent ids (`CAgent.id`, so a new unit at a freed one's address counts)
  and the leader. A change fires `TOME_EVENT_SELECTION` with a `tome_unit`
  per unit (type id, `GetUnitTypeName`, owner, flags from `CUnit.flags`),
  built only if a handler is registered. Polling covers every way the
  selection changes (clicks, drag boxes, groups, Tab, triggers, deaths)
  for at most 12 pointers a frame. The snapshot is taken at game start and
  cleared at game end without an event.
- **Handle ids:** `tome_unit.handle` comes from `units::find_handle`, the
  lookup half of the game's `0x282E30` on the handle manager
  (`CGameWar3.handles`, `HandleManager.*`), 0 if the map script never got
  a handle for the unit. Making one on one client only would shift that
  client's later handle ids, so Tome never does.
- **Unit events and desyncs:** the handlers only read. They run inside the
  game's simulation, the same on every client; a mod that changes the game
  from them on one client only desyncs a multiplayer game, as with any
  client-side change.
- **Not here:** ties; the synced selection events (`EVENT_PLAYER_UNIT_SELECTED`,
  `DESELECTED`: the game sends the network action only while a trigger
  listens, so other clients never hear of it; the local selection is
  `TOME_EVENT_SELECTION`); alliance changes; trackables (local only);
  regions, timers and state limits (they need a registration's values;
  resources and the time of day are covered by polling); the tournament
  events (Battle.net tournaments only); buffs and missiles (not found).
- **Adding an event:** a `tome_event` value at the end, `kEventCount` in
  `events.cpp` raised to it (the handler lists are indexed by event), the
  mapping, and a `kUnitHooks` row (or a handler in `install`'s list) and its
  name in `kEventFunctions`.
- **Registrations** are shared `Registration`s in copy-on-write lists (one
  per event; commands in a map by lowercase name) behind an SRW lock. Firing
  takes a snapshot; a revoker sets `revoked` (on the main thread, before
  the mod's DLL can go) and removes it, and revoked entries are skipped, so a
  handler that disables another mod mid-round never calls into its freed DLL.
- **Why these points:** Reset also runs when a LAN lobby is created or
  left and partway through loading a saved game, so it can't mark a map
  load; every way into a game (menus, `-loadfile`, LAN, saved games,
  replays) shows the loading screen. GAME_END fires once per game that
  started, at the score screen or at the next Reset, whichever comes first;
  a load that fails before EnterGame ends nothing. The map's name and path are read again for each
  event (`CGameWar3.instance` → `CGameWar3.map_info` → `MapInfo.name`/`path`,
  RCStrings; `MapInfo.description` since API version 8).
- **Mid-game registration:** a `TOME_EVENT_GAME_START` handler registered
  on the main thread while a game runs (a mod enabled from the mod menu or
  `/tome enable`) is called once for that game, posted to the dispatcher so
  it runs after `tome_mod_enable` returns, unless the game ended or the
  handler was revoked by then (a game serial number).
- **Two clocks:** TICK follows rendering (real seconds, every frame,
  everywhere); GAME_TICK follows game time (5 ms steps, games only).
  `game_time` counts from the game's start, also for a loaded saved game.
- **Exit:** GAME_END also fires when the program exits from a game, from
  Reset or GameShutdown. Then GameShutdown's handler calls
  `ModManager::shutdown`: every running mod is disabled (reverse enable
  order), every loaded mod gets `tome_mod_unload`, and their registrations
  are revoked. The DLLs aren't freed, since the game's teardown still runs
  after it, and refreshes stop (the watcher may still fire).
- **Chat:** `core/chat` parses `/name args` (names: 1-32 of a-z, 0-9, `_`,
  `-`, case-insensitive). Mods can't take `tome` or the game's own
  commands, nor another mod's. `/tome help|list|enable <id>|disable
  <id>|reload [id]` answers through `CGameUI::DisplayText`; enable,
  disable and reload are posted to the main-thread dispatcher, so they run
  outside the game's chat handling (the mod being changed may have handlers
  on that very call). `reload` forces a DLL reload (`ModManager::reload`
  marks the DLL as changed and refreshes).
- **Verified paths:** single player from the menus and with `-loadfile`,
  a hosted LAN game (and a cancelled lobby), saved games and replays (see
  `reverse-engineering.md`, "Games"). Not covered: joining another host's
  game, Battle.net, lobby and Battle.net chat, chat in replays. The
  selection is verified in single player (clicks and `SelectUnit`), chat in
  single player and LAN, pause from the Esc menu and `PauseGame`, the unit
  and item events, melee victory, saving and loading a saved game with the
  map script's `SaveGame`/`LoadGame`, orders of all three kinds, attacked,
  training start and cancel, construction and upgrade start, hero skill,
  revivable and revival (`ReviveHero`), loading into a transport, owner
  change. Not verified: rescue, sales and pawning, the other cancels,
  research, revival at an altar, a LAN player's pause, defeat and leaving,
  the Esc menu's Save Game and loading from the Load Saved Game screen,
  replays and observers.

## Multiplayer handshake

`loader/src/handshake.cpp` and `loader/src/core/handshake.cpp` (unit-tested).
Every player of a game needs the same `"sync": "required"` mods, or the game
desyncs (`research.md` §4). **Verified** in LAN games between two instances
on one machine (2026-10-26): matching sets, a refused Start, the in-game
report on both sides and in a replay. Not in `tools/e2e-test.sh` yet; see
"Testing with two instances" below.

- **Identity:** a running required mod is `id version #hash`; the hash
  (`ModManager::sync_hash`, `ModStatus.sync_hash`) is FNV-1a over
  `tome.mod.json`, the DLL and every asset file (names and contents), folded
  to 32 bits and cached until one of them changes. Ids and versions are at
  most 64 characters (`kMaxIdLength`, `kMaxVersionLength`), so an entry
  always fits in one chat line.
- **Lobby** (GlueScreen 11, `CGameChatroom`): twice a second, from a
  PresentFrame handler, Tome reads the local net id (`NetGetLocalPlayer`),
  whether it hosts (`NetIsHost`) and the other players (`NetGetPlayerName` on
  every id byte: LAN lobby players have ids from 128 up, in the net client's
  second list), and feeds `handshake::Lobby`:
  - the host, if it has required mods, sends each player who joins its set
    as lobby chat lines (`[Tome] Required mods 1/1: hello 1.0.0 #1a2b3c4d`,
    split under 250 bytes) through `NetSendLobbyEvent` to that player only,
    and again to everyone when its set changes (also "none" to players it
    told before). A host without required mods sends nothing, so players
    without Tome see nothing;
  - a client answers each player who sent it a set (`[Tome] My mods 1/1:
    ...`), and again when its own set changes. It can't tell which player
    hosts (the game doesn't say where Tome can read it), so it notes when
    more than one player sent a set; a player pretending to host can add a
    note but can't divert the replies the real host checks. A client with
    required mods that gets no set within 5 s is told the host may not run Tome;
  - `CGameChatroom::OnChatReceived` (before) hides lines that decode as
    Tome's and passes them on; anything else, "[Tome] ..." typed by a person
    included, shows as usual. Notes go into the lobby's chat area
    (`CTextArea::AddText`) with a "Tome:" prefix;
  - `CGameChatroom::StartGame` (before): the host refuses to start while a
    player's set differs or hasn't arrived (with required mods of its own),
    listing each player and what differs (missing, extra, different version
    or hash). If the lobby couldn't be read at all, a host with required
    mods refuses too.
- **Games:** on the first game tick (not in replays, `NetIsReplay`), each
  player with required mods sends one SyncStoredInteger action per mod
  (game cache `TomeMods`, mission `1`, key `id version #hash`, value the
  number of mods), built in place (`CNetEventSyncStoredInteger::Init`,
  `NetSendAction`). Actions reach every player and the replay; clients
  without Tome store them in a game cache nothing reads (no handle, the same
  on every client). `OnNetSyncStoredInteger` (before) collects them per
  sender (`GameRecord`). After 2 s of game time, every human player in the
  game (controller user, playing) is compared with the local set; a player
  who sent nothing counts as having none. Differences are shown with
  `display_text` and logged; in a replay, the recorded players are compared
  with the viewer's mods.
- **Game lock** (§5.7, `core/game_lock.*`, `ModManager::set_game_lock`):
  a game with more than one person playing (controller user, playing; checked
  at the start and on the first step) or a replay locks required mods until
  it ends. The running required mods and what they depend on (directly or
  not, by id or `provides`) keep running as they were enabled: toggles,
  `/tome reload`, a changed DLL or `tome.mod.json`, or a removed folder
  don't stop or reload them, and their assets stay as mounted
  (`assets::plan`'s `frozen`). Mods that can't start before the game ends
  wait (state `waiting`): required mods that weren't running, mods that
  could meet what a locked mod depends on (so a locked mod never comes to
  rely on a mod that may still stop), mods a locked mod `breaks` (as it was
  enabled), and a new id in a locked mod's folder (its DLL would run twice)
  (`game_lock::waits`). A refresh leaves them out of the resolver, as if
  turned off, so they can't turn running mods off (one that breaks them, a
  load order cycle) or stand in for a dependency; what needs them while
  nothing that runs meets that need waits too (`waiting_dependents`;
  `game_lock::plan_start` holds the same as a backstop). A mod whose DLL is
  known to fail shows its error instead, and a mod that's running keeps
  running. `provides` meets a dependency only on any version, as in the
  resolver. Toggles still go into
  `state.json`; the mod list shows a note (`ModStatus.locked`), and the
  version, `sync` and handshake hash stay those the game started with (a
  locked mod whose folder is gone or unreadable stays listed as it started,
  so the handshake still counts it, and keeps its settings and keybinds). When the game
  ends, a refresh from the message loop applies what waited. Only a crash
  stops a locked mod (logged: the game may desync). A single player game,
  alone or against computers, isn't locked. Unit-tested; verified in a replay
  (2026-10-27: a locked dependency, a toggle that waits, a required mod
  waiting, both applied on leaving through End Game, Quit Mission); not yet
  in a two-instance game.
- **Not done:** saved games; the host's net id (see `reverse-engineering.md`).

### Testing with two instances

Two copies of war3.exe in one Wine prefix shut each other down (its
single-instance check), and LAN needs UDP/TCP 6112 for each (`netgameport`
doesn't move the LAN port). What worked: the second instance in its own
Wine prefix, and each instance in its own network namespace, joined by a
veth pair inside one unprivileged user namespace (`unshare -rn`, then a
nested `unshare -n`; 10.77.0.1 and .2, a default route on each side so
broadcasts cross). `nsenter -U` can't join that user namespace (setgroups is
denied), so the process holding the namespaces starts the games itself. The
second prefix has no profile, so the client types a name on the LAN screen
(`TOME_DEBUG_CLICKS` `text:`). Both instances share the game folder, so only
one should leave a game (`replay\LastReplay.w3g`).

## Hooks

`loader/src/hooks.cpp` (patching, Windows) and `loader/src/core/hooks.cpp`,
`core/x86.cpp` (layouts, chain order, instruction decoding; unit-tested).
API version 4 (`hook`, `unhook`, `hook_call_next` in `tome.h`).

- **Targets are mapped names.** `hook(mod, "CTextFrame::SetText", phase,
  priority, fn, user)` looks the name up in the mapping database; it must be
  a `function`, or since API version 9 an `import`. Its `conv` and `sig` give the argument layout
  (`hooks::layout_for`): thiscall puts `this` in ecx, fastcall the first two
  32-bit non-float arguments in ecx/edx, everything else is on the stack.
  Handlers see the arguments as 32-bit words in declaration order;
  `double` and 64-bit integers take two (low half first, two stack slots,
  never a fastcall register). Pointers and unknown type names count as
  32-bit, so a struct passed by value needs its fields spelled out in the
  `sig`. Results come back in eax, edx:eax, or st(0) for `float`/`double`
  (`result_float`).
- **Variadic functions** (`...` last): MSVC makes them caller-cleaned with
  everything on the stack, whatever the declared convention, and so does
  the layout. Tome can't know how many arguments the caller passed, so the
  dispatcher copies the next `kVariadicWords` (32) stack words after the
  fixed arguments (fewer near the top of the thread's stack) into
  `args[arg_count…]` (`extra_count`), and passes them on to the original.
  A function reads only what it needs, so the extra words are harmless.
- **One patch per function**, made on the first `hook` call and kept for
  the session:
  1. the current bytes are compared with the mapping's `bytes` again;
  2. `x86::relocate` copies whole instructions (at least 5 bytes, all
     inside the checked `bytes`) to a trampoline and appends a `jmp` back.
     Relative branches keep their targets: rel32 ones are re-encoded,
     short ones widened (`jcc rel8` → `jcc rel32`, `jmp rel8` → `jmp
     rel32`, `loop`/`jecxz` → `op +2; jmp short +5; jmp rel32`), and a
     target inside the moved bytes (a loop back to the start) moves with
     the copy, so it doesn't run the chain again. A function that returns
     or jumps away within 5 bytes can be hooked when the rest up to 5 is
     padding (int3/nop). Unknown opcodes and branches into the middle of a
     moved instruction are refused, so the hook fails instead of guessing.
     The decoder covers the one- and two-byte maps, `0F 38`/`0F 3A`, VEX
     and 16-bit addressing. A unit test relocates every mapped function's
     `bytes`;
  3. a 10-byte stub (`push <Target*>; jmp tome_hook_entry`) is written;
  4. the first 5 bytes become `jmp stub`, written as one 8-byte interlocked
     store when they fit in an aligned qword (usual for function starts).
- **Imports** (API version 9, `hooks::install_import`): the same stub, but
  nothing is moved: the "trampoline" is what the import slot held (the
  imported function, or another tool's replacement), and the slot gets the
  stub's address in one interlocked store. Every call game.dll makes
  through the import, by its thunk or by `call [slot]`, then runs the
  chain; other DLLs' imports are untouched. The slot is checked against the
  import directory at startup (above).
- **Calls:** `tome_hook_entry` (asm) pushes ecx/edx and calls
  `tome_hook_dispatch` with a pointer to them, the target, the return
  address and the stack arguments, plus 24 bytes for its answer. The
  dispatcher copies the arguments (declaration order) into an array, runs
  the chain, and writes eax, edx, the bytes to pop and, for floating-point
  results, the value and its kind; the stub loads st(0) from it (`fld
  dword`/`fld qword`), sets eax/edx and returns, popping the stack arguments
  for callee-cleaned conventions (`ret N`, N from the layout). The original
  runs through `tome_hook_invoke` (asm), which pushes the possibly changed
  arguments, sets ecx/edx, calls the trampoline, and stores eax, edx and
  (popped, so the x87 stack stays balanced) st(0); esp is restored from
  ebp, so the callee's convention doesn't matter. Registers no parameter
  uses pass through unchanged.
- **Chain order** (`core/hooks.cpp`, `run`): BEFORE handlers, then the
  REPLACE handlers nested (each calls `hook_call_next` for the rest; the
  last one's next is the original), then AFTER handlers. Within a phase,
  higher priority first, ties in registration order. A BEFORE handler can
  set `skip_original` (and `result`); REPLACE and the original are then
  skipped, the AFTER handlers still run.
- **Threads:** handlers run on whatever thread called the function. The
  chain is a copy-on-write vector behind a per-target SRW lock; a call keeps
  the snapshot it started with, so `hook`/`unhook` from any thread is safe.
- **Disable:** each `hook` records a revoker, tagged with its gate, that
  removes exactly that registration; `unhook` drops the revokers of the
  handlers it removed, so hooking and unhooking repeatedly doesn't pile
  them up. The patch stays; with an empty chain the dispatcher calls the
  original directly.
- **`hook_call_next`** only acts on a call running on the calling thread:
  `run` keeps a thread-local list of its dispatches, and the pointer is
  compared against them, never read, so a copied or stale
  `tome_hook_call*` does nothing.
- **Unloading safely:** every registration has a `hooks::Gate` (a busy
  count and a revoked flag), shared by every snapshot of the chain that
  holds it and listed in the mod's `tome_mod::hook_gates`. A call raises
  busy before it checks revoked, and removing a handler sets revoked
  before it leaves the chain, so once a revoker has run, no new call of
  that handler starts, even from a snapshot taken earlier (a skipped
  REPLACE handler passes on to the next). `ModManager::unload` then calls
  `hooks::idle` without waiting, so the main thread never blocks on them.
  If a call on another thread is still running, or one is on the unloading thread's own stack
  (a thread-local list of running gates says so: e.g. the mod menu
  disabled the mod from a click that came through the mod's own OnEvent
  hook), `FreeLibrary` and the shadow copy's deletion are retried from the
  main thread every 250 ms instead. `tome_mod_unload` still runs right away.
- **Testing:** `tome_hook_runtime_tests` hooks an import slot of its own
  and small assembly functions in each convention (with an MSVC-style prologue, one starting with a call)
  and checks arguments, results and stack balance; others return float and
  double, take 64-bit and variadic arguments, start with short jumps or
  loops, or return within 4 bytes; a thread blocked inside a handler keeps
  `idle` false until it returns. The e2e fixtures hook-a
  and hook-b both hook `RCString::GetText`; hook-b calls it on a fake string
  each time it's enabled, and the test checks the order of both mods'
  handlers, hook-a's changed argument reaching the original through
  hook-b's REPLACE, and the chain after disabling and re-enabling hook-a.

## Mod menu

- Tome generates `UI\FrameDef\Glue\TomeModMenu.fdf` plus `UI\FrameDef\Tome.toc`
  and puts them in the generated overlay (see "Asset overlay"), then loads
  `Tome.toc` with LoadTocFiles. No game file is changed. The FDF defines the
  panel twice with the same parts (`core/main_menu_fdf.cpp`): `TomeMods*`
  from the glue templates (8 rows) and `TomeEscMods*` from the Esc menu's
  templates (6 rows).
- **Pages** (`core/mod_menu_model.cpp`, unit-tested): List (every mod,
  Details and Enable/Disable), Details (metadata, status, every problem in
  full: metadata errors, the crash line and its "called from" line as
  logged, schema errors, warnings, waiting assets; Enable/Disable,
  Settings), Settings, Tome (versions, mods folder, safe mode, Keybinds,
  Mods Folder) and Keybinds. `build()` turns a page into a generic view
  (title, summary, rows of name/info/small button/button, or a text body,
  and four footer buttons); `click_row`/`click_footer` change the page and
  return an action (enable, set a setting, set a key, safe mode, ...) that
  `mod_menu.cpp` carries out before drawing the page again.
- **Clicks** inside both panels go to Tome's observer (`frames::on_click`,
  the same one mods' buttons use). Only the main menu's Mods button is
  bound with Glue BindControl (command `0x7100`, handled by the CMainMenu
  OnEvent hook), so its `M` shortcut keeps working.

### Main menu

- The CMainMenu Setup hook creates `TomeModsBackdrop` (the Mods button, upper
  slot of the bottom box) and `TomeModsPanel` under `ControlLayer`, moves
  `ExitBackdrop` (Quit) to the lower slot, registers the panel's buttons, and
  hides the panel.
- The panel uses the game's chained panel model (`TomeModsChains` sprite):
  "Options Morph" to lower, "Options Stand Alternate" while open, "Options
  Morph Alternate" to raise. Content and the logo are hidden while it moves.
  Steps are timed with `main_thread::post_delayed`, and a sequence counter
  drops stale steps.
- Rows come from `ModManager::status()`. Toggling calls `ModManager::set_enabled`,
  which saves state.json and refreshes immediately.
- A mod whose asset changes are waiting shows a note in its row ("restart
  to apply asset changes").

### In games (the Esc menu)

- Hooks (through the hook manager, AFTER) on `CEscMenuMainPanel::Setup`
  and `CEscMenuMainPanel::SetPanel`.
- Setup (once per game) creates `TomeEscModsButton` in `InsideMainPanel`
  and `TomeEscModsPanel` in the Esc menu's main panel. Options is narrowed to
  0.11 (`CLayoutFrame::SetWidth`) and moved left, Mods takes the right half
  of its row, Tips is anchored under Mods and End Game re-anchored under Load
  Game, so the menu keeps its shape.
- Mods calls `SetPanel(EscMenuPanel.help)`: the game hides its panels and
  sizes the backdrop for Help (0.432 × 0.384); the SetPanel hook then hides
  `HelpPanel` and shows Tome's panel. Any other SetPanel call (Done, which
  asks for `EscMenuPanel.main`; the menu reopening) hides it again, so the
  game's own state machine stays in charge. Esc is Tome's (`on_key`): back
  one page, and from the list back to the game menu. (Left to the game,
  Esc closed the menu with the Help state still set, and the next F10 did
  nothing, as it does after the game's own Help panel.)

## Settings

- `"config"` in `tome.mod.json` names a schema (`core/settings.h`: boolean,
  integer, number with minimum/maximum/step, string with an `enum` or
  `maxLength`; title, description, default). It's read with the metadata;
  a bad schema is shown on the details page and the mod still runs.
- **On the settings page** each setting takes two rows, so its description
  has room for about three lines (longer ones are cut, `kSettingInfoMax`).
  Integers and numbers have -/+ buttons, or with `"input": "text"` a text
  box like free-text strings: Enter or leaving the page saves what was
  typed (`parse_typed_number`, then `coerce` rounds and clamps it); text
  that isn't a number is dropped and the box shows the value again.
- Values live in `<mod>/config/settings.json`: every setting, plus keys the
  schema doesn't know (kept for other versions of the mod). Invalid values
  fall back to the default with a warning.
- **Hand edits without refreshes:** settings.json isn't part of the watcher's
  fingerprint (Tome's own writes would refresh every mod after each click).
  The mod manager keeps each file's stamp as it last read or wrote it
  (`settings_stamps_`); the watcher's `poll_settings_changed` compares, and
  `reload_settings` reads only the files that differ, applies their valid
  values (a file that doesn't parse keeps the current ones) and tells the
  mods. A full refresh reads the file again only if its stamp changed, so it
  never puts back an older file over a value a mod set meanwhile.
- The mod manager keeps the values under a **lock of its own**
  (`settings_lock_`), loaded before any mod starts: mods read and change them
  from `tome_mod_enable` and other callbacks that run while the manager's
  main lock is held (SRW locks aren't reentrant; asking for the status there
  deadlocked).
- Changes (the menu, `set_setting_*`, the file) reach the mod's
  `on_setting_changed` callbacks on the main thread, one call per key.
- The menu changes booleans and choices with one button, numbers with -/+
  by `step`, and free text in a text box (an `EDITBOX` per row, read with
  `CEditBox::GetText`). What's typed is saved on Enter, and before any
  other click or Esc on the panel. Tome doesn't overwrite a box while it
  has the keyboard focus.

## Keybinds

- `"keybinds"` in `tome.mod.json` declares them (`core/keys.h`); the mod
  manager hands the list to `keybinds.cpp` before mods start, so `on_keybind`
  never calls back into the manager.
- The player's changes go to `tome/keybinds.json` (only keys that differ
  from the default; entries of mods not installed are kept).
- Tome has one binding of its own, listed first: "Open the mod menu"
  (`tome`/`menu`, Ctrl+F10). On the main menu it opens or closes the panel;
  in a game it does what F10 does (`CEscMenu::Reset`, then
  `CGameUI::SetMode(CGameUI.esc_menu, 1)`) and opens Mods.
- Keys come from the game window's `WM_KEYDOWN`/`WM_SYSKEYDOWN` through
  Tome's subclass (`main_thread::set_key_hook`), with modifiers from
  `GetKeyState`. Order: the open mod menu (`mod_menu::on_key`: a new key
  for a keybind, where Esc cancels and Backspace unbinds; Esc, which goes
  back one page and closes the menu from the list; Enter in its text
  boxes); nothing while the focused frame is a `CEditBox` or
  `CChatEditBar` (the chat box); Tome's binding; then every running mod's
  callback for that key, under the guard (each checked against the live
  registrations just before it runs, so one an earlier callback revoked is
  skipped). A key that ran a callback doesn't
  reach the game. Auto-repeat is ignored.
- Conflicts: two bindings with one key, and keys the game uses as far as
  Tome knows (`keys::game_use`: control groups, hero keys, F9-F12, camera,
  command card letters, ...), with the sections of the player's
  `CustomKeys.txt` (next to war3.exe) for the keys it lists. They're shown,
  not refused.

## Mods talking to each other

API version 11 (`intermod.cpp`, tables in `core/intermod.*`). Two ways:

- **Services:** `service_register(name, version, table)` offers a struct of
  function pointers; `service_get(name, version)` returns it (0 = any
  version) and `on_service` reports when it appears or goes away, whatever
  the load order, and at once if it's already there. One provider per name.
  The registration is a revoker like any other, so disabling the provider
  withdraws it and then tells the listeners. A mod that lists the provider
  in `depends` is disabled before it and may keep the pointer; others drop
  it on `available == 0`. Calls through the table run in the caller's
  guard, so a provider's crash is blamed on the caller.
- **Messages:** `subscribe(topic, fn)` and `publish(topic, data, size)`.
  Delivery is synchronous, on the main thread, in subscription order, to
  other mods only, each call guarded (a crashing subscriber is disabled and
  skipped). Publishing a topic from inside its own delivery is refused.
  The data is the publisher's own buffer, valid during the call.

All calls need the main thread (`TOME_ERR_WRONG_THREAD`), and callbacks run
with Tome's lock released, but under the mod manager's during enable and
disable: don't call manager-level functions from them.

## Mod icons

- `"icon"` in `tome.mod.json`: a `.blp` or `.tga` in the mod folder (the
  game can't show PNG). The list shows it left of the mod's name.
- Setting a backdrop's texture at runtime isn't mapped, so each icon gets
  an FDF definition of its own: Tome copies the file into the generated
  overlay (`UI\Tome\Icons\<hash>.blp`) with a BACKDROP definition
  `TomeIcon<hash>` (`ui::icon_fdf`) and loads it with LoadTocFiles, the
  first time a page shows it. The name comes from the file's contents,
  because a definition can't be replaced: a changed icon is a new one.
- Icon frames are made per row (create context = row + 1) on first use and
  hidden on every redraw; a new screen starts a new list.

## Popups

API version 10: `popup`, `popup_close`, `restart_game`.

- **Queue:** mods' and Tome's popups, one shown at a time (`popups.cpp`),
  first come first shown. A pick removes it and calls its `fn` on the
  main thread (guarded); `popup_close` calls it with `TOME_POPUP_CLOSED`;
  disabling the mod drops its popups without a call (a revoker per popup,
  tagged with its id so a finished one drops its revoker).
- **Where:** `frames::popup_parent`: the game UI's root in games (Esc menu
  look), otherwise the glue manager's current screen, or the main menu's
  `ControlLayer` (glue look, DialogWar3's backdrop). None on the loading
  screen: the popup waits, and every new screen (`frames::on_screen`,
  the main menu's Setup) shows it again there.
- **Frames:** `TomePopup.fdf` (`popup::popup_fdf`) is in the generated
  overlay and listed in `Tome.toc` with the mod menu. `TomePopup` is a
  `BUTTON` without backdrops over the whole screen (`SetAllPoints`), so
  clicks don't reach the menu or the game behind it; the backdrop, title
  and text are its children, anchored at runtime with two points each
  (no SetHeight needed). Buttons are separate `TomePopupButton` frames,
  one per slot. Frames can't be destroyed, so each screen gets one set,
  with its own range of create contexts (popup = base, slot i = base + 1
  + i), reused by every later popup there; unused slots are hidden.
- **Layout** (`popup::layout`, unit-tested): up to three options whose
  labels fit share one row; more go in a column, `kColumnPage` (6) to a
  page, with Previous/Next. The height comes from an estimate of the
  message's wrapped lines (code points, color codes left out, at most 16).
- **Keys** (`popups::on_key`, before the mod menu and keybinds): Enter
  picks `default_option`, Esc `cancel_option`, Left/Right turn pages, Alt
  combinations pass (Alt+F4); every other key is swallowed while a popup
  shows.
- **Kept shown:** while a popup shows, `popups::on_frame` (from the
  per-frame hook) shows its set again every 250 ms, with the unused slots
  hidden. Once, the first set made right after a mod menu click stayed
  hidden although it had been shown; it happened in 1 of 13 tries and
  never again, so the cause isn't known. A popup nobody can see must not
  keep taking every key.
- **Restart required:** when a toggle in the mod menu leaves a mod's asset
  changes waiting for a restart (`ModStatus::restart_pending`, newly so),
  the menu asks: Restart Now / Later (`menu::restart_prompt`).
- **Restarting** (`restart.cpp`): `WM_CLOSE` to the game window, so the
  game shuts down as usual (GameShutdown: game end, every mod disabled and
  unloaded). In a game, closing the window shows the Esc menu's Confirm
  Exit panel, so Tome clicks its `ConfirmQuitQuitButton` instead
  (`CControl::Click`, `frames::click`): the player already said yes. Then, from the GameShutdown hook, Tome starts
  `rundll32 "<tome.mix>",TomeRestart <pid>|<exe>|<command line>`: the
  helper waits for the old process to exit, then starts war3.exe with the
  same command line from the game folder. It must wait outside the game:
  a second war3.exe fails with "unable to initialize" before it loads any
  .mix. In the rundll32 process DllMain does nothing else
  (`restart::is_helper_process`); `--kill-at` keeps the export undecorated.
  If the game is still running its message loop 60 s later (it didn't
  close), the restart is called off, so a later exit doesn't restart; the
  check runs from that loop, so a shutdown in progress never cancels it.

## Asset overlay

`loader/src/overlay.cpp` (archives) and `loader/src/core/assets.cpp`
(planning, unit-tested). Two archives in `tome/cache`, opened through Storm:

| Archive | Priority | Contents |
|---------|----------|----------|
| `overlay-N.mpq` (generated) | 15 | Tome's menu FDF and `Tome.toc`; every FDF loaded with `load_fdf` plus its TOC. Paths are Tome's own, so nothing is overridden. |
| `assets-N.mpq` | 12 | The enabled mods' `assets/` folders (or the folder named by `"assets"` in `tome.mod.json`), at the same paths as inside the game's MPQs. |

- **Priorities:** the game opens its base MPQs at 0–3, War3Patch.mpq at 8,
  and campaign and map archives at 13, 14 and 16. Mod assets at 12 override
  the game's files, but a map's own imports still win.
- **One archive for all mods**, written in load order: when two mods ship
  the same path, the later one wins, and the log line counts the overridden
  files. Files are stored uncompressed and streamed from disk in 1 MiB
  chunks (`mpq::ArchiveWriter`), so large asset folders don't need to fit
  in memory. The whole archive is rebuilt on every change, on the main
  thread; the log shows the time it took. At startup, the last session's
  archive is mounted again instead if it was built from the same mods with
  the same files (`tome/cache/assets.json` lists the mods, their assets
  fingerprints and where each file is; an archive with unreadable files
  isn't kept).
- **Swapping:** a change writes a new archive, opens it, then closes the
  old one with SFileCloseArchive. Storm reference-counts archives: a closed
  archive stays alive until the game closes its last file from it (checked
  in Storm's code), so a file the game is still reading or streaming stays
  valid. The old file is deleted once Storm lets go of it, or at the next start.
- **When changes apply** (`assets::plan`, from the mod's `"reload"`):

  | `reload` | Applies |
  |----------|---------|
  | `live` | at the next refresh (toggle, or a file changed in `assets/`) |
  | `map` | when no map is running: right away from the mod menu while the main menu is showing and no map has loaded since it was built (`frames::main_menu_showing`, using the InitNatives hook's map-load count), otherwise at the next map load (`CGameWar3::Reset`, before the map reads its files) or the next CMainMenu Setup, e.g. after quitting a game |
  | `restart` | at the next launch |

  Until then the mod keeps what it has mounted, in its place in the order,
  even if another mod's change rebuilds the archive (its files are copied
  from the old archive). The status says which mods are waiting. Asset
  changes don't affect the mod's DLL, which follows the usual lifecycle.
- **Caches:** files the game has already loaded stay in its own caches.
  "Applies" means later reads see the new files; already-loaded textures
  and models generally update on the next map or restart.
- **Watching:** the watcher's fingerprint covers every file in each mod
  folder except `config/`, so editing an asset triggers a refresh, which
  rescans the folder (`overlay::scan_assets`: names, sizes, write times).
- **Order inside a refresh:** assets are applied before mods are enabled,
  so `tome_mod_enable` can already load a TOC from its own assets. If a DLL
  then fails to load, its assets are removed again.
- **Scripts:** `Scripts\Blizzard.j` (and so `common.j`) can be overridden
  the same way; the game reads them on each map load.
- **Testing:** `TOME_DEBUG_ASSET=<archive path>` logs the size and FNV-1a
  hash of that file as the game sees it after each change. The e2e test
  overrides the main menu button texture with a generated BLP
  (`tests/e2e/make_blp.py`) and `Blizzard.j` with a copy that calls back into
  the map (`tests/e2e/patch_blizzard_j.py`), loads a TOC and FDF shipped in a
  mod's `assets/` from the map script, and plays a game started from the
  main menu to check the `map` and `restart` modes.

## Threads

| Thread | Does |
|--------|------|
| Main (game) | DllMain, InitNatives hook, mod lifecycle after startup, menu, events and chat commands |
| Bootstrap | first mod load, hashing, window discovery; exits |
| Watcher | file notifications and polling; posts to main |
| Short-lived | `post_delayed` timers, `TOME_DEBUG_CLICKS` |

## Testing

- `ctest` in `build/`: unit tests for everything in `loader/src/core` (run under Wine),
  and `tome_hook_runtime_tests` for the hook engine and the guard on real
  code: access violations, illegal instructions, divisions by zero,
  breakpoints, C++ exceptions, stack overflows (twice on one thread), nested
  guards, inner SEH frames unwound, callee-saved registers and FPU state
  restored, and crashing BEFORE/REPLACE handlers and a crash below a nested chain.
- `tools/e2e-test.sh`: six real game launches with a temporary `TOME_HOME`.
  `TOME_DEBUG_CLICKS` items can also be `type:<text>@ms`: Enter, the text
  as WM_CHAR messages, Enter (a line in the in-game chat).
  It covers:
  - build recognition and the signature check;
  - startup errors per mod;
  - natives called from a generated map;
  - live toggling and hot reload;
  - disabled-mod natives returning 0;
  - the frame natives from the map script;
  - JASS strings (arguments, UTF-8 results, null) and code (called inside a
    native, a function returning nothing, a stored callback run at game start,
    an action thread that waits, a code value no native received refused);
  - two mods hooking the same function (order, arguments, revoking on disable);
  - crash isolation with the `crash` fixture (`tests/e2e/mods/crash`, which
    crashes where its `mode.txt` says): in a native called by the map
    script (the script goes on, later calls return 0), a tick handler, a
    REPLACE hook handler (the original still runs), `tome_mod_enable` and
    `tome_mod_load`; each time the mod is stopped and its status says where,
    and turning it off and on retries it;
  - safe mode (sixth launch, `-tomesafe` and `tome/safe`): nothing loads,
    statuses say safe mode, the mod menu enables a mod anyway, state.json
    is unchanged;
  - the asset overlay: a texture override seen through Storm, a TOC and FDF
    from a mod's assets loaded by the map, a `Blizzard.j` override run by
    the game, live removal and re-adding, and (fourth launch: main menu →
    custom game → quit → main menu) `reload: map` waiting during the game
    and applying at the menu while `reload: restart` keeps waiting;
  - the mod menu through simulated clicks, and the hello mod's own button
    (created, clicked, hidden on disable and recreated on enable); a setting
    changed on the settings page (saved, the mod told), a keybind changed on
    the keybinds page with simulated keys and then run, a text setting
    typed into its box, the hello mod's icon, Esc page by page and Ctrl+F10;
    in the fourth launch's game, the Esc menu's Mods button, Esc, Ctrl+F10
    and Done;
  - events: map load (with the map's path) and game start on both start
    paths, ticks, game end; a fifth launch hosts a LAN game against the
    computer (after creating and cancelling a lobby, which must fire
    nothing), checks that Enter opens the chat box after an in-game
    click and exits the program from the game (game end, then the mod's
    disable and unload, process gone);
    game ticks (200 steps = 1 s of game time); a mod enabled mid-game
    getting the game start; on_ui and clicks on the single player screen
    and in a game; `/hello`, `/tome disable|enable` in a game, an unknown
    `/tome` subcommand, and a disabled mod's command not running.

  The map phase needs `tome-mpq` (`-DTOME_BUILD_DEV_TOOLS=ON`). In about one
  run in five it has had a flaky check, not yet identified.
