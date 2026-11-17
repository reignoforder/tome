# Tome architecture

How the loader works today. For plans see `ROADMAP.md`, for game internals
`reverse-engineering.md`, and for the original design discussion `research.md`.

## Repository layout

```
CMakeLists.txt            top level: options, MinGW stdio, subprojects
cmake/                    i686 MinGW toolchain (forwards to sdk/cmake/; sets Wine as the test runner),
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
    log_view.*            /tome logs: tome.log lines as chat lines
    handshake.*           multiplayer handshake: mod-set lines, comparison, lobby state, game report
    game_lock.*           which required mods a multiplayer game locks, and the notes
    command_line.*        splits the command line like argv (for -tomesafe, -tomenopatch)
    hooks.*               argument layouts from mapping signatures; hook chain order and run
    x86.*                 instruction length decoder; moves a function's first instructions
    pe.*                  lays out a PE file like the loader (for the test)
    embedded_mappings.h   mappings/*.json compiled in (generated .cpp)
    mpq.*                 MPQ v1 writer (overlay archives), streaming for large ones
    package.*             .tome packages: names, unpacked folder names, layout check
    zip.*                 zip reader for .tome packages: directory, inflate, CRC, safe names,
                          NTFS name folding (upcase_table.inc from tools/gen-upcase.py)
    assets.*              asset archive paths; which mods' assets to mount, per reload mode
    main_menu_fdf.*       generates TomeModMenu.fdf (main menu and Esc menu layouts)
    mod_menu_model.*      the mod menu's pages and what their buttons do
    mod_status.h          ModStatus, one row of the mod list
    objects.*             game objects: the property table, "property:" requirements, the
                          handle table (an id's object, new ids), jass_call's checks
    settings.*            config.schema.json subset, settings values, menu steps
    signature.*           tome.sig: Ed25519 signatures of mods (Monocypher), base64
    wasm_abi.*            Wasm mods (wasm-mods.md): checked reads/writes of a module's memory, handles
    wasm_layout.*         Wasm mods: the wasm32 structs Tome writes (events, units...), JASS signatures
    wasm_imports.h        Wasm mods: Tome's import list (WAMR signatures) and the modules' limits
    wasm_alloc.*          Wasm mods: the capped heap WAMR allocates from
    keys.*                key names, declared keybinds, keybinds.json, conflicts
    popup.*               popup layout (rows, paged columns), keys, TomePopup.fdf
    crash_report.*        tome/startups, the crash loop popup, crash report text
    timers.*              mods' timers: the schedule on the real and game clocks
    desync.*              desync checks: the checksum actions, comparing players' hashes
    profile.*             mods' times: by frame, the last 128 frames, /tome perf lines
  src/testing/            the loader without the game, not in tome.mix: the fake game image
                          (fake_game.*; tests/host too), tome_test.dll (tome_test.cpp, for
                          mods' tests) and tome-check (tome_check.cpp); "Testing" below
  src/                    Windows side (tome.mix)
    dllmain.cpp           pins the module, calls start_bootstrap
    bootstrap.*           startup sequence (see below); TOME_DEBUG_EXIT: exits once tome/debug-exit appears (coverage runs)
    mappings.*            loads the embedded mapping files, finds the build
    game_build.*, sha256.*  build identification
    patch.*               call-site and pointer patching, verified before writing
    patches.*             built-in patches (fixes, on unless the player turns one off); the registry and version gating
    patches/              one file per patch (memhack_guard: the JASS typecast fix;
                          path_sanitizer: file-writing native paths;
                          blp_guard: malformed BLP textures;
                          d3d_reset_lights: Direct3D lights across a reset;
                          gl_context_restore: the OpenGL context after Alt+Tab)
    natives.*             native registry, thunks, InitNatives hook, JASS strings and code
    objects.*             game objects for mods: checks, properties, handles, jass_call
    hooks.*               inline hooks on mapped functions: patch, entry stub, dispatcher
    mod_manager.*         discovery, state.json, lifecycle, hot reload, crashed mods, safe mode
    mod_handle.h          tome_mod, the handle mods get
    guard.*               crash isolation: an SEH frame around every call into a mod
    timers.*              mods' timers (API 19): run from frames and game steps
    desync.*              desync checks (API 19): checksum rounds, sending, telling the player
    crash_handler.*       crash reports for faults outside mod calls (a filter in front of the game's)
    crash_loop.*          counts starts that don't get going; safe mode after a crash loop
    api.*                 the tome_api table given to mods
    main_thread.*         window-message dispatcher (the main-thread safe point)
    watcher.*             change notifications (+ timestamp polling under Wine: every 1.5 s, or 50x as long as a poll takes)
    storm.*               Storm.dll file/archive functions by ordinal
    overlay.*             the generated overlay and the mod assets archive
    packages.*            unpacking .tome packages into tome/packages, removing unused copies
    frames.*              game frame functions, the frame API for mods, Blz* natives
    events.*              game events, screens and chat commands (Tome's own hooks)
    handshake.*           the multiplayer handshake in the lobby and in games
    mod_menu.*            the mod menu on the main menu and in the Esc menu (frames, hooks, animation)
    settings_api.*        the settings part of the API (values live in the mod manager)
    keybinds.*            keybinds: the keyboard hook, tome/keybinds.json, the API
    popups.*              the popup queue and its frames on the current screen; the API
    restart.*             restarting the game (WM_CLOSE, then a rundll32 helper)
    debug_input.*         TOME_DEBUG_CLICKS: clicks, keys, typed text and chat lines (tests);
                          TOME_DEBUG_SKIP_MOVIES: Esc into each movie's window
    log.*, paths.*        tome.log, game/tome/mods folders (TOME_HOME), moving the old layout
    wasm_mods.*           WebAssembly mods (wasm-mods.md): WAMR, Tome's imports, callbacks, natives, traps
  third_party/monocypher/ Ed25519 checks for core/signature (BSD-2 or CC0, 4.0.3, unmodified)
  third_party/wamr/       WAMR 2.4.5 (Apache-2.0 WITH LLVM-exception): downloaded and patched at configure time; TOME.md
sdk/                      everything for mod makers; README.md is the guide. Builds on its own too (sdk-dist zip)
  include/tome/tome.h     the public C API for mods
  include/tome/utf8.h     UTF-8 helpers (header-only; also used by Tome itself)
  include/tome/tome.hpp   C++17 helpers: Mod::call for any API function (table size check, lambdas as callbacks),
                          exceptions reported as crashes, TOME_EXPORT_MOD; generic, so new API functions need no change
  CMakeLists.txt          includes TomeMod.cmake, builds the example (and rust/ when cargo can)
  TomeMod.cmake           tome_add_mod(), tome_add_rust_mod(), tome_package() (mods outside this repo include it too)
  cmake/                  the i686 MinGW toolchain (cmake/ at the top forwards to it), dist.cmake (the sdk-dist zip)
  docs/tome-mod-json.md   tome.mod.json and config.schema.json, field by field
  schemas/                JSON Schemas of both files, for editors ("$schema")
  templates/              the smallest mod in C, C++ and Rust, to copy; built here so they keep working
  examples/hello/         example mod: natives (numbers, strings, callbacks), buttons on three screens, events, /hello,
                          settings (config.schema.json) and a keybind
  examples/hello-cpp/     the hello example in C++ with tome.hpp (README.md: the C++ guide)
  rust/                   Rust support (README.md): tome-sys (bindgen output of tome.h, committed), tome (the safe
                          layer: closures, Results, version checks, panics reported as crashes), examples/hello-rs
tests/                    unit tests (run under Wine by CTest)
  test_hook_runtime.cpp   the hook engine on real functions (a second test executable)
  e2e/                    fixture mods (hook-a and hook-b are built) and the map-script patcher
tools/
  e2e-test.sh             end-to-end test in the real game
  game.sh                 start/stop/wait/screenshot helper
  test-env.sh             where both run the game: a linked game folder, an own Wine prefix, Xvfb
  container/              Containerfile with the whole toolchain (make container-test, container-e2e)
  gen-upcase.py           generates loader/src/core/upcase_table.inc (NTFS name folding)
  gen-rust-bindings.sh    regenerates sdk/rust/tome-sys/src/bindings.rs from tome.h (--check for the test)
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
   - checks for safe mode (see "Crash isolation and safe mode"), a crash
     loop among the reasons (`crash_loop::check`);
   - identifies the build from the in-memory PE header;
   - checks the build's byte signatures and call sites against the loaded
     module (`verify_loaded_build`); on any mismatch nothing is patched;
   - installs the hooks, each checking the target bytes before patching:
     the 3 InitNatives call sites, the FrameDef.toc call site, two
     CMainMenu vtable slots, the event hooks (`events::install`, through
     the hook manager) and the built-in patches (`patches::install`, see
     "Patches");
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
     signals `mods_ready::set()`, which two of the game's steps wait on
     (≤ 10 s, `mods_ready.cpp`): InitNatives, and the game opening its sound
     device (a BEFORE handler at the highest priority on game.dll's
     `AIL_open_digital_driver` import, installed in DllMain, which runs
     inside Miles' `AIL_startup` just before). Mods are thus loaded and
     enabled before sound starts, and can choose the device; the open call's
     own hook chain is fixed by then, so they act in `tome_mod_load` (e.g. by
     patching Miles) rather than by hooking that import;
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
  are JSON integers or `"0x…"` strings. `doc` is an optional note. A
  function may have a `size` (its length in bytes from its RVA, jump tables
  in it included; at least as long as `bytes`): the functions built-in
  patches reserve have one, so a mod's name for a place inside one is
  reserved too (`hooks::is_reserved`).
- `bytes` is the function's first 5–64 bytes as lowercase hex, `??` for
  bytes the Windows loader relocates (ASLR). `tools/re/signatures.py`
  generates them: at least 16 bytes, extended until the pattern is unique in
  its section. At startup `verify_code` checks every function's `bytes` and
  every call's `E8 rel32` and target against the loaded module, and finds
  every import's slot in the import directory (the names come from the
  lookup table, which the loader leaves alone), before anything is patched. A mismatch logs the symbol and leaves the game
  unmodified. A unit test runs the same check against the local
  `input/<version>/game.dll` (skipped when there's none).
- `"module"` (optional, lowercase, default `"game.dll"`) makes a file
  describe another DLL Tome patches: `mappings/blizzard.ax.json` (the
  movies' decoder). Such a file is kept apart from the game builds (never
  identifies the game, never seen by mods' `resolve`), binds through its
  own function (`bind_blizzard_ax`, `BlizzardAxMappings`), and its patch
  finds it by the loaded DLL's header (`find_blizzard_ax_by_header`) and
  checks it with `verify_loaded_module`. A module Tome doesn't know is an
  error.
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

### Mods' own mappings

A mod can map functions (and any other symbol) Tome doesn't, without
waiting for a Tome release: `tome.mod.json` `"mappings"` names a folder
(default `mappings`, used if it exists) with one file per build,
`<folder>/<build>.json` (`sdk/docs/tome-mod-json.md`, "Mappings").

- **Format:** `mappings::parse_mod_build` (core): the same `symbols` as
  Tome's files, checked by the same `Parser`; the build header is optional,
  and a field that's there must equal the running build's (`mod_header`),
  so a file under the wrong name is refused. RVAs are checked against the
  running build's `size_of_image`. Fuzzed in `fuzz_json`.
- **Names:** `"<mod id>/<name>"` (`split_mod_symbol`; Tome's names never
  have a `/`). The file itself uses the bare names.
- **Discovery** (`ModManager::read_mappings`, cached by the file's stamp):
  an error in the file makes the mod invalid, with the error on its Details
  page. That includes its code: `mod_mappings::verify` runs `verify_code`
  on the file (every function's `bytes`, every call's target, every
  import's slot) against game.dll as it was before Tome patched it (the
  patch functions and the hook manager keep each byte they overwrite,
  `note_original`), so what `resolve` hands out was checked, as Tome's own
  file was at startup. DllMain's scan for the startup assets runs before
  `mod_mappings::init`: it reads the files but checks them on the next
  scan, and a file that wasn't checked isn't published. `"requires"` entries with a `/` are checked once every mod is read
  (`check_mod_requires`), against the mod they name (itself or another,
  e.g. a library), so the order mods are read in doesn't matter; again
  until none fails, since a mod that fails takes its mappings along; and
  once more after `declare_natives`, which can fail a mod too.
- **Publishing:** each refresh hands the tables of the valid mods that are
  on and allowed to `mod_mappings` (`publish_mappings`, after state.json is
  read); the others' are withdrawn, with the reason, which a hook by one of
  their names reports ("not mapped now: the mod lib isn't allowed yet"). A
  locked mod (multiplayer game lock) keeps the table it runs with. A table is never freed, because
  `resolve`'s `tome_symbol*` and the hook manager's lookups point into it;
  only a changed file makes a new one (a table published before, as when
  a mod was invalid for a while, is reused). `hooks::find_or_install` and
  `api_resolve` look a name up in Tome's mapping, then here; a mod's name
  hooked before whose file now maps another address hooks the new one (the
  old patch keeps its handlers, and `unhook` by the name still reaches
  them: `hooks::targets_of` lists every place a name hooked); one whose file
  was withdrawn hooks nothing more. Hooks a mod already has don't move: a
  changed mapping file doesn't reload the mods that use it. The fake game replaces the check (`mod_mappings::set_verifier`): it
  writes the file's function bytes and calls' `E8 rel32` into its image
  (where nothing was written yet, or a mod's file wrote before, never under
  one of Tome's patches), so
  `tome-check` and `tome_test` can hook them, then checks all but the
  imports (its image has no import table).
- **Checks at hook time:** the function's `bytes` against the code again,
  as for Tome's names. Built-in patches' reservations hold by address too
  (`hooks::is_reserved`): a mod's name for a reserved function, or for a
  place inside one (the function's `size`), is refused.
- **Approval:** the files are among those `code_hash` covers, and a mapping
  file alone makes a mod one with code (`CodeKind::Assets`): a library
  without a DLL waits for approval too, and a replaced package's changed
  file asks again, since it moves other mods' hooks.
- **Multiplayer:** `sync_hash` mixes in the mod's own file for this build
  and those of the mods it names in `"depends"` or in `"requires"`
  (`"<id>/<name>"`), so players whose library files differ are told even
  when the library is `"sync": "client"`. A mod that hooks a library's
  names should name the library in one of them.

## Mods

- Folder: `<home>/tome/mods/<folder>/tome.mod.json` (+ optional DLL). `<home>` is
  the game folder unless `TOME_HOME` is set. `tome/mods/config/` isn't a
  mod: it holds each mod's settings (`<id>.json`) and its `config_dir`
  (`<id>/`), by id, so they survive replacing or repackaging the mod.
- Old layout (`<home>/mods/`, settings in `<mod>/config/settings.json`):
  `migrate_layout` (paths.cpp, from DllMain before the first discovery)
  moves the folder to `tome/mods/` if that doesn't exist yet, then each
  mod's `config/settings.json` to `config/<id>.json` and the rest of its
  `config/` to `config/<id>/`. It never overwrites; what it did is logged
  as `Layout:` lines.
- **Editions.** One game.dll runs Reign of Chaos and The Frozen Throne;
  `war3.exe -classic` (what `Warcraft III.exe` starts) is Reign of Chaos.
  The game sets `Game.expansion` (0/1, -1 before) while it opens its
  archives, before Mss32 loads `tome.mix`; `read_edition` (game_build.cpp)
  reads it into `GameBuild.edition`, in DllMain for the startup assets and
  again on the bootstrap thread, and logs it with the build. A mod's
  `"edition"` (`roc`/`tft`, `core/edition.h`) makes the resolver treat it as
  `IncompatibleGame` in the other game ("is for Reign of Chaos, this is The
  Frozen Throne"); without it the mod runs in both. If the value isn't set
  yet (Tome attached before game.dll), every mod counts as fitting. Mods read
  it in `tome_game_info.edition` (API version 15). `tome-check` runs a mod
  in its own edition (`--edition` to choose); the e2e test runs either one
  (`EDITION=roc`, `make e2e roc`). Tome's own UI follows the edition in one
  place: the mod menu's chained panel is the model the game's skin names
  for the edition (`GlueSpriteLayerTopLeft_V<edition>`, looked up with the
  game's `Skin::GetString` when the overlay is built, `mod_menu.cpp`
  `chains_model()`), so a skin that changes the game's panel changes Tome's
  too; without a usable entry, the edition's own model (`ui::default_chains_model`).
- State: `<home>/tome/state.json` (`{"schema":3,"mods":{"id":{"enabled":bool,
  "approved":"<sha256>","approved_from":"<folder>","approved_key":"<base64>"}}}`,
  the key only for a signed mod), parsed and written by
  `core/mod_state.*`. New mods start enabled. Entries for mods that aren't
  installed are kept.
- **Approval of mod code** (`core/mod_state.*`, `ModManager::approval_need`).
  A mod with a DLL is native code with full access to the machine, so its
  code doesn't run until the user turns it on once (the mod menu's Enable, or
  `/tome enable`; `set_enabled` records it). Until then the resolver sees it
  as off, its assets aren't mounted, its DLL isn't even loaded, its icon
  isn't shown, and its status is `approval` with a reason the menu shows as
  "New code" / "Code changed" ("New files" / "Files changed" for assets).
  The log says how to allow it, once.
  - **What counts as code** (`Installed::code_kind`, `mod_state::CodeKind`,
    shown as `ModStatus.code_kind`): a DLL ("runs native code"); game scripts
    among the assets (`mod_state::is_script`: `.j`, `.ai`, and `.pld`, which
    `Preloader` runs), since an asset-only mod that replaces `Blizzard.j` runs
    JASS in every game ("replaces game scripts"); and any other assets or an
    icon ("adds game files"), since the game reads models, textures, sounds
    and FDF with its own 1.27b parsers, which trust sizes in the file (the
    BLP guard fixes one of them): a crafted file could run code without a
    click otherwise. Only a mod with nothing but `tome.mod.json` (and
    settings) needs no approval.
  - **What's hashed:** `Installed::code_hash`, the SHA-256 of a manifest of
    every file in the mod except the top `tome.mod.json` and `tome.sig`. The manifest is
    `sha256sum` output sorted by path (`<hash>  <path>\n`), so it can be
    checked by hand. Both are kept across starts in
    `tome/cache/file-hashes.json` (`core/hash_cache.h`): the folder's hash
    until a file's name, size or times change (so a start where nothing
    changed opens none of the mod's files; the startup mount runs in
    DllMain), and each file's SHA-256 until its size, times or file id
    change, so only new or changed files are read again. The first refresh
    saves the cache, not DllMain. A file that can't be read gives ""
    (never allowed). The details page shows
    it as "Code SHA-256".
  - **When it asks again:** a folder mod in the folder it was allowed in
    never does, so a mod being built into `tome/mods/` hot-reloads as before.
    Anything else (a replaced `.tome`, or another folder or package with the
    same id) runs only if its code hash is the one allowed.
  - **Older `state.json`:** approvals of schema 1 and 2 aren't carried
    over (they hashed other files): those mods ask again, and the file is
    written as schema 3. Mods it doesn't list ask. An unparseable
    `state.json` allows nothing (and still isn't rewritten).
  - **DllMain:** `mount_startup_assets` applies the same approvals in memory
    (`implicit_approvals`, quietly); the first refresh records them.
  - **Tests:** `TOME_DEBUG_APPROVE` (ids, comma-separated, or `*`) allows
    those mods' code without a click, logged as `code allowed
    (TOME_DEBUG_APPROVE)`. The e2e test lists its mods with code and keeps an
    `unapproved` fixture whose "DLL" is text, to show it's never loaded.
  - **Signatures** (`core/signature.*`, `ModManager::check_signature`): a
    `tome.sig` at the top of the mod (`{"schema":1,"key":…,"signature":…}`,
    base64, at most 4 KiB) is an Ed25519 signature by the author over
    `tome-signature-v1\n<id>\n<code hash>\n<SHA-256 of tome.mod.json>\n`,
    checked with Monocypher (`loader/third_party/monocypher`, BSD-2/CC0)
    after the code hash; a key of small order is refused (anyone can sign
    for one). A valid one sets `Installed::signer`; a malformed, oversized
    or non-matching one is a warning and counts as unsigned. `approve`
    records the signer with the hash. `mod_state::need`: code that differs
    from what was allowed runs if what was allowed was signed and this is
    signed by the same key (an update from the same author; the next refresh
    records it, "signed by the same key as before"); if it isn't, or by
    another key, it's `SignerChanged`, which the menu shows in red ("Signer
    changed") with a warning. Code allowed unsigned stays `Changed`, however
    it's signed now. The details page shows "Signed by". Signed by
    `sdk/support/tome_sign.py` (RFC 8032's reference Ed25519 in plain Python,
    with its test vectors as `selftest`) or `tome_package(SIGN_KEY)`, which
    signs a staged copy of exactly the packed files. Tests: the RFC vectors
    and a Python-made signature in unit tests; host tests sign the crash
    fixture (Monocypher in-process; `tome_sign.py` on a folder with
    subfolders and non-ASCII names; a `tome_package(SIGN_KEY)` package) with
    `tests/host/test-signing.key`, a public test-only key; `fuzz_signature`.
  - **Not a boundary against local programs:** anything that can write
    `tome/mods/` can also write `state.json` or replace `tome.mix`. This is
    about mods that arrive by download or are passed around, not malware
    already on the machine.
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

### The mod list and versions for mods (API version 20)

For mods that manage other mods (an updater, a mod browser). `list_mods`
hands out the rows of the mod menu's list: id, name, version, folder,
files' folder, state (`tome_mod_state`, from `ModStatus::state`), the
reason it isn't running, whether the player wants it, package, sync,
restart pending, `provides`. The end of each refresh copies `status_` into
an immutable snapshot (`core/mod_list.*`, unit-tested: the `tome_mod_info`
structs and the strings they point to, struct sizes older and newer than
Tome's) under a lock of its own, never held while taking another, so
`list_mods` works from any thread and from callbacks that run under the
mod manager's lock (a mod's enable). A mod's handle keeps the snapshot it
was last given (`tome_mod::listed_mods`): its strings stay valid until it
asks again, while refreshes make new ones. When a snapshot differs from the
last in what mods see, `locked()` posts `TOME_EVENT_MODS_CHANGED` to the
main thread once the lock is released (handlers may list or toggle mods).
The first refresh at startup runs on the bootstrap thread before the
dispatcher exists, and its mods are enabled before its snapshot is made
(`list_mods` gives them nothing in `tome_mod_enable`): bootstrap posts its
event once the dispatcher is installed. `mods_dir` is `tome/mods`; a mod installs
by moving a finished folder or `.tome` in, and the watcher does the rest,
approval included.

`check_metadata` runs what discovery and the resolver run on an installed
mod over a release's `tome.mod.json` (`core/metadata_check.*`,
unit-tested, in the JSON fuzz target): the parse, `compatible()` (the
resolver's game, edition and Tome check, shared) and `unmet_requires`
(shared with discovery) against the running build's mappings. Not
`depends`: that needs the installed set, which the mod has from
`list_mods`.

`version_compare` and `version_match` are `core/version.*`'s own parser
and matcher (`compare_versions`, `match_version`): the rules Tome checks
`tome.mod.json` with, so a mod that picks a release to install agrees with
the resolver that will run it. Both kinds: semver (`depends`, `tome`) and
game versions (`game`, with the known names like `1.27b`). Also WebAssembly
imports, like `check_metadata`; `list_mods` and `mods_dir` aren't
(pointers, paths). The Rust crate wraps them in `sdk/rust/tome/src/mods.rs`.

## Packages

`.tome` files: one file per mod for distribution; folders stay the way to
develop one. `core/package.*`, `core/zip.*` (portable, unit-tested) and
`packages.*`.

- **Format:** a zip of what would be the mod's folder, `tome.mod.json` at
  the top (a package of the folder itself, `hello/tome.mod.json`, is refused
  with a hint). Stored or deflated entries; no zip64, encryption or other
  methods. At most 256 MiB on disk, 512 MiB and 20000 entries unpacked.
- **Names:** `zip::read_directory` refuses names that could leave the
  folder (`..`, absolute, drive letters, `\`, device names like `nul` or
  `con .txt`, characters Windows refuses or drops, a trailing space or dot,
  invalid UTF-8), two names that are one file on NTFS (`fold_name`: each
  BMP code point through its simple uppercase mapping, the table generated
  by `tools/gen-upcase.py`; no normalization, as NTFS) and a name that's
  both a file and a folder. The unpacker also creates every file with
  `CREATE_NEW`, so a collision the table misses fails instead of
  overwriting.
- **Unpacking:** `discover()` lists `tome/mods/*.tome` next to the folders.
  A package's `Installed::folder` is its file name (`hello.tome`), its
  `dir` the unpacked copy, `tome/packages/<stem>.<FNV-1a of the file>/`.
  The copy is written to `<name>.<pid>.partial` and renamed when complete,
  so a folder with the final name is always whole and is reused (another
  launch, the same file again). Unpacked again only when the file's stamp
  changes (`package_cache_`). Under the mod manager's lock, on the thread
  that refreshes (DllMain at startup, then the main thread): a large
  package holds the game for its unpack (about 100 MB/s under Wine).
- **Everything else is a folder mod:** the DLL's shadow copy, assets, FDF,
  the icon and the settings schema come from the unpacked copy. Settings and
  `config_dir` are by id in `tome/mods/config/`, so an update keeps them.
- **Updates:** a changed `.tome` unpacks into a new folder, so the DLL path
  changes and the mod is reloaded like a hot reload.
- **Cleanup:** after each refresh, `packages::remove_unused` deletes every
  unpacked or partial folder in `tome/packages` except the installed
  packages' copies and the folders loaded mods came from (a locked mod in a
  multiplayer game keeps its copy). One that can't be deleted (a helper DLL
  loaded from it) goes on a later refresh. Partial folders of another
  running process are left alone.
- **Same id:** a folder and a package with the same id: the package gets the
  error "the folder <f> has the same id and is used instead", so a mod can
  be worked on while it's also installed packed. Two packages with one id
  are a duplicate id, as two folders are.
- **Watching:** the fingerprint covers each `.tome`'s name, size and time;
  the unpacked copies aren't watched (Tome's own).
- **Handshake:** the sync hash is over the unpacked files, by name, so a
  package and the same mod as a folder match.
- **Menu:** the Details page says `Package: tome\mods\hello.tome`
  (`ModStatus::package`); the icon comes from `ModStatus::dir`.
- **Packing:** `tome_package(<target> [FILES ...])` (`sdk/TomeMod.cmake`)
  makes `<build>/packages/<folder>.tome` from the folder `tome_add_mod`
  assembled, with `cmake -E tar --format=zip` run inside it (entries
  without `./`). Any zip tool works if the entries start at the mod
  folder's contents.

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
  (`__fastfail`, `/GS` failures, heap corruption detection). The first two
  get a crash report (below); the game still ends.

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

### Crash loops

`crash_loop.cpp`, `core/crash_report.*`. A mod that crashes the game
before the main menu (an asset the game can't load, a thread of its own,
memory it broke) would otherwise mean deleting `tome.mix` by hand.

- **Counting:** in DllMain, once the main menu hook is installed (so
  getting there can be seen), `count_start` adds 1 to `tome/startups`
  (`<count>\n<report stem>\n`, written atomically). The main menu's Setup or
  a game starting (`-loadfile` and the World Editor's Test Map never show
  the menu), followed by `kGoodAfterMs` (5 s) of the game running on, sets
  it back: the file is deleted. A start closed before then counts as failed
  too. The World Editor and builds without the menu hook don't count.
- **The loop:** `crash_loop::check` (DllMain, in `safe_mode_reason`) reads
  the count; 3 (`kStartsBeforeSafeMode`) or more turns safe mode on with the
  reason `the last 3 starts didn't reach the main menu`. That start counts
  too, so a crash in safe mode keeps it there.
- **The offer:** once that start gets going, a popup (Tome's own, through
  `popups::show`) says what happened, names the last crash among those
  starts if Tome reported one (the report's summary line; `note_crash`
  records its stem in `tome/startups`), and offers **Keep Safe Mode**
  (creates `tome/safe`, turned off with the mod menu's Normal Mode) or
  **This Start Only** (Enter, Esc). Not shown if `tome/safe` is there already.

### Crash reports

`crash_handler.cpp`, `core/crash_report.*`. For faults outside the guarded
calls: a mod's own threads, the game's code, Tome's own code.

- **Where:** game.dll sets an unhandled-exception filter of its own early in
  its main function, before Miles loads `tome.mix` (`CrashHandler::Install`,
  `reverse-engineering.md`, "Crash handler"). Tome's goes in front of it in
  DllMain (`SetUnhandledExceptionFilter`, which returns the game's), writes
  its report, then calls the game's, which writes the game's own error log
  and starts BlizzardError.exe as before. The main menu checks that nothing
  replaced it since (`check_filter`) and goes in front again if something
  did. Installed only when the build's signatures match, like every other
  change Tome makes.
- **The writer:** a thread of Tome's own (started at bootstrap, with
  `MiniDumpWriteDump` from the system's `dbghelp.dll` looked up then): the
  crashed thread may have no stack left or hold a lock. The filter hands it
  the exception and waits up to 15 s (`kWriteTimeoutMs`); a stuck writer
  (a lock the crash left held, such as the heap's) just means no report.
  One report at a time; a second thread crashing meanwhile waits for that
  report, then goes to the game's filter unreported; a fault while writing
  (in the writer or the crashed thread) goes straight to the game's filter.
  A filter that `check_filter` put Tome's in front of and that chains back
  to Tome's (the one it replaced) gets the game's filter once, not another
  report: a per-thread depth (a TLS slot) tells Tome it's been there.
- **What:** `tome/crashes/crash-<yyyymmdd-hhmmss>.txt` and `.dmp` (the
  five newest are kept). The text: a summary line (`access violation
  writing 0x00000000 at crash.1.dll+0x1420 (mod crash)`), the exception,
  the thread (and whether it's the game's main thread), registers, likely
  callers (return addresses on the stack, as the guard finds them), the
  mods as of the last refresh (`set_mods`, with whose DLL is whose), and
  the last 80 lines of `tome.log`. The minidump has the referenced memory,
  thread data and unloaded modules. A line goes into `tome.log` too, through
  `log::try_write`, which gives up instead of waiting for a lock the crashed
  thread may hold.

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
  frame natives below. A map's log lines are untrusted: each map load logs
  at most 1000 of them (then one line saying the rest aren't, and at the next
  map load how many weren't), and a string is cut to 1000 bytes with its
  control characters replaced by `?`, so it can't forge a line
  (`core/log_guard.*`). `tome.log` itself stops taking info and debug lines
  at 32 MiB and everything at 33 MiB, with a notice the first time.

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

## Game objects (API version 16)

`loader/src/objects.cpp`, with the portable parts in `core/objects.*`
(`tome.h`, "Game objects"). Mods get the game's objects (`tome_unit.object`
in events and the selection, `object_of`, `player_object`) and read and
change them through Tome, which calls the same game functions the JASS
natives call, found by reading each native's code on both builds
(`reverse-engineering.md`, "Game objects").

- **Every object is checked before use** (`objects::type_of`): a player
  is one of `CGameWar3::GetPlayer`'s results (compared, never read); a
  unit, item or destructable has the class's vtable and an agent record
  that says it's alive (`ObjectLookup` on `CAgent.id`, the record's tag
  `'+agl'`, not dying): what the unit natives check. A gone object fails
  the call with `TOME_ERR_INVALID_ARGUMENT`.
- **Properties** (`get_property`, `set_property`, by `tome_property`, all
  values doubles) are a table in `core/objects.cpp`: the kinds of object
  each is on, whether it can be set, and the mapped names it needs. At
  install Tome marks each property available if all its names are mapped
  (`objects` in `BuildMappings`, all optional), so a build that lacks one
  fails just that property with `TOME_ERR_UNAVAILABLE` and logs "Game
  objects: N of M properties mapped". `tome.mod.json` `"requires":
  ["property:<name>"]` is checked against the same table when mods are
  read (`capability_met`): the first capability Tome checks.
- **Keys:** some properties take a key (`get_keyed_property`): an ability's
  type id (required), a hero attribute's base (0) or total with bonuses
  (1, read only), or a unit's weapon (0 or 1). The plain functions pass 0.
  `PropertyInfo` says what each takes (`Key`), where it can be read (`on`)
  and set (`set_on`, e.g. x and y on units only), and whether only heroes
  have it; `check_access` decides from that.
- **Listing objects** (`list_objects`): units, items and destructables
  through the game's own walker (ForEachAgent with the class's type tag,
  what GroupEnumUnitsOfPlayer uses; items and destructables with its
  subclass walk, filtered by vtable), so it lists every live one, handle or
  not, in the game's order; a callback of Tome's collects them and the mod
  gets an array, so no mod code runs during the walk. Players: every slot
  CGameWar3::GetPlayer gives.
- **Changing the game** from one computer only desyncs a multiplayer game.
  `set_property` refuses (`TOME_ERR_UNAVAILABLE`, a warning once per mod)
  while the game lock is on (a multiplayer game or replay,
  `core/game_lock.h`) unless the mod is `"sync": "required"`
  (`tome_mod.sync_required`, set at each resolve).
- **Handles:** `object_of` reads the handle table itself
  (`core/objects.cpp`, `handle_object`), checking the id against the
  number handed out (`HandleManager.count`) and the entry's reference
  count; the game's own lookup (`0x285FE0`) checks only the lower bound.
  `handle_of` is the existing read-only lookup (`units::handle_of`), which
  never makes a handle.
- **`jass_call`** runs any native by name. An AFTER handler on `BindNative`
  (`last`, after mods' handlers) records each native's address and
  signature as the engine binds it at every map load: the game's (with the
  path sanitizer's wrappers where it put them), Tome's and mods' thunks. The
  arguments are converted by the signature (`wasm::parse_signature`): reals
  go as pointers to floats, strings are made with `CreateJassString` and
  passed as their string table entry (`JassVM::Get`, `JassVM.strings`:
  16-byte entries, an RCString each; made first and looked up last, since
  making one may move the table), handles are checked against the handle
  table, and code values must be ones the map script passed to a Tome
  native (`natives::code_seen`). Types made by a `Convert*` native from one
  integer (`unitstate`, `playerstate`, `race`...) are integers, not table
  entries: they're learned from the same BindNative calls. The call goes
  through `hooks::call_with_stack` (cdecl); a real result comes back as
  float bits, a string result as an id whose entry's text Tome returns.
- **Multiplayer:** a native that returns an agent may make its first
  handle (`0x282E30` with 1), which shifts every later id on that computer,
  and Tome can't tell which natives only read. So in a locked game (a
  multiplayer game or a replay) `jass_call` refuses mods that aren't
  required, as `set_property` does (warned once per mod).
- **Texttags** are indices (0 to 99), not handle table entries: every
  texttag native checks the id against the texttag count itself, so
  `jass_call` passes them as integers (`objects::handle_is_index`). Images
  and ubersplats are indices too, but their natives index an array with
  the id unchecked: those stay refused (not live handles).
- **Handle types:** besides being live, a handle argument must fit its
  parameter (`handle_type_fits`): unit, item, destructable and player
  take their own kind, widget any of the three, handle and agent anything,
  other types only objects Tome doesn't classify, which the game's
  converter then type-checks itself (`reverse-engineering.md`).
- **WebAssembly mods** get all of it, `jass_call` too, with strings copied
  in and out of the module's memory (`wasm-mods.md`).

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
| `CGlueMgr::SetScreen` (after) | maps the GlueScreen id to a `tome_ui_event` and calls `frames::on_screen` with `[mgr+CGlueMgr.screen]`; the main menu keeps its Setup hook. The loading screen is the map load: it reads the map's name and path, applies `reload: map` asset changes (`ModManager::on_map_load`, `assets::Moment::MapLoad`) and fires `TOME_EVENT_MAP_LOAD`. The natives are bound by then (CGameWar3::Reset), and the map's files aren't read yet, except its scripts: the game compiles `common.j`, `Blizzard.j` and `war3map.j` when the map is picked (twice with `-loadfile`), so a `reload: map` change to a script applies from the next map. The score screen ends the game. |
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
  <id>|reload [id]|logs [warn|error] [count]` answers through
  `CGameUI::DisplayText`; `logs` shows the last lines of `tome.log` (10 by
  default, up to 30; `warn` keeps warnings and errors, `error` errors only;
  `core/log_view` makes them chat lines, the level colored); enable,
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

## Patches

`loader/src/patches.*` and `loader/src/patches/`. Patches are Tome's own
fixes for game bugs and vulnerabilities, compiled into `tome.mix` and applied
once at startup. Unlike mods they aren't discovered or removed, and no mod
can turn them off, so a fix like the JASS "memhack" typecast is in force
unless the player turns it off — the reason to make a fix a patch rather than
ship it as a mod.

- **A patch** is a `patches::Patch`: a name, a one-line summary, its
  `variants` (below), the mapping `symbols` it needs, an optional
  `needed(report)` check of where the game runs (the Wine-only fixes; a
  patch that isn't needed is counted and logged as skipped, not applied),
  and an `install(base, build, variant, report)`. Each lives in its
  own file under `patches/` and is listed in `patches.cpp`'s `kAll` table
  (an `extern const Patch`, so the registry references it across translation
  units without static-initializer ordering). A patch installs through the
  **hook manager** (`hooks::find_or_install` + `hooks::add`) with no mod and
  no gate, exactly like Tome's event handlers, at the highest BEFORE / lowest
  AFTER priority so it wraps mods' handlers.
- **The player can.** Each patch has a setting in Tome > Settings,
  `patch.<name>` (on by default; `core/tome_settings.*` `with_patches`, after
  Tome's other settings, titled "Patch: <name>" with the patch's summary;
  none for a patch with a setting of its own name, widescreen),
  and `-tomenopatch=<name>[,<name>...]` (any case, repeatable, `all` for
  every patch; `command_line::list_values`) turns patches off for one start.
  Patches install in DllMain, before the bootstrap thread reads the
  settings, so DllMain reads `tome/settings.json` and the option first
  (`tome_settings::load`, no logging; the option even when Tome's folders
  can't be found), and `patches::install` asks
  `tome_settings::patch_off` for each patch before anything else: one turned
  off is noted as `<name> off (turned off in Tome > Settings | turned off by
  -tomenopatch)`, counted as "turned off", and neither installed nor
  reserved. `patches::outcome` keeps what install did with each patch
  (applied, skipped, off, failed, and why); the "Patches:" line is a
  warning when one is off, and `tome_settings::init` logs any
  `-tomenopatch` name that isn't a patch. The menu's description of each
  switch starts with that outcome ("In force." / "Not in force now: ..."),
  since the switch itself says what the next start does: a change applies
  from then (patches aren't uninstalled).
- **Multiplayer.** A patch that changes what map scripts do (`Patch::sync`:
  memhack-guard, execute-func-guard, global-string-guard, jass-token-guard,
  path-sanitizer) desyncs a game when players differ on it. Each one not in
  force joins the handshake's set like a required mod,
  `patch:<name> off #0` (`handshake::patch_not_in_force`; the `:` keeps it
  apart from mod ids), so the host refuses to start and the in-game report
  names the player. A host with such a patch off also waits for an answer
  from players without Tome, as with a required mod; a client with one off
  sends its set to a quiet host unasked (below). A player without Tome
  next to a host with every patch in force isn't detected: nothing in the
  lobby tells it from a Tome player with nothing to report. Either way the host
  can start anyway by pressing Start again (below).
- **Mods can't disable a patch.** A patch lists the functions it `reserve`s
  (and each variant those only it hooks, e.g. memhack-guard's constructor);
  `patches::install` calls `hooks::reserve(name)` for each once the patch is
  live, and the mod-facing `hook`/`unhook` API then refuses those names with
  `TOME_ERR_NOT_OWNER`. So a mod can't drop the patch's handlers or slip in one
  that interferes (e.g. a REPLACE on the scanner that suppresses the
  `endfunction` token so the restore never fires). Tome's own internal
  add/remove take a `Target*`, not a name, so they're unaffected. This is an
  API-level boundary: a mod is native code and could still write process memory
  directly, which Tome can't prevent for an in-process plugin — the same trust
  as loading any native DLL. The reservation closes the only *sanctioned* way
  to interfere.
- **Version targeting is capability gating, not a version string.** The patch
  manager skips a patch unless the running build maps every symbol in
  `symbols` and its variant's. Those symbols are the
  same ones the mapping database already verifies against the loaded `game.dll`
  by byte signature at startup, and the hook re-checks the target bytes before
  patching. So a patch runs only where its functions are present and match,
  and automatically covers any later build whose mapping defines the same
  symbols — no per-build copy of the patch needed.
- **Variants: what differs per build stays with the patch.** A
  `patches::Variant` names builds (`mappings/<build>.json` "name"), the
  extra symbols the patch needs there, and a pointer to the patch's own
  struct of what it does differently (`params`), or a `skip` reason when the
  patch isn't for those builds. The manager picks the variant naming the
  running build, else the default (no builds); without one the patch is
  skipped as "not for <build>", and a `skip` variant logs "not for <build>:
  <reason>". `install` gets the chosen variant. Today: memhack-guard's
  constructor (`JassLocalVarDesc::ctor`, or on 1.26a the shared
  `JassVarDesc::ctor` with the kind as an argument), blp-guard's header-copy
  site (1.27b's 7 bytes, 1.26a's 9 with a `mov edi,eax` it keeps), and
  ambient-sound's 1.26a skip (that build still makes the MIDI sound). A
  patch with no variants gets one that adds nothing.
- **When:** `patches::install` runs from DllMain right after
  `natives::install_hooks` (bootstrap.cpp), so the fixes are in place before
  the game compiles any map script. It doesn't depend on Tome's folders or on
  `events`; if game.dll loads after Tome, bootstrap logs a warning that the
  patches stay off. It always returns true: a patch that doesn't fit or fails to install
  is counted and described, not fatal. Because DllMain runs before the
  bootstrap thread opens `tome.log`, the per-patch outcome is collected into
  the returned `report` string (applied/skipped/failed with the reason), which
  bootstrap logs once the file is open — as `events` does with its report.
- **memhack-guard** (`patches/memhack_guard.cpp`, `docs/known-bugs.md`): the
  one patch today. It hooks `JassLocalVarDesc::ctor` (BEFORE) to remember a
  global's type descriptor whenever a local shadows it, and
  `JassScanner::nextToken` (AFTER) to restore every shadowed global's
  descriptor at the `endfunction` token, closing the local/global type-shadow
  typecast with no change to legitimate code. It also hooks `Jass::compile`
  (BEFORE and AFTER) to drop what a compile that stopped inside a function
  left saved. The separate map-script *scanner*
  that reports the same pattern in the log is not a patch: it runs at map load
  in `events.cpp` (`core/jass.cpp`, `scan_shadow_exploit`), and also counts
  `common.j` and `Blizzard.j` globals. It logs what it finds, tells the player
  at game start (`display_text`), and gives mods the count in
  `tome_event_info.script_shadows` and the details through
  `map_script_shadows` (API version 13).
- **blp-guard** (`patches/blp_guard.cpp`, `docs/known-bugs.md`): refuses a
  malformed BLP texture before the game's loader decodes it. The 1.27b BLP1
  loader reads each mipmap's span, and a JPEG texture's shared JPEG header,
  out of the file using offsets and sizes from the file's own header without
  checking them against its length, so a crafted `.blp` (a downloaded map's,
  or a mod's asset) makes it read past the buffer — a crash and an
  out-of-bounds read. Tome can't decode BLPs itself and the bug is the
  game's, so the guard hooks `CBLPFile::Open` (the one BLP1 header parser;
  every texture load reaches it). Open loads the whole file and stores the
  buffer and its length in the object (`CBLPFile.buffer` / `.size`); an AFTER
  handler at `INT32_MIN` reads them back and runs the portable validator
  (`core/blp.*`) over the bytes. When the file is unsafe to decode it sets
  the result to 0 — exactly what Open returns for a wrong-magic file — so the
  caller skips the mip/JPEG decode and the texture just fails to load. The
  validator parses the BLP1 header itself (magic, dimensions, the 16 mip
  offset/size slots, the JPEG-header size) so it doesn't depend on the
  game's in-memory layout; it's unit-tested (`tests/test_blp.cpp`) and
  fuzzed. Unlike the path sanitizer it reserves `CBLPFile::Open`: a mod that
  dropped the hook would reopen the hole for every texture, so the mod-facing
  API refuses it. Open also copies a fixed 1180-byte header out of the file
  before it checks the magic, so a file shorter than that is over-read before
  the AFTER handler ever runs; the patch additionally rewrites that copy site
  (`CBLPFile.header_copy`) to an absolute indirect call to a small detour
  (`tome_blp_header_copy`) that copies only `min(1180, size)` bytes — the
  length Open just stored — and zero-fills the rest, so no file is read past
  its end. That rewrite is best-effort and keyed to the exact site bytes
  (`patch_bytes` refuses a mismatch): on a build that doesn't map the site or
  whose bytes differ it's skipped, logged as "unclamped", and the AFTER guard
  alone applies. Covers maps and mods alike.
- **background-mute** (`patches/background_mute.cpp`, `docs/known-bugs.md`,
  "Sound in the background"): the game keeps playing its sound after
  Alt+Tab. The main-thread dispatcher passes the game window's
  `WM_ACTIVATEAPP` to the patch (`main_thread::set_activate_hook`), which
  mutes the game process's default Core Audio session (`ISimpleAudioVolume`)
  on leaving and unmutes it on coming back, leaving a mute the user set alone
  (`core/background_mute.*`, unit-tested). Windows keeps a session's mute
  for the next run, so `tome/background-mute.flag` marks Tome's mute and
  `patches::background_mute_start` (bootstrap, on the main thread) undoes a
  mute that a crashed run left. Needs no game.dll symbols, so it covers any
  build and runs everywhere. Reserves nothing.
- **ambient-sound** (`patches/ambient_sound.cpp`, `docs/known-bugs.md`,
  "Ambient sound"): 1.27a turned off maps' day and night ambience:
  `CreateMIDISound` calls `CSound::InitMIDI`, which now makes a plain sound
  from the label, and MIDI labels aren't in the plain tables, so the sound
  stays empty. The innermost REPLACE handler there (`Handler.last`,
  so mods' BEFORE and REPLACE handlers still run first) makes it with the
  game's own MIDI constructor (`CreateMIDISnd`) into `CSound.midi`, with the
  ambient category (`SoundCategory.ambient`, so the Ambient sounds option
  still applies), in place of the game's body; a label that isn't a MIDI one
  hands on to it. Only what each player hears changes. Reserves nothing.
- **d3d-reset-lights** (`patches/d3d_reset_lights.cpp`, `docs/known-bugs.md`,
  "Under Wine"): works around a wined3d bug that leaks the active-light slots
  of every enabled light on `IDirect3DDevice9::Reset`, so the lights the game
  enables again after Alt+Tab in Direct3D fullscreen are refused and the
  scene gets darker each time. A BEFORE handler on
  `CGxDeviceD3d::ReleaseForReset` calls `LightEnable(i, FALSE)` for lights
  0–7 on the device at `CGxDeviceD3d.d3d_device` when it's called with 0
  (right before Reset; the teardown's call with 1 is left alone), which frees
  the slots; `RestoreState` enables them again. Skipped outside Wine
  (`needed`). Reserves nothing.
- **widescreen** (`patches/widescreen.cpp`, `docs/known-bugs.md`,
  "Stretched picture on widescreen"; addresses in `docs/reverse-engineering.md`,
  "The 2D UI's coordinates"): 1.27b lays everything out for 4:3 and stretches
  it to the window. Two parts.
  - The 2D UI, where its symbols are mapped (1.27b and 1.26a; otherwise
    skipped and reported). The six UI-to-screen conversions (`DDCToNDC`,
    `NDCToDDC` and their `Rect` and `Width` forms, which divide or multiply x
    by the 0.8 at `DDCWidth`) get the window's width in UI units instead
    (`widescreen::ui_width`: 0.6 x the aspect): their `[disp32]` operands
    (found by `widescreen::constant_operands`, one or two per function: eight
    on 1.27b, six on 1.26a) point at a float of Tome's (1.27b), or a double
    (1.26a's x87 code reads 0.8f widened to a double), set on each new screen and each `WM_SIZE`
    (`main_thread::set_size_hook`), so they run as the game's code with no
    hook; `DDCWidth` itself stays (other code reads it). UI units are square and the
    root is the whole window: every frame and text keeps its shape, frames
    anchored to the screen's edges go to the window's edges (1.29's layout),
    and the mouse follows. In a game, `follow_ui` lays the rest out again on
    every frame (from the world camera's setup): the console's bottom tiles go
    off the screen (moved by -10; neither a width of 0 nor clearing their
    points stopped them drawing) and Tome's pieces replace them
    (`widescreen::console_layout`, `console_fdf`, unit-tested: SIMPLEFRAMEs
    from `UI\FrameDef\UI\TomeWideConsole.fdf` in Tome's overlay, made with
    `CreateSimpleFrame` as the console's children, shown and hidden with it);
    whatever the game put in the middle of a 4:3 screen moves to the middle
    (frames on the bottom corners of the console, the second console and the
    portrait's and minimap's layers between x 0.19 and 0.61 at 4:3, the clock,
    two strings off the resource bar, the inventory cover, which the game
    places on the screen rather than on a frame and gets re-anchored); each
    change is made from what the game last set, so nothing moves twice and
    what the game builds later (the life and mana text) is caught. The
    middle moves only once the console's four bottom tiles are found: a map
    with its own `ConsoleUI.fdf` (no such tiles after 30 frames) keeps its
    console and middle as it made them. Points are read only if they're
    relative to a frame (`CLayoutFrame.relative_point_vtable`; one relative
    to the screen holds x where the frame would be). The menus fill the
    window as in 1.29: their frames go to the window's edges as the game
    anchors them, and their panels, the glue manager's sprite layers (models
    made for 4:3), are drawn in proportion on their side: BEFORE
    `C44Matrix::Ortho`, the top-left layer's flat camera is left as it is
    (the model at the window's left), the top-right one's moves so the model
    ends at the window's right (`CGlueMgr.sprite_top_left`,
    `sprite_top_right`), and another full-screen model on a menu (Battle.net's
    center panel) goes to the middle. The loading and score screens (one
    piece of art each, with frames placed on it) are laid out at 4:3 in the
    middle: the screen's frame (`CGlueMgr.screen`) and its full-screen models
    drawn flat are held by their corners in a box 0.8 wide (a model boxed
    itself gets its camera moved by the box's margin: it's placed from the
    root's left edge, its camera from its frame's). Behind the art, the
    window's sides show the same art stretched and darkened: BEFORE
    `CSpriteFrame::Render` and `CModelFrame::Render`, the first model in a
    frame whose rect is the box (`CLayoutFrame.vtable.GetRect`) is rendered
    into each side (`GxXformSetViewport`, put back from `CGxDevice.viewport`)
    with a camera that stretches it over the window, 24 times, shifted
    across a disc and at alphas that average the copies at 40% brightness
    (`CFrame.alpha`, `CFrame.vtable.ApplyAlpha`): a blur. Then it's set up
    again for the game's own render in the box. The root stays the
    window: the mouse, the cursor, and the game's UI (made under the loading
    screen). Frames are changed only
    while the game uses them (a model while its camera is set up, the
    screen's frame while it's current), since a screen's models may be freed
    before the next screen is made. The box is latched with the setting in
    BEFORE `CGlueMgr::SetScreen`. Until boxed (their first frame), full-screen models (a sprite frame whose
    corners are its parent's, up to the root; `fills_parent`) get the game's
    0.8 across their flat camera (BEFORE `C44Matrix::Ortho`), so they're
    stretched as the game draws them and the frames on them line up. The
    cursor's frame (`CCursorFrame.vtable`) fills the screen too but is left
    alone: its model is drawn at the pointer. Roots
    take the screen's width again before and after `CGlueMgr::SetScreen` and,
    in a game, after the setting changes (`CLayoutFrame.vtable.UpdateScreenRect`).
  - The 3D view. With the 2D part the world frame is the window's shape and
    its camera follows. Otherwise (not mapped, or narrower than 4:3) an AFTER
    handler on `C44Matrix::Perspective` scales the projection's x term (or,
    narrower than 4:3, its y term) by the client area's aspect against 4:3
    (`core/widescreen.*`), only while `WorldView::SetupCamera` runs or
    `CSpriteFrame::SetupCamera` for the world frame (unit picking), so more of
    the map shows instead.

  The window size comes from `main_thread::client_size` (kept from
  `WM_SIZE`). The player can turn it off (Tome > Settings > Widescreen, on by
  default). The 2D part latches the setting at each `CGlueMgr::SetScreen`
  and follows from the next screen (the next menu screen, or the next game):
  text is laid out as it's made and keeps that layout, so changing the width
  under it scrambled the top bar. The 3D-only fallback follows every frame.
  At 4:3 nothing changes. Reserves nothing.
- **gl-context-restore** (`patches/gl_context_restore.cpp`,
  `docs/known-bugs.md`, "Under Wine"): with the OpenGL renderer in
  fullscreen under Wine, coming back from Alt+Tab leaves most textures
  white (Wine replaces the driver context behind the game's HGLRC). An AFTER
  handler on `CGxDeviceOpenGl::CanDraw` runs the game's own
  `DestroyContext` and `CreateContext` on the first frame CanDraw is true
  after being false while fullscreen (`CGxDevice.fullscreen`), so everything
  uploads again; a CreateContext that fails is tried again on the next frame.
  The decision is portable (`core/context_restore.*`, unit-tested). Skipped
  outside Wine (`needed`: `wine_get_version` in ntdll). Reserves nothing.
- **movie-frame-length** (`patches/movie_frame_length.cpp`,
  `docs/known-bugs.md`, "Under Wine"): `blizzard.ax`, the movies' decoder,
  leaves every frame the length of its buffer (4 bytes a pixel whatever
  format it plays in), and Wine's VMR7 copies that whole length into a
  smaller YUY2 surface. A BEFORE handler on `Movie::SetDisplayMode` (the
  first call in Movie::Play, on the main thread, before the graph exists)
  loads `blizzard.ax` from game.dll's folder the first time and never frees
  it, so DirectShow gets that copy; finds its mapping by PE timestamp and
  size (`mappings/blizzard.ax.json`, below), checks it with
  `verify_loaded_module`; and points that vtable slot (`patch_pointer`) at a
  wrapper that calls it, then sets the output sample's length to one frame
  of the format it's in (`core/movie_decoder.*`, unit-tested). The buffers
  keep their size, so the decoder has room after a format change too.
  Skipped outside Wine (`needed`). Reserves nothing.
- **movie-display-reset** (`patches/movie_display_reset.cpp`,
  `docs/known-bugs.md`, "Under Wine"): in a window the movie player resets
  the display mode before and after a movie and retries without limit
  while that fails, which under Wine on a display that can't change modes
  hangs the game. The innermost REPLACE handler on `Movie::SetDisplayMode`
  (`Handler.last`) does the windowed reset itself: nothing if the
  display is already in its registry mode, otherwise the same retries for
  about a second, then it goes on. In the fullscreen (cinematic) case it
  puts the desktop's registry mode back the same way (nothing to do when
  the game runs at the desktop's size) and writes the mode the display is
  then in to `Movie.cinematic_mode`, as the game's body does when none of
  its modes pass, so Movie::Play's window covers the screen. Proton emulates modes
  and leaves the monitor as it is: the game's 800x600 played in a 4:3 box
  with black around it, and any mode it scales leaves light lines along
  the video's window. If the mode can't be read it hands on to the game's
  body. It also points Movie::Play's `call [SetDeviceGammaRamp]`
  (`Movie.gamma_call`, the fullscreen movie's 1/2.2 gamma ramp, which
  Proton applies), then the restore when a key skips the movie
  (`Movie.gamma_restore`), at a no-op that reports success, with
  `patch_bytes` after checking that the slot holds gdi32's
  SetDeviceGammaRamp; if that fails, the report says so and the rest still
  applies. Skipped outside Wine (`needed`). Reserves nothing.
- **path-sanitizer** (`patches/path_sanitizer.cpp`, `docs/known-bugs.md`):
  refuses path traversal in every JASS native that takes a file path (38;
  `docs/known-bugs.md` lists them). The writes (`PreloadGenEnd`,
  `InitGameCache` and the save natives) are rooted in the game folder on
  1.27b, so a map-controlled name like `..\..\Start Menu\...\x.bat` could
  plant a file anywhere the process can write; the reads could run or load
  a planted file. It hooks the mapped `BindNative(fn, name,
  signature)` (which the engine calls for every built-in native on each map
  load), and when it binds one of those it remembers the engine's real
  `fn` and substitutes a sanitizing wrapper as the bound pointer — so no native
  address is hardcoded and any build that maps `BindNative` is covered. The
  wrapper (same `__cdecl` ABI as the native, string arg as an `RCString*`)
  reads each path through `RCString::GetText` (bound by the patch itself),
  and either calls the real native or, for a name that
  `path_guard::is_unsafe` rejects (absolute, drive or NTFS stream `:`,
  UNC/rooted, any `..` component or one of only dots and spaces with two
  dots or more, which Windows' trimming could make `..`, a reserved DOS
  device name, a name over `kMaxPathLen` (259, `MAX_PATH` less its NUL: the
  game would truncate a longer one, and one of ~4000 characters crashes
  `PreloadGenEnd`); for the writes also an empty name and one a program or
  script would have, `path_guard::is_executable_name`: `.exe`, `.dll`,
  `.bat`, `.vbs`, `.lnk`, Miles' `.mix`/`.asi`, ..., after the trailing dots
  and spaces Windows drops), skips the call and returns 0 (nothing / false /
  0 / a null handle). `Preload` is never refused (maps use its argument as data, which
  `PreloadGenEnd` writes out); an argument that isn't a safe path instead
  makes it refuse the natives that open the preload list's files
  (`PreloadStart`, `PreloadEnd`, `PreloadEndEx`, `PreloadRefresh`) until
  `PreloadGenClear` empties the list. One template wrapper per entry of a table of name, argument count
  and path arguments, so covering another native is one table line.
  `BlzLoadTOCFile` checks its TOC path and the FDF paths listed in it
  (`path_guard::toc_is_unsafe`).
  The decision logic is portable and unit-tested (`core/path_guard.*`,
  `tests/test_path_guard.cpp`). Unlike memhack-guard this BEFORE handler runs
  at the *lowest* priority (`INT32_MIN`), not the highest, so mods' BEFORE
  handlers of higher priority can't undo the substitution (a mod's own
  `INT32_MIN` or REPLACE handler still runs later). It reserves nothing — the threat is
  malicious maps, not mods, and a mod is native code that bypasses any API
  boundary anyway, so reserving `BindNative` would only cost mods a useful hook.
- **execute-func-guard** (`patches/execute_func_guard.cpp`,
  `docs/known-bugs.md`): `ExecuteFunc` runs a function with no arguments, so
  naming one that takes parameters (the map's own or Blizzard.j's) crashes
  the game in the JASS VM. The same `BindNative` substitution as the path
  sanitizer binds a wrapper for `ExecuteFunc`. At each map's loading screen
  it takes the scripts the map script guard reads there (`war3map.j` or
  `scripts\war3map.j`, `Scripts\common.j`, `Scripts\Blizzard.j` through Storm;
  `events::set_map_script_hook`), so a big map's scripts are parsed under the
  loading screen rather than mid-game, and collects the functions that take
  parameters (`jass::functions_taking_parameters`, unit-tested and fuzzed).
  Without the events (not mapped) it reads them itself on its first call in
  each map (`natives::map_load_count`). A call naming one is refused (nothing runs, as for
  a missing name, which the game already ignores) and logged, the first 20
  and then every 1000th. If the map's script can't be read, calls go through
  unchecked. Reserves nothing.
- **global-string-guard** (`patches/global_string_guard.cpp`,
  `docs/known-bugs.md`): a native returning a string (`I2S`,
  `GetLocalizedString`, ...) in a global's initial value crashes the map
  load: `CreateJassString` adds the string to the VM's table, and while the
  globals block runs `JassVM::Get(CGameWar3.vm_index)` is still null.
  `CreateJassString`'s call to `CGameWar3::CreateString`
  (`CreateJassString.call.create_string`, `patch_call`: no hook on a path
  every string native takes) goes to a check that makes the same lookup
  (`JassVM::Get`, `JassVM.strings`) and, with no game, VM or string table,
  returns 0, the null string; otherwise it calls `CreateString`. The call's
  byte is `??` in `CreateJassString`'s mapped bytes, so mods can still hook
  it (their handlers run before the check). Reserves nothing.
- **lan-map-guard** (`patches/lan_map_guard.cpp`, `docs/known-bugs.md`,
  "Map downloads from a host"): the map path a game's host sends goes
  through the map transfer's own check, `MapTransfer::IsValidName`, before
  the client looks it up under the game folder and, failing that, downloads
  it. The game refuses absolute paths, `:` and wildcards, extensions other
  than `.w3m`/`.w3x` and device names compared up to the last dot, but not
  `..` components or `com1.x.w3x`. An AFTER handler (last priority) also
  refuses what `path_guard::is_unsafe` refuses, so no transfer is made and
  the client can't join with that map. Logged like the path sanitizer's
  refusals (the first 20, then every 1000th). Reserves the function.
  `tests/game` calls the real check through the guard.
- **jass-token-guard** (`patches/jass_token_guard.cpp`, `docs/known-bugs.md`,
  "Long string literal"): the JASS scanner keeps a token in two heap buffers
  of `JassScanner.capacity` + 1 bytes (2100 + 1), and its length check never
  fires (its exit also wants a flag `nextToken` clears when it starts), so a
  string, comment or name over 2100 characters in `war3map.j` writes past
  them. The check's `jne` (`JassScanner::nextToken.overflow_check`) becomes
  a `jmp` to the same exit, the scanner's own error return (-2), which stops
  the parse; the target is read from the instruction and its bytes checked
  first. That error path crashed by itself: the parser's constructor leaves
  `JassParser.root` unset and the caller destroys it after a failed parse, so
  a BEFORE handler on `JassParser::parse` zeroes it. The map's script then
  doesn't compile and the game goes back to the menu, as for any script
  that doesn't compile. An AFTER handler on `nextToken` logs the -2. Reserves
  both functions (memhack-guard also reserves `nextToken`). `tests/game` runs
  the real scanner on a 3000-character string with its buffers swapped for
  larger ones: past capacity + 1 without the patch, nothing with it.
- **command-card-guard** (`patches/command_card_guard.cpp`,
  `core/command_card.*`, `docs/known-bugs.md`, "Command card button
  positions"): 1.28's fix. `CCommandCard::PlaceButton` indexes the 4x3 grid
  with a button's `CCommandButtonData.x`/`.y` unchecked; a BEFORE handler
  skips it (the button isn't shown) for a position outside the grid. On
  1.26a, whose setters store any value, that is every bad position (a crash
  before; logged, the first 20 and then every 1000th); on 1.27b, whose
  setters drop a coordinate outside the grid, it's the two positions maps
  hide buttons with, (0, -11) and (INT_MIN, INT_MIN), which the patch makes
  the setters keep: their store sequences
  (`CCommandCard::AddButton.pos_check`,
  `CGameUI::SetCommandButtonPos.pos_check_button` and `.pos_check_data`) are
  rewritten into a `call [ptr]` (`ff 15`, reaching `tome.mix` at any distance)
  to a C function that stores what `command_card::stored` says, with NOPs
  after it. All three sites are compared with the expected bytes before any
  is written; on a mismatch none is (the report says so) and PlaceButton's
  check still applies. Reserves `PlaceButton`. Checked in game on both builds
  with `CustomKeys.txt` (Option.customkeys) moving Attack to (0, -11) and Move
  to (INT_MIN, INT_MIN): both hidden.

## Multiplayer handshake

`loader/src/handshake.cpp` and `loader/src/core/handshake.cpp` (unit-tested).
Every player of a game needs the same `"sync": "required"` mods, or the game
desyncs (`research.md` §4). **Verified** in LAN games between two instances
on one machine (2026-10-26): matching sets, a refused Start, the in-game
report on both sides and in a replay; in `make e2e` since 2026-11-02
(`tools/e2e-multiplayer.sh`, "Testing with two instances" below).

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
    ...`), and again when its own set changes. It knows the host: the
    player whose number is 1 (`NetGetPlayerNumber` on every id: the game's
    router gives a game's creator number 1; `reverse-engineering.md`, "The
    network client"), and counts only the host's set: another player who
    sends one is named in a note and ignored. Senders come from the host's
    router, so a client can't pass for the host (the LAN test sends a line
    claiming the host's id, `TOME_DEBUG_LOBBY_AS`, and checks the host sees
    the real one). Such a player still gets a reply, so a host Tome got
    wrong somewhere could still check its players and start. A host that
    isn't number 1 logs a warning. On a build without the mapping, the
    client notes when more than one player sent a set instead. A client with
    required mods (or a sync patch off) that gets no set within 5 s is told
    the host (by name) may not run Tome, and, with the host known, sends it
    its set unasked: a host without required mods and with every patch in
    force sends nothing, yet must see a client that differs. It then
    answers with its own set ("none"), and counts the difference at Start;
  - `CGameChatroom::OnChatReceived` (before) hides lines that decode as
    Tome's and passes them on; anything else, "[Tome] ..." typed by a person
    included, shows as usual. Notes go into the lobby's chat area
    (`CTextArea::AddText`) with a "Tome:" prefix;
  - `CGameChatroom::StartGame` (before): the host refuses to start while a
    player's set differs or hasn't arrived (with required mods of its own),
    listing each player and what differs (missing, extra, different version
    or hash). If the lobby couldn't be read at all, a host with required
    mods refuses too. The host can override either: a second press of
    Start within 10 s, with the same problems, starts anyway, with a
    warning in the log and the lobby (`handshake::StartOverride`). When
    only built-in patches differ (`is_patch`), the lines say so: they only
    desync maps that use what the patches close.
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
- **The game's host** (`handshake::host_slot`, `host_player` for mods): in
  a game, the player slot of player number 1, found on first use (net client
  0, or 1 in a replay) and logged (`Handshake: the host is player 0 (...)`);
  -1 outside a game or without `NetGetPlayerNumber`.
- **Limits on what other players send:** a set of more than `kMaxMods`
  (256) mods is dropped, whether it comes as lobby lines (`Assembler`) or as
  game actions (`GameRecord::add`), so a modified client can't make the
  comparisons slow. Each player's handshake messages also get a log budget
  (`LogBudget`: 100 lines per lobby, `kMaxMods` + 8 per game); past it one
  line says the rest aren't logged. Unreadable keys are logged cut to 100
  characters.
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
  waiting, both applied on leaving through End Game, Quit Mission), and in
  a two-instance LAN game (`tools/e2e-multiplayer.sh`: the host's toggle
  waits for the game to end).
- **Not done:** saved games; the host's net id (see `reverse-engineering.md`).

### Testing with two instances

`tools/e2e-multiplayer.sh` (in `make e2e`). Two copies of war3.exe in one
Wine prefix shut each other down (its single-instance check), and LAN needs
UDP/TCP 6112 for each (`netgameport` doesn't move the LAN port). So the
client runs in a Wine prefix of its own (`build/test-env/wine-client`), and
each instance in its own network namespace, joined by a veth pair inside one
unprivileged user namespace (`unshare -rn`, then a nested `unshare -n`;
10.77.0.1 and .2, a default route on each side so broadcasts cross).
`nsenter -U` can't join that user namespace (setgroups is denied), so the
script runs itself again inside it and starts the games from there; the
client's with `nsenter -n`. What it took:

- **Every Wine command of the client in the client's namespace**, `winepath`
  included. Wine creates sockets in its wineserver, so a wineserver started
  in the host's namespace puts the client's sockets there too: both want
  6112 and the client's LAN screen says "Could not connect to the network".
- **Xvfb outside the namespaces.** Inside a network namespace the desktop's
  abstract X socket isn't visible, so `-displayfd` picks `:0`, replaces the
  desktop's `/tmp/.X11-unix/X0` and deletes it on exit (X apps that use the
  file, such as Flatpak's, can't connect until the next login). Started
  outside, it picks a free display, and the games reach it through the
  socket file, which every namespace sees.
- **Separate game folders,** so each instance writes its own
  `replay\LastReplay.w3g`. Both prefixes have the profile Tome; the client
  types "Guest" over it on the LAN screen (`focus:PlayerNameEditBox`,
  `text:`). The game list's rows have no names: the client clicks the
  first at its position.


## Messages between players

API version 18 (`net_send`, `on_net_message`; `loader/src/net_messages.cpp`,
the wire format in `core/netmsg.*`, unit-tested and fuzzed). A mod's message
reaches the same mod on every player's computer, the sender's included.

- **The channel:** the game's SyncStoredInteger actions, as the handshake's
  sets in games (`handshake::send_sync_action`). They reach every player at
  the same point of the game, in order, and the replay records them, so mods
  can act on a message alike everywhere (a required mod may change the game
  in response without a desync). Each action is one part: game cache
  `TomeNet`, mission `1 <message id> <mod id>`, key up to 189 bytes of the
  message in base64 (the text fields hold no NUL; 252 characters fit the
  key's 0x100 bytes), value `part | count << 8`. A message is up to 1024
  bytes (`TOME_NET_MAX_MESSAGE`), so at most 6 actions.
- **Receiving:** the handshake's `OnNetSyncStoredInteger` hook hands
  `TomeNet` actions to `net_messages::receive` and skips the game's own
  handling (it would store each part in a game cache nothing reads). The
  sender is the action's player slot, which the host's game stamps
  (`reverse-engineering.md`, "Senders can't be faked"). The reassembler
  keeps one message in progress per sender (a player's parts come in a
  row), drops anything malformed or out of order (logged, 10 per player
  per game), and never holds more than 1024 bytes per sender. A whole
  message goes to every `on_net_message` listener of the mod with that id,
  each under `guard::run` (a crash disables that mod); in a multiplayer
  game only if that mod is required (no other mod can send there, so such
  a message is forged: dropped, logged once). Listeners go with
  the mod's other registrations when it's disabled.
- **Sending:** only in a game from its first step on (the first
  `GAME_TICK`; `handshake::game_stepped`), not in a replay (the game sends
  nothing there), and in a multiplayer game only from a `"sync": "required"` mod:
  the handshake makes sure every player has those, so every player gets the
  messages, and no player without Tome fills a game cache with them. At
  most 16 messages a second per mod (`TOME_ERR_UNAVAILABLE` past that,
  logged once). Message ids count up per mod. Not at `GAME_START`: the
  players may not all be in place yet, so the game lock (and with it the
  required-mods rule) is only final once the first step checks it again,
  and the handshake sends its own sets then too.
- **Tests:** `tests/test_netmsg.cpp`, `fuzz/fuzz_netmsg.cpp` (reassembled
  messages are valid and re-encode to themselves), host tests for the
  arguments, and the two-instance LAN game (`tools/e2e-multiplayer.sh`):
  sync-test's `/synctest` sends a line and 1000 bytes from the client and a
  line from the host, and both sides check each arrives whole from the right
  player. Not checked: playback in a replay (the test's replay runs without
  sync-test). For the lobby, before a game, see "Lobby messages".

## Lobby messages

API version 21 (`lobby_send`, `on_lobby_message`, `lobby_players`,
`lobby_show`, `on_lobby_start`, `lobby_start`; `loader/src/lobby_messages.cpp`, the wire
format in `core/lobbymsg.*`, unit-tested and fuzzed). For required mods that
must agree on something with every player before the game starts (the
Geomancer mod: the seed of the map every player generates).

- **The channel:** lobby chat lines, as the handshake's
  (`handshake::lobby_send_line`: `NetSendLobbyEvent` to one player), each
  `[Tome] Msg 1 <message id> <mod id> <part>/<count>: <base64>`, up to 96
  bytes of the message per line (the longest header and 128 base64
  characters stay under the 250 a line may have), so a 1 KiB message is up
  to 11 lines. `before_chat_received` hands every line the handshake doesn't
  decode to `lobby_messages::receive`, which hides all `[Tome] Msg ` lines,
  well formed or not (a person can't type one into the chat), reassembles
  them per sender (`lobbymsg::Reassembler`, as `netmsg`'s) and gives whole
  ones to the mod they name, if it's required. The sender is the event's
  sender byte, stamped by the host's router; `from_host` compares it with
  the host the handshake knows (`Lobby::host_id`).
- **Who gets them:** only players whose Tome hides them: on the host, those
  whose handshake reply arrived (`Lobby::runs_tome`); on a client, the host
  once its set arrived. Players without Tome would see the lines, so they
  get none; a joined player talks to the host only (it doesn't know who
  else runs Tome). Not in replays (lobby lines aren't recorded); nothing
  is sent in games (`net_send` is for that).
- **The start:** `before_start_game` runs the handshake's checks first;
  once they let the game start (or the host overrides them), every
  required mod's `on_lobby_start` callback runs (`start_allowed`), and any
  refusal skips Start, its reason in the chat. `StartOverride` doesn't
  apply to mods: a mod refuses because something isn't ready (a player
  without the map), which a second press doesn't change. `lobby_start`
  calls `CGameChatroom::StartGame` through its hooks, so the same checks
  run; it's refused inside an `on_lobby_start` callback (it would recurse).
- **Limits:** 16 messages a second per mod (each to each recipient), the
  log budget per sender for malformed lines (`LogBudget(10)`), one message
  in progress per sender, dropped when the lobby closes.
- **Trust:** `from_host` and who a client may send to come from the host
  the game names (NetGetPlayerNumber); without that mapping a client knows
  no host for mods (`Lobby::host_id` -1), so it sends nothing. A mod's
  `lobby_start` never overrides or arms the handshake's override (the
  host's second press), and an `on_lobby_start` callback that fails refuses.
  `lobby_players` reads the list the half-second poll keeps; a player who
  leaves drops their message in progress.
- **Tests:** `tests/test_lobbymsg.cpp`, `fuzz/fuzz_lobbymsg.cpp`,
  `handshake_lobby_tome_peers` (who runs Tome), host tests for the
  arguments outside a lobby, and the two-instance LAN game
  (`tools/e2e-multiplayer.sh`: sync-test's hello and welcome, its note,
  its hold on the start and its own press).
## Timers

API version 19 (`timer_start`, `timer_stop`; `loader/src/timers.cpp`, the
schedule in `core/timers.*`, unit-tested). Two clocks, each the sum of what
the game reports: real time from the frames' deltas (`PresentFrame`), game
time from the steps of a game (`OnNetGameTick`, the step's float as
`GAME_TICK` sees it). Both count whole microseconds (`core/game_clock.*`,
unit-tested): the step's float and the mods' delays become integers by
reading their bits, with no floating point. In 32-bit code a float sum goes
through the x87 FPU, whose rounding follows the thread's precision setting
(Direct3D sets single precision, `-opengl` leaves double), so two players'
sums could drift apart and a due time land in different steps; integers
can't. `events.cpp` keeps the game's count (`GAME_TICK`'s `game_time` is it
in seconds) and hands it to the timers and the desync rounds. Real-time timers fire from the frame after the
`TICK` handlers; game-time ones in the step after the `GAME_TICK`
handlers. Since game time only moves in steps every player's computer takes
alike, a game timer started from a synced point fires in the same step
everywhere. A timer fires at most once per frame or step; one that fell
behind goes on from now, so nothing bursts. A game's timers end with it
(`GAME_END`), real ones go on. Registration is any thread for real time
(a mod's first enable is on the bootstrap thread), the main thread for game
time. A mod still in its `tome_mod_load` or `tome_mod_enable` on another
thread (the guard would skip the call) keeps its due timers due until it's
done, so one that fires once isn't lost. One revoker per mod however many
timers it starts (a mod starting a timer every frame mustn't grow its
revoker list). The fake game's
`tome_test_event(TICK)`/`(GAME_TICK)` move the clocks by `delta`
(`events::dispatch_for_test`), so mods' own tests drive timers too.
Wasm modules keep a record per timer and free it when the callback block
says the timer is done (`wasm-mods.md`); `timers_api::set_ended_hook`
tells `wasm_mods.cpp` of game timers that ended without firing.

## Desync checks

API version 19 (`on_checksum`; `loader/src/desync.cpp`, the wire format and
the comparison in `core/desync.*`, unit-tested and fuzzed). A lockstep game
desyncs when one computer computes something the others don't; the game
then drops players without saying why. A required mod's checksum callback
returns a hash of its game state; Tome compares it between the players.

- **Rounds:** every 3 s of game time (`desync::kInterval`, in the integer
  microseconds of `core/game_clock.h`, as the timers count), in the game step
  that passes a multiple of it (`round_ending`), after the step's
  `GAME_TICK` handlers and game timers: the same step on every computer.
  Only in a multiplayer game or its replay (the game lock's rule), from the
  first step on, and only for running `"sync": "required"` mods (the
  handshake makes sure every player has them), called by mod id, each under
  `guard::run`.
- **The channel:** each hash is one SyncStoredInteger action, like the
  messages between players: game cache `TomeSum`, mission
  `1 <round> <mod id>`, key the hash in 16 lowercase hex digits, value 0.
  The handshake's `OnNetSyncStoredInteger` hook hands them to
  `desync_api::receive` and skips the game's own handling.
- **Comparing:** `desync::Checker` keeps this computer's hashes for the
  last 8 rounds; another player's is compared at once, or waits for this
  computer's round (at most 8 rounds ahead, 2048 in all: the input is
  other players'). Rounds this computer passed without a hash for that mod
  (it doesn't run it) compare nothing. Live, its own actions coming back
  are skipped; in a replay every recorded player's are compared with the
  replay's.
- **Telling:** the first match per mod and player is logged ("matches",
  so the log shows the checks run); the first difference is logged with
  both hashes and shown to the player (the mod, the other player's name and
  the game time); later differences only at debug level. Malformed actions
  are logged 10 per player per game.
- **Tests:** `tests/test_desync.cpp`, `fuzz/fuzz_desync.cpp` (what parses
  re-encodes to itself; what waits stays bounded), host tests for the
  registration, and the two-instance LAN game: sync-test hashes the
  messages it got, the same on both computers until the client's
  `/synctest skew`; both sides check the match first, then the difference.

## Profiling mods

`guard::call` (every call into a mod: lifecycle, natives, hooks, events,
commands, UI, timers, checksums) reads `QueryPerformanceCounter` around the
call and gives the mod its own time: the call's, less the guarded calls
inside it (a thread-local counter), which count for whoever they belong to
(a mod's publish runs the subscribers' callbacks; a hook handler calls
the next handler). The game's own function, when a mod's REPLACE handler
hands on to it (`call_next`, also through Tome's own handlers after it),
runs under `guard::uncharged`, which adds its time to that counter too
(`Dispatch::original_handed_on`, `core/hooks.cpp`): the game's work isn't
the mod's. A game function a mod calls itself, hooked or not, still is.
Lifecycle calls (`tome_mod_load`, enable, disable, unload; `guard::kLifecycle`)
count in the calls and total time and in "starting and stopping" on the
Details page, not in the frames or the slowest call: a slow load isn't a
slow frame, and would hide the per-frame calls. `tome_mod::profile` (`core/profile.*`, unit-tested) adds
the times up by frame (`events::frames()`, via
`guard::set_frame_counter`) in a ring of the last 128 frames, and keeps
the calls, total time and slowest call (what it was: "event handler
(tick)") since the DLL was loaded. A mod averaging over 4 ms a frame over a
full window is named in `tome.log` once. `ModStatus::profile` carries the
numbers to the Details page ("Time: ...", yellow when slow) and to
`/tome perf` (the busiest 8 running mods). The cost is two counter reads
and a lock per call.

## Hooks

`loader/src/hooks.cpp` (patching, Windows) and `loader/src/core/hooks.cpp`,
`core/x86.cpp` (layouts, chain order, instruction decoding; unit-tested).
API version 4 (`hook`, `unhook`, `hook_call_next` in `tome.h`).

- **Targets are mapped names.** `hook(mod, "CTextFrame::SetText", phase,
  priority, fn, user)` looks the name up in the mapping database, then in
  mods' own files (`"<mod id>/<name>"`, "Mods' own mappings" above); it must be
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
- **One function, several names** (Tome's and a mod's, or two mods'): the
  target is kept by address as well as by name. A second name for a hooked
  address shares its patch and chain (`add_alias`) if `hooks::same_layout`
  says both pass the arguments the same way (registers, stack slots,
  cleanup, kind of result; a float argument may be an int), and its `bytes`
  match the code, the first 5 compared with what the jmp replaced
  (`Target::original`). Otherwise it fails and logs both mappings. Each
  handler keeps the name it hooked by (`Handler::name`), which is what its
  `tome_hook_call.name` says, and `unhook` by one name leaves the handlers
  hooked by another. A patch whose moved bytes would overlap another
  target's, or that starts inside them, is refused (`overlapping`): it
  would copy the other's jmp, or be bypassed through its trampoline.
- **Imports** (API version 9, `hooks::install_import`): the same stub, but
  nothing is moved: the "trampoline" is what the import slot held (the
  imported function, or another tool's replacement), and the slot gets the
  stub's address in one interlocked store. Every call game.dll makes
  through the import, by its thunk or by `call [slot]`, then runs the
  chain; other DLLs' imports are untouched. The slot is checked against the
  import directory at startup (above).
- **Calls:** `tome_hook_entry` (asm) pushes eax/edx/ecx and calls
  `tome_hook_dispatch` with a pointer to them, the target, the return
  address and the stack arguments, plus 24 bytes for its answer. The
  dispatcher copies the arguments (declaration order) into an array, runs
  the chain, and writes eax, edx, the bytes to pop and, for floating-point
  results, the value and its kind; the stub loads st(0) from it (`fld
  dword`/`fld qword`), sets eax/edx and returns, popping the stack arguments
  for callee-cleaned conventions (`ret N`, N from the layout). The original
  runs through `tome_hook_invoke` (asm), which pushes the possibly changed
  arguments, sets eax/ecx/edx, calls the trampoline, and stores eax, edx and
  (popped, so the x87 stack stays balanced) st(0); esp is restored from
  ebp, so the callee's convention doesn't matter. Registers no parameter
  uses pass through unchanged. The caller gets its ecx and edx back as it
  passed them (edx stays the result's high half for a 64-bit result):
  1.26a's compiler keeps a value in ecx across a call to a leaf function
  that doesn't touch it (callers of RCString::GetText), and a hook that
  handed back the handlers' ecx crashed the game
  (`hook_runtime_keeps_the_callers_ecx_and_edx`). The caller's eax goes into
  the original, so a void function that leaves it alone still does when
  hooked, and a constructor declared void still returns `this`; a void
  function whose original the handlers skip gives back the caller's eax
  (`hook_runtime_passes_the_callers_eax_through`).
- **Chain order** (`core/hooks.cpp`, `run`): BEFORE handlers, then the
  REPLACE handlers nested (each calls `hook_call_next` for the rest; the
  last one's next is the original), then AFTER handlers. Within a phase,
  higher priority first, ties in registration order. Tome's own innermost
  REPLACE handlers (built-in patches that replace only the game's body) set
  `Handler.last`, which the mod API can't: they run after every other
  handler of their phase, even a mod's at `INT32_MIN`. A BEFORE handler can
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
  `idle` false until it returns; two names share a patch, and overlapping
  patches are refused. Host tests hook a mod's own function on the fake
  game and an alias of `RCString::GetText`, and check that an alias of a
  reserved function is refused. The e2e fixtures hook-a
  and hook-b both hook `RCString::GetText`; hook-b calls it on a fake string
  each time it's enabled, and the test checks the order of both mods'
  handlers, hook-a's changed argument reaching the original through
  hook-b's REPLACE, and the chain after disabling and re-enabling hook-a.

### Under Shade

Shade (`../shade`) recompiles the game to C and runs `tome.mix` recompiled
too, so writing a `jmp` or a call's target into the game's code changes
nothing there. `loader/src/shade.cpp` finds Shade's natives
(`GetModuleHandle("shade.dll")`, which exists only under Shade) and the hook
manager and patch functions use them instead of patching code:

- `hooks::install`: `ShadeOriginal(address)` gives the trampoline (no
  instructions are moved; the same address before, during and after the
  hook) and `ShadeMakeStub` the stub, both stored in the `Target` before
  `ShadeHookPushJump(address, tome_hook_entry, Target*)` makes the function
  run that stub. Shade's hook is on before that native returns, so a call
  on another thread can reach the dispatcher at once: it finds the `Target`
  complete. `ShadeOriginal` is in Shade's natives from version 2; a Shade
  without it can't hook functions (each hook fails, saying so). The `jmp`
  is still written, so memory reads as on Windows (and may not cover
  another hook's start). If writing it fails after Shade hooked the
  function, `ShadeUnhook` takes the hook off (a warning if it can't) and
  the `Target` is kept (unregistered) for calls already on their way.
- `hooks::install_import`: the slot gets a stub from `ShadeMakeStub` (code
  made at run time can't run under Shade); the slot write itself works.
- `patch_call`: `ShadeHookCall(site, target)` before the write (Shade reads
  the call's target from memory), once the page is writable: Shade can't
  take a redirect back, so none stays behind a failed patch.
- JASS natives: instead of a thunk written at run time, each slot gets
  `ShadeMakeStub(tome_native_entry, Slot*)`; `tome_native_entry` (asm in
  `natives.cpp`) asks `tome_native_route` and either jumps to Tome's own
  native with the engine's stack frame or returns a mod's guarded result.
- `patch_bytes` fails ("code patches don't apply under Shade"), so the
  patches built on it (blp-guard, movie-display-reset, jass-token-guard,
  command-card-guard's setter rewrites) report that they're off until
  they're ported to hooks (jass-token-guard fails as a whole and takes its
  handlers off again; command-card-guard's PlaceButton check, a hook, still
  applies). `patch_pointer` works on data (vtables)
  but refuses an address in a module's executable section: a pointer there
  is an instruction's operand, compiled in, so widescreen's UI part (which
  repoints operands of the coordinate conversions) reports that it's off
  ("the UI stays stretched"); its 3D part, hooks only, works.
- Mods' DLLs load from version 4 of Shade's natives (`shade::can_load_dlls`):
  Shade runs only recompiled code, so a DLL's code is built apart from the
  game's (`shade-recomp build-mod`, Shade's design.md "Mods' DLLs") and
  found by the DLL's SHA-256 when `LoadLibrary` gets the shadow copy. A DLL
  without it fails to load like a missing file (126); Tome's error then says
  it must be built, and Shade's log says how. One whose built code Shade
  can't use fails with 1114, and the error says why it may be
  (`shade::load_error_hint`).
- What runs is that library, not the DLL, and it sits outside the mod
  folder (`$SHADE_MOD_LIBS`, `<data>/mods`), so approval covers it too:
  `ShadeModCodeHash` gives its SHA-256 (`shade::mod_code_hash`), and the
  mod's code hash becomes the SHA-256 of
  `mod_state::built_code_manifest(<files' hash>, <library's hash>)`. Another
  library for the same DLL is asked about again ("changed"); one that isn't
  built yet leaves the files' hash, so building it asks again too. A
  signature covers the mod's files only, so under Shade it allows no
  update (the mod counts as unsigned). As on Windows, a mod allowed in its
  own folder (not a package) runs whatever its files are; that includes the
  library. Shade hashes the library when asked and opens it at the load
  after, so a library swapped in between runs unasked: whoever can write
  there can also write the mods folder. Version 3 loaded DLLs without
  `ShadeModCodeHash`: Tome treats it as a Shade that doesn't. A Shade before
  version 4 runs no DLLs: such a mod (without a Wasm module) gets an error
  saying so and never asks for approval. Wasm mods run in either (WAMR's
  interpreter).

On Windows and Wine nothing changes: `shade::active()` is false.

Tests: `tests/shade/fake_shade.cpp` is a fake `shade.dll` (Shade's natives
on Windows; each can fail on request, and a hook can catch a call on another
thread while it goes on). `tome_hook_runtime_shade_tests` loads it and runs
the whole hook runtime suite plus `tests/test_shade.cpp`; the host test
`host_shade_natives` calls natives through the Shade stubs,
`host_shade_refuses_mod_dlls` checks a DLL mod is refused by a version 3
Shade, `host_shade_loads_mod_dlls` that one runs under version 4 (the
fake's `FakeShadeSetVersion`), and `host_shade_*built_code*` and
`host_shade_signature_does_not_allow_built_code` that the built code's hash
(`FakeShadeSetModCode`) is part of the approval.

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
  Settings), Settings, Tome (versions, mods folder, safe mode, Settings,
  Mods Folder), Tome's Settings (Tome's own settings, `core/tome_settings.*`,
  shown like a mod's and saved in `tome/settings.json` by
  `tome_settings.cpp`; Keybinds in its footer) and Keybinds. `build()` turns a page into a generic view
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
- `TomeModsPanel` covers only the left part of the screen (0.45 wide): any
  frame on top of the main menu's buttons, even an empty one, takes their
  clicks. So Single Player, Options, ... work with the panel open, like the
  game's own screens. A command from one of them closes Tome's panel
  first (`close_glue_panel`: saves its text boxes, ends a keybind's capture,
  cancels an opening's pending steps), as the Options screen raises its
  own. The CMainMenu OnEvent hook only counts a button's command, the event
  object `ControlClickEvent` (CControl::Click's): the menu's UI timers
  report there too, with ids in the same range, in other event objects.
  Leaving the main menu any other way (a mod or the game changing screens)
  goes through a BEFORE hook on `CGlueMgr::SetScreen`, which saves the text
  boxes before the screen and its frames are destroyed.
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

- **Tome's own** (`core/tome_settings.cpp`, Tome > Settings): Widescreen,
  Check for Updates, Install Updates Automatically, a switch per built-in
  patch (Patches, above), and Stop on Memory Corruption (`harden_heap`, off by default): read once at startup, it calls
  `HeapSetInformation(HeapEnableTerminationOnCorruption)` (`bootstrap.cpp`,
  `harden_heap`), which `war3.exe` (linked for Windows XP) doesn't get by
  default. Windows can't undo it in a running process, so a change applies
  from the next start. The process then ends through Windows' critical
  failure path (`0xC0000374`), which may skip exception filters, so there may
  be no Tome crash report, only Windows' own (event log, Error Reporting).
  Not checked on Windows yet; Wine accepts the call but its heap ignores it
  (the e2e test runs every launch with it on).
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
- Values live in `tome/mods/config/<id>.json` (outside the mod's folder, so
  replacing the mod keeps them): every setting, plus keys the
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
  folder (not `tome/mods/config/`), so editing an asset triggers a refresh, which
  rescans the folder (`overlay::scan_assets`: names, sizes, write times).
- **Order inside a refresh:** assets are applied before mods are enabled,
  so `tome_mod_enable` can already load a TOC from its own assets. If a DLL
  then fails to load, its assets are removed again.
- **Scripts:** `Scripts\Blizzard.j` (and so `common.j`) can be overridden
  the same way; the game reads them on each map load.
- **Testing:** `TOME_DEBUG_ASSET=<archive path>` logs the size and FNV-1a
  hash of that file as the game sees it after each change. The e2e test
  overrides the main menu button texture with a generated BLP
  (`sdk/support/make_blp.py`) and `Blizzard.j` with a copy that calls back into
  the map (`tests/e2e/patch_blizzard_j.py`), loads a TOC and FDF shipped in a
  mod's `assets/` from the map script, and plays a game started from the
  main menu to check the `map` and `restart` modes.

## Update check

Tome's only connection of its own, and only once the player allows it.

- **Asking:** Tome's setting `update_check` (`core/tome_settings.*`, off by
  default). While `tome/settings.json` doesn't have it, the first main menu
  of a start shows a popup (4 s after Setup, like other UI from code)
  asking; either answer is saved there, a popup taken away asks again next
  start. The Tome > Settings page changes it later; turning it on checks
  at once if a check is due.
- **Checking** (`update_check.cpp`): at most once a day
  (`core/update_check.h`, `due`), a GET of GitHub's
  `repos/reignoforder/tome/releases/latest` over WinHTTP (https only, TLS
  1.2 asked for explicitly for Windows 7, 15 s timeouts, a 1 MiB cap,
  user agent `Tome/<version>`) on a thread of its own; the result goes to
  the main thread. HTTP 404 means no release yet. A failed check is logged
  and shown on the Tome page, and doesn't count as a check (the next start
  tries again).
- **The reply is untrusted** (`parse_release`, `fuzz/fuzz_update.cpp`):
  only `tag_name` (an optional `v` and a semver, `[A-Za-z0-9.+-]` only),
  `draft` and `prerelease` are read; the release page's URL is made from the
  tag (`release_page`), never taken from the reply.
- **Only signed releases are announced:** for a newer version, Tome fetches
  the release's `SHA256SUMS` and `SHA256SUMS.sig` (`asset_url`) and checks
  the signature (over the version and `SHA256SUMS`) against the keys built
  in from `loader/release-keys.txt`, and that `tome-<version>.zip` is listed
  (`verify_release`). Anything else is a failed check: logged, on the Tome
  page, no popup. The zip's hash goes into `update.json` and the popup.
  `docs/releasing.md` has the signing side.
- **Telling:** a newer version than this one gets a popup (Update Now;
  Release Page, which `ShellExecute`s the page; Not Now), once per version
  (`notified`), and the Tome page's "Updates:" line shows it from then on.
- **Installing automatically:** with `update_install` on (Tome > Settings,
  off by default; only with `update_check`), `notify` starts the install
  instead of showing the popup, once per version (a failed one shows its
  popup and isn't retried by itself), and the "Tome Updated" popup says it
  was automatic. It never restarts the game.
- **Installing (Update Now, one click):** a thread
  downloads `tome-<version>.zip` (64 MiB cap) and goes on only if its
  SHA-256 is the one the signed `SHA256SUMS` listed. `plan_install` takes
  only `tome.mix` and Tome's texts (`tome/README.txt`, `DISCLAIMER.txt`,
  `licenses/`), never anything else a zip carries (mods, settings, other
  files); every entry is unpacked in memory first (Tome's zip reader:
  unsafe names refused). Windows doesn't let a loaded DLL be overwritten,
  but renamed: the new one is written as `tome.mix.new` (read back and
  hashed), the loaded one renamed to `tome.mix.old`, the new one to
  `tome.mix`; the game only loads `*.mix`, so neither leftover runs. Any
  failure before the last rename leaves the old `tome.mix`. The thread has
  Windows' file virtualization off (a copy of the process token), so a game
  folder under Program Files without admin rights fails cleanly instead of
  writing into the VirtualStore. Right after the swap, `update.json` gets the
  new `tome.mix`'s SHA-256 (`installed_mix`), before the install finishes,
  in case the game closes first. Then "Restart Now / Later" (`restart.h`);
  a failure offers the release page. Restart Now's helper runs from the
  file this session's code came from (`update_check::loaded_file`: now
  `tome.mix.old`), not the new `tome.mix`, whose `TomeRestartW` may take
  other arguments or not load at all.
- **Rollback:** at startup (the bootstrap thread, as soon as the log is
  open: before the mods' first load), `tome.mix.old` kept by an update
  (`old_from_update`: the `tome.mix` in place hashes to `installed_mix`;
  any other `tome.mix.old`, left by hand or under a developer's build
  deployed since, is neither rolled back to nor deleted, and the stale
  `installed_mix` is cleared) with this version's start
  failing twice in a row (`crash_loop::failed_starts`, as read before this
  start counted) moves this `tome.mix` to `tome.mix.failed` and the old one
  back, and records the version in `update.json` (`rolled_back`: never
  offered again). This start still runs the new version (it's loaded); the
  old one runs from the next. Once a start has reached the main menu and settled,
  `tome.mix.old` is removed (not if an update was installed meanwhile: then
  it's this version, the new one's way back). A clean shutdown after the main menu counts as
  a good start (`crash_loop::on_shutdown`, except in a crash loop's safe
  mode), so restarting from the popup
  within 5 s isn't a failure. It can't help a `tome.mix` that never gets to
  run its startup.
- **Once per version:** a version counts as shown (`notified`) when the
  player answers Release Page or Not Now, or when its install finishes
  (either way); an install the game closed in the middle of is offered (or
  installed automatically) again at the next start. A check that finishes
  while an install runs or after one is dropped: the next start checks again.
  With `update_check` off, nothing found before is shown or installed.
- `tome/update.json` keeps the last successful check's time, the latest
  version and tag, its zip's signed hash, the version last shown, a
  version that was rolled back, and the hash of the `tome.mix` an update
  installed (`installed_mix`, cleared once its `tome.mix.old` is removed or
  rolled back to).
- **Developer builds** never ask or check without `TOME_DEBUG_UPDATE_URL`
  (the URL to ask instead, `http://` allowed; the release's files are
  fetched from `<tag>/SHA256SUMS[.sig]` next to it): the tests start from
  fresh `TOME_HOME`s and would get the question every time.
  `TOME_DEBUG_RELEASE_KEY` adds a test key. Checked by hand with a local
  file server: the question, the check, the popup with a release signed by
  the test key, and refusals with no key and with another key.

## Threads

| Thread | Does |
|--------|------|
| Main (game) | DllMain, InitNatives hook, mod lifecycle after startup, menu, events and chat commands |
| Bootstrap | first mod load, hashing, window discovery; exits |
| Watcher | file notifications and polling; posts to main |
| Short-lived | `post_delayed` timers, `TOME_DEBUG_CLICKS`, the update check's request |

## Testing

- `ctest` in `build/`: unit tests for everything in `loader/src/core` (run under Wine),
  and `tome_hook_runtime_tests` for the hook engine and the guard on real
  code: access violations, illegal instructions, divisions by zero,
  breakpoints, C++ exceptions, stack overflows (twice on one thread), nested
  guards, inner SEH frames unwound, callee-saved registers and FPU state
  restored, and crashing BEFORE/REPLACE handlers and a crash below a nested chain.
- `tome_host_tests` (`tests/host/`): the loader itself without the game,
  deterministic (nothing waits on a clock or a window). It links
  `tome_runtime`, the object library `tome.mix` is linked from (everything
  but `dllmain.cpp`), so it tests the code that ships. Each test runs in a
  process of its own (the program starts itself with `--run <name>`; a
  crash or a hang past 60 s is a failure): the loader keeps its state in
  globals. The harness (`harness.h`) gives a temporary `TOME_HOME` with the
  built fixture mods, the log kept in memory for checks, and a fake game
  image (`loader/src/testing/fake_game.h`, shared with the mod makers'
  tools below): committed memory at the real 1.27b RVAs from the embedded
  `mappings/1.27b.json`, zeroed, with `RCString::GetText` copied from its mapped bytes (the whole
  function), `InitNatives` and `BindNative` as jumps to recorders, and the
  three InitNatives call sites as real `call` instructions, so calling one
  is a map load and the bound thunks can be called like the JASS VM calls
  natives. Without a game window `main_thread::post` runs everything at
  once on the test's thread. It covers approval, loading, natives (integer
  and real ABI, C, C++ and Rust), disabled natives returning 0, crashes in
  load, enable, a native and a REPLACE hook (the original runs), two mods'
  hooks in priority order with revoking and the patch installed once,
  metadata errors, a 64-bit DLL, state.json edits, hot reload, removed
  mods, packages (unpacked, a folder winning, an update reloading, removal),
  safe mode at startup and from the menu, crash loops (`tome/startups`
  counted, the loop, the count set back) and crash reports (written for a
  fake crash, and for a real access violation nothing handles, through
  Tome's filter to the one before it), shutdown, and chat commands: the
  fake image carries every mapped function's bytes, so `events::install`
  hooks them all, and `CGameUI::HandleSlashCommand` is a runnable function
  that handles nothing (its mapped prologue, a cookie it can read and an
  epilogue of the test's), so `/tome` commands, a mod's command and the
  lines that go on to the game all run through Tome's real hook.
- `tome_game_tests` (`tests/game/`): the same harness against the real
  `game.dll` from `input/<version>/` (`TOME_GAME_DIR`, set by `make test <version>`;
  `tests/game_dir.h`), loaded into the test process (where
  Windows puts it, so the RVAs are exercised) but never run; each test is
  skipped without it. `Mss32.dll` doesn't load `.mix` files until the game
  opens its sound, so a `tome.mix` in the game folder stays out (the
  test checks). It checks the build's identity and signatures on the loaded
  image, applies everything `start_bootstrap` patches from DllMain (natives,
  every built-in patch, the sound wait, the mod menu, events, the
  handshake, the Esc menu) except loading Tome's frame definitions (that
  runs game code), hooks every mapped function's real prologue (181 of
  181), and runs the hook-a/hook-b and crash fixtures' handlers around the
  game's own `RCString::GetText`. Not yet: the memhack guard on the real
  JASS compiler (`docs/reverse-engineering.md`, "Running the compiler
  outside a game").
- Tools for mod makers (`loader/src/testing/`, in the SDK zip's `bin/` and
  `lib/`; `sdk/README.md`, "Testing a mod"): the same fake game in programs
  of their own, built from `tome_runtime` but not part of `tome.mix`.
  `tome-check` installs a mod folder, allows, loads, enables, binds,
  disables and unloads it, and reports `tome.mod.json` errors, a state other
  than active, natives declared but never registered, an unsafe icon, and
  every warning or error line Tome logged on the way (but the approval
  notice every new mod gets); `TomeMod.cmake` runs it after each build of a
  mod (`<target>_check`, with a stamp so it reruns when the mod changes).
  `tome_test.dll` (`sdk/include/tome/tome_test.h`) is a C API over the same
  session for mods' own tests: install, start (every mod allowed and turned
  on), state and reason, the kept log, natives called like the VM calls
  them, chat lines, events (`events::dispatch_for_test`), keybinds
  (`keybinds::press_for_test`), settings, and a runner that starts each case
  in a process of its own like `tome_host_tests`. For it the fake image also
  has `CreateJassString` and the trigger natives Tome runs code values with
  as fakes (string results kept, code runs counted), CGameWar3's instance
  set from the first map load (so string results and code values work) with
  `CGameWar3::GetPlayer` finding no players (the selection reads as none), a
  fake Storm (`storm::use_fake_for_test`: archives open, nothing is read),
  and the main thread set (`frames::set_main_thread`). Leftover homes (the
  mods' DLLs, which Tome never unloads) are removed by the next run once
  their process has ended; a failed run keeps its `tome.log` and stays.
  Tested by `tome_mod_tools_tests` (the Rust and Wasm examples, crashes, a
  missing dependency), `hello_tests` (`sdk/examples/hello/test_hello.c`)
  and `tome_check_<fixture>` (broken e2e fixtures and the error each gives).
- `tools/fuzz.sh [seconds] [target...]`: libFuzzer targets in `fuzz/`, one
  per untrusted input: lobby handshake lines and game-cache keys
  (`handshake`), chat commands (`chat`), packages (`zip`: directory,
  extraction and the package checks, asserting every accepted name stays
  inside its folder), raw deflate (`inflate`), `tome.mod.json`, settings
  schemas and keybinds (`json`), map scripts and native declarations
  (`jass`), paths, TOCs and package names (`paths`), BLP textures
  (`blp`), signatures (`signature`), what Wasm modules pass Tome's imports
  and the layouts Tome writes for them (`wasm_abi`, `wasm_layout`), and
  WebAssembly modules themselves through WAMR built in Tome's
  configuration (`wamr`; `docs/wasm-mods.md`). Built for the host
  with clang, ASan and UBSan in `build-fuzz/`, apart from the mingw build;
  seeds from `fuzz/make_seeds.py`, the corpus in `build-fuzz/corpus/`, and
  crashing inputs kept in `fuzz/regressions/<target>/`. They run 64-bit, so
  overflows only 32-bit `size_t` arithmetic would hit don't show up.
- `tools/e2e-test.sh` (`make e2e`): six real game launches with a temporary `TOME_HOME`.
  The game runs as `tools/test-env.sh` sets it up, the same on every
  machine: a temporary game folder of symbolic links to the files of
  `input/<version>/` (`GAME_VERSION`, default 1.27b; only read) with the build's `tome.mix` and its own `Maps/`,
  `save/`, `replay/`; a Wine prefix of the test's own (`build/test-env/wine`,
  made once: 800x600, a profile called Tome, the intro movie seen), so
  neither the user's prefix nor their settings matter; and, with
  `HEADLESS=1` (the default), an Xvfb server of its own with Mesa's
  software OpenGL, so the window always has the focus, `SetCursorPos`
  moves only that server's pointer and no GPU is needed. With `SOUND=0`
  (the default when headless) the game's sound goes to ALSA's null device
  through Wine's ALSA driver (`winepulse.drv` disabled with
  `WINEDLLOVERRIDES`, `ALSA_CONFIG_PATH` pointing at a config holding only
  a `null` default PCM; with the system's `alsa.conf` Wine also lists
  PipeWire's devices and plays through them): no sound server, nothing audible, and the game's
  sound start-up still succeeds. Without any device it shows a modal
  "Unable to initialize base sound services" dialog over the main menu
  that takes every click (sounds turned off in its settings don't avoid
  it). The prefix is remade when Wine changes (its version and DLL list
  are recorded), since Wine copies its DLLs into a prefix only when it
  makes it. Stopping uses `wineserver -k` on the test prefix, and "is it
  running" looks for `war3.exe` processes with that `WINEPREFIX`, so the
  user's own game is never touched.

  **Nothing is timed.** The input (`TOME_DEBUG_CLICKS`,
  `core/debug_script.h`, played by `debug_input.cpp` in developer builds)
  is paced by the game: `wait:<text>` for a `tome.log` line (each wait
  searches after the line the last one matched; the log keeps its lines in
  memory for this), `frames:<n>` for presented frames (`events::frames`),
  `settle` until no UI timer has ticked for 10 frames, and `until:<text>`,
  which repeats the last action every 30 frames until the text is logged
  after it, but only while no control has taken it: developer builds count
  `CControl::Click` calls (mouse, shortcut keys and Tome's clicks all pass
  there), so an action a button took is waited on, never done twice.
  Buttons are clicked by name (`click:<frame>[,<context>]`, through
  `frames::click` on the main thread), not at positions; the lobby's slot
  dropdown and the sixth launch's two clicks stay
  positional (the dropdown's items have no names; the others keep the
  mouse path to Tome's buttons tested). A text box gets the keyboard focus
  by name (`focus:<frame>`, `CFrame::SetFocus`): a click on it is dropped
  while Tome's panel is still lowering, which `settle` can't see (the
  panel's own animation doesn't run on UI timers). The in-game Menu button is a
  `SIMPLEBUTTON` (not a control), so the menu opens with F10. Clicks, keys and chat lines are spaced by frames too. Game
  screens and panels drop clicks while they slide in; their animations run
  on UI timers (`UITimer::Fire`, hooked only in developer builds), so
  Tome logs `Screen ready: <name>` once a screen's own timers have ticked
  and stopped for 10 frames (a screen that doesn't slide in, the score
  screen, never logs it: the test waits for `Screen: score screen` and
  `settle`; a screen with no ticks for a while isn't ready either, since
  the main menu waits about a second of clock time before it animates),
  and `settle` waits for all of them. `until`
  is only for effects logged in the same frame as the input (Tome's menu,
  which logs every page, `Mod menu page: ...`; mods' buttons and
  commands); `Screen: <name>` comes about a second after its click (the old
  screen slides out first), so those clicks wait for readiness and then
  click once. The test map logs what the input waits for (`e2e: dialog
  shown`), starts its save and load once hello answers again, and its
  victory check on the chat line `-e2e-done` and 20 s of game time (the
  game's clock, deterministic like the map's own events). The checks wait
  for their log lines; `E2E_WATCHDOG` (120 s) only bounds a hang. They
  never see the input driver's `Debug ...` lines, and a check of what the
  script just did (`set_enabled`, a file swapped) uses `check_new`, which
  looks only after the script's `mark`. `launch` refuses a script step with
  a fixed time (`@ms`).

  **The same registry every launch.** The game keeps the last map, the
  profile and its options in the registry, and `tools/game.sh` shares the
  prefix. A new prefix keeps a snapshot of its fresh user registry
  (`user.reg.pristine`); the e2e test saves yours, starts every launch from
  the snapshot plus its own values (the LAN game on port `E2E_PORT`, 6119,
  so a game holding 6112 doesn't matter), and puts yours back at the end.
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
  - safe mode (sixth launch, `-tomesafe`, `tome/safe` and three failed
    starts in `tome/startups`): nothing loads, statuses say safe mode, the
    mod menu enables a mod anyway, state.json is unchanged, the main menu
    sets the count back, and no popup offers safe mode (`tome/safe` is
    there); every other launch starts with no count. The same launch turns
    a patch off in `tome/settings.json` and one with `-TomeNoPatch=` (with
    an unknown name, logged): neither is applied. The first launch checks
    that the crash filter went in front of the game's;
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
