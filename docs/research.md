# Tome: research notes

Tome is a mod loader for Warcraft III: The Frozen Throne, modelled on Fabric for
Minecraft. Development targets **1.27b** first. Later it will support all major
versions (§6), and the final phase is feature parity with patch 1.31 (§11). Each mod targets Tome's stable hooks, events and names.
Without Tome, every mod has to reverse-engineer `game.dll` again.

This is the original design document, kept for its reasoning. It is partly
superseded by these newer documents:
- `ROADMAP.md` for current status and what's next;
- `architecture.md` for how the loader works now;
- `reverse-engineering.md` for the verified game internals and addresses.

---

## 1. Target fingerprint

| File       | Version       | PE timestamp              | ImageBase    | SHA-256 |
|------------|---------------|---------------------------|--------------|---------|
| game.dll   | 1.27.1.7085   | 0x584a499e (2016-12-09)   | 0x6F000000   | `d51e5680243fc90e19c9d6074f7fac433c466d3cf5f46e2364291725574d8236` |
| war3.exe   | 1.27.1.7085   | 0x584a4994                | 0x00400000   | `0ea3434d2bdfd9609eda5fce2a68febac2137af72846de52e0008b142297ffbd` |
| Storm.dll  |               | 0x584a49bf                | 0x15000000   | `36339f69727f0f3dcb4431a9567b84f56007c331cfa3b183862af43e8314eb72` |

- Every binary is **32-bit x86** (PE32), so Tome and every mod must be built as 32-bit.
- `game.dll` has `DYNAMIC_BASE` set (DllCharacteristics 0x140). Do not hardcode
  absolute addresses. Store addresses as **RVAs** and add
  `GetModuleHandleA("game.dll")` at runtime.
- `game.dll` exports exactly one symbol, `GameMain`. `war3.exe` is a thin
  launcher that `LoadLibrary`s Game.dll and calls `GameMain`.
- The game links the **MSVC 2013 CRT** (`msvcr120.dll`/`msvcp120.dll`). Never
  pass CRT objects (`FILE*`, `std::string`, memory from `malloc`) between
  Tome/mods and the game.
- **Action item:** confirm these hashes against other retail 1.27b installs
  before treating them as canonical. If any distribution of 1.27b ships a
  different `game.dll`, the mappings need a separate entry for it (Tome
  identifies builds by hash and refuses unknown ones).

Tome should refuse to start (log a clear error and do nothing) unless
`game.dll`'s timestamp and hash match a build it has mappings for (§6).
Fabric does the same thing per Minecraft version.

---

## 2. Getting code into the process

### Verified: `.mix` files are loaded by Miles Sound System
`Mss32.dll` contains the search patterns `*.mix`, `*.asi`, `*.m3d` and `*.flt`.
It `LoadLibrary`s every match as a Miles provider (RIB) DLL. That's how
`RenderEdge_Widescreen.mix` in this install gets loaded: it's a normal PE32
DLL with a `.mix` extension.

**Recommendation:** ship Tome as `tome.mix` in the game root.
- No patched exe, no separate launcher, no injector. Users drop one file in.
- Plays nicely with RenderEdge and other `.mix` plugins.
- `DllMain` must stay minimal (loader lock). Install a small hook, or spawn
  a thread that waits for `game.dll`, then bootstrap from there.
- **Verified at runtime (milestone 1, under Wine 11):** `tome.mix` loads into
  `war3.exe`, and **`game.dll` is already loaded and initialized** when it
  does. Miles is started by game.dll, so Tome arrives after game.dll's own
  startup but, presumably, before any map loads.
- **Observed:** game.dll loaded at **0x766B0000**, not its preferred
  0x6F000000. Address randomization is real; RVAs are mandatory.
- **Threading caveat for hooks:** today the bootstrap runs on its own thread,
  in parallel with the game's main thread. Hooks that must be in place before
  a particular game function first runs (for example InitNatives) should be
  installed synchronously while the game is still waiting for Miles. Either
  do it from `DllMain` (patching memory is safe under the loader lock) or
  block in the first Miles callback. Decide in milestone 3.

Fallbacks, if the timing is too late:
- A proxy DLL (`ijl15.dll` is imported by game.dll; the warcraft3onlinefix
  project already replaces it on 1.27b).
- A launcher that starts `war3.exe` suspended and injects (the approach used
  by SharpCraft, EasyHook and w3l).

---

## 3. Mapping Fabric concepts onto Tome

| Fabric                     | Tome equivalent                                     | Difficulty |
|----------------------------|-----------------------------------------------------|------------|
| Fabric Loader              | `tome.mix`: bootstrap, mod discovery, dependency resolution, entrypoints | Medium |
| `fabric.mod.json`          | `tome.mod.json` (id, version, deps, entrypoints, `sync` flag) | Easy |
| Intermediary/Yarn mappings | **Symbol database**: names to RVAs, signatures and struct layouts for 1.27b | **Hard, and the core value** |
| Mixin                      | Function hooks with a shared chain (before/after/replace) | Medium |
| Fabric API                 | `tome-api`: events, JASS natives, assets, UI, config | Large, incremental |
| Resource packs             | Mod MPQs/folders layered over the game's MPQs       | Easy to medium |
| Loom (dev plugin)          | SDK: headers generated from mappings, CMake template, Wine dev loop | Medium |

### 3.1 Mappings (the core of the project)
Fabric works because mods never refer to obfuscated names. Tome's equivalent:
- A versioned, text-based database (TOML or JSON) in the repo, with entries like
  `jass.bind_native = { rva = 0x90D120, conv = "fastcall", sig = "void(void* fn, const char* name, const char* sig)" }`.
  Struct layouts (CUnit, CPlayer, CAbility, frames, …) live there too.
- Generate C headers from the database, so mods compile against names and never see raw addresses.
- Each entry gets a byte signature as well as an RVA, so that later builds
  (1.26a, 1.28) can be supported by re-resolving names. The 1.26a community
  knowledge base is large and most of it carries over.
- Keep a shared Ghidra project or exported symbols to grow the database. (Ghidra isn't installed here yet.)

### 3.2 Hooking (the Mixin replacement)
Native x86 code can't be rewritten the way Mixin rewrites bytecode. The practical equivalent:
- Inline hooks (MinHook, or a small hand-written x86 trampoline engine).
- **Tome owns every hook.** Mods register handlers against a mapped name, and
  Tome builds one ordered chain per target, with priority and
  cancel/replace semantics. If two mods patched the same prologue directly,
  they would conflict. Preventing that is half of what Fabric/Mixin
  provides.
- Hooking a non-standard calling convention needs per-target thunks (the game
  mixes `thiscall`, `fastcall`, `cdecl` and register-only functions). The
  mappings must record the convention.

**Implemented** (API version 4; see `architecture.md`, "Hooks"): a small
x86 decoder moves the prologue, one shared stub and dispatcher handle every
convention the mappings record (register-only functions aren't covered),
and chains run before/replace/after by priority.

### 3.3 JASS natives (the most valuable early feature)
Found in `game.dll` (RVA = VA − 0x6F000000):

| RVA        | What it is |
|------------|------------|
| `0x90D120` | **BindNative**. `__fastcall`: `ecx` = C function, `edx` = name (`"CreateUnit"`), stack = signature (`"(R)R"`). Parses the signature and counts the arguments. |
| `0x207490` | **InitNatives** for common.j. Calls `0x90CAC0` first (probably a table reset), then ~1,170 BindNative calls (`Deg2Rad`, `Rad2Deg`, …). Called from 3 sites (the call instructions are at RVAs `0x04542B`, `0x1DFD64` and `0x1E37A2`). |
| `0x713450` | A second table that registers ~1,290 of the same names, all pointing to one stub (`0x727800`). Probably a parse/validation-only environment. **Needs investigation.** Custom natives might have to be added here too, or the script compiler will reject them. |
| `0x9BDEC0` | ~124 more natives, probably `common.ai` (AI natives). |

**Implemented (milestone 3) and verified in game:**
- The 3 `call InitNatives` sites are redirected (call-site patch, bytes
  verified first) from `DllMain`, before the game continues. Tome's hook calls
  the original, then BindNative for every Tome native. In practice the engine
  calls InitNatives twice: once at startup, after Tome's bootstrap, and again
  at each map load.
- Natives declared in a map's `war3map.j` bind and compile fine through the
  main table alone; the second table (`0x713450`) isn't needed for map scripts.
- Each native gets a 16-byte thunk that jumps to the mod's function, or
  returns 0 while none is attached (`loader/src/natives.cpp`).
- **ABI confirmed:** `__cdecl`; integer/boolean/handle as 32-bit values;
  **real arguments are `float*`**; **real results are raw float bits in `eax`**
  (HelloDouble(1.25) → 2.5 round trip). Strings and code are not wrapped yet.
- Built-in natives: `TomeIsLoaded`, `TomeLogInteger`.

Design notes that still apply:
1. A map script that calls a Tome native must declare it (`native TomeFoo takes integer returns nothing`).
   Tome can also inject the declarations into `scripts\common.j` through the
   asset overlay, so maps don't need them.
2. **Graceful degradation:** a map that declares a native the engine doesn't
   know won't load. Document a pattern for maps that need to run with and
   without Tome (for example, check for Tome first and pick an alternate code
   path).
3. Strings and `code` are done (API version 6, `architecture.md`,
   "Strings and code"). Still to do: handles (resolving IDs through the
   handle table).

Signature type codes in the strings: `I` integer, `R` real, `B` boolean,
`S` string, `C` code, `H<name>;` handle, `V` nothing.

### 3.4 Assets (resource packs)
`game.dll` imports Storm entirely **by ordinal**. The relevant ones include
266 (SFileOpenArchive), 267/268 (SFileOpenFile/Ex), 269 (SFileReadFile) and
others in the 251–300 range.
- Simplest option: call `SFileOpenArchive` on each mod's MPQ with a higher
  priority than `War3Patch.mpq`. Storm's priority-based lookup then does the
  overriding.
- Better developer experience: hook the file-open ordinals so that a loose
  `mods/<id>/assets/` folder can override files without repacking an MPQ.
- Overridable this way: models, textures, SLK/TXT data, UI `.fdf`, `common.j`/`blizzard.j`.

### 3.5 Events and API surface (grow it over time)
Build these first, roughly in this order. Each one is a hook plus a mapped struct:
- Lifecycle: `game.dll` loaded, main menu, map load start/end, game start/end, per-frame tick.
- JASS: register natives, run code before or after `main`/`config`, read and write globals.
- Gameplay: unit created/removed, damage, order issued, ability cast. Many of these can wrap existing trigger events.
- Input and chat: chat commands, hotkeys.
- UI/render: a hook on the D3D present call or OpenGL swap (the game supports
  both; `-opengl` is used under Wine) for the mod menu (§8) and overlays.
  1.27b loads **Direct3D 9** at runtime (`d3d9.dll`, `Direct3DCreate9`, not a
  static import) and has an OpenGL path (`wglGetProcAddress`).
- Tome services: logging, per-mod config files, a crash handler that names the mod in the stack, and an in-game mod list.

---

## 4. Multiplayer, determinism and replays (design up front)

Warcraft III multiplayer is **lockstep**: every client simulates the same game.
Any mod that changes game state (natives used by the map, gameplay hooks)
will **desync** unless every player runs the same mod set. Replays break the same way.
- Every mod declares `"sync": "required" | "client"`. Client-only covers
  visuals, UI and QoL, which is like Fabric's `environment` but stricter.
- The lobby handshake must check that every player has the same
  `required` mods (ids, versions and hashes) before the game starts. Options:
  piggyback on an existing lobby packet, check over chat, or carry the list
  in the game name.
- Record the mod set in the replay (or next to it) so replays play back.
- Official Battle.net no longer serves 1.27b, so the realistic targets are LAN and PvPGN.
  Still, avoid features that look like cheats (map hacks); they would get Tome
  blocked or banned in communities.

---

## 5. Mods folder and live enable/disable

Goal: mods that depend on Tome live in a `mods/` folder, and mods can be turned
off and back on while the game is running, without restarting it. Fabric can't
do this. It's achievable here because Tome sits between the mods and the game
for every hook, event, native and asset.

### 5.1 Folder layout
```
WarCraft III/
  tome.mix                 the loader (picked up by Miles)
  tome/
    state.json             which mods are enabled (Tome writes this; the user can edit it)
    tome.log
  mods/
    example-mod/
      tome.mod.json        metadata
      example-mod.dll      optional native code (32-bit, C ABI)
      assets/              optional file overrides (same paths as inside the MPQs)
      config/              the mod's own settings; kept when the mod is disabled
```
A mod can be assets-only (no DLL) or code-only (no assets).

`tome.mod.json` fields that matter for toggling (the full schema is in §7):
```json
{
  "id": "example-mod",
  "version": "1.0.0",
  "depends": { "tome": ">=0.1.0", "other-mod": "^2.0.0" },
  "entry": "example-mod.dll",
  "sync": "client",
  "reload": "live"
}
```
- `sync`: `"client"` (visuals, UI, QoL; only this player) or `"required"`
  (changes the simulation; every player must have it, see §4).
- `reload`: how quickly a toggle takes effect: `"live"` (immediately),
  `"map"` (on the next map load) or `"restart"`. Tome enforces it and the UI
  tells the user what to expect.

### 5.2 Two operations, not one
- **Disable/enable (soft):** the DLL stays loaded. Tome stops dispatching to the
  mod and revokes everything it registered. Re-enabling calls the mod's enable
  function again. This is cheap and safe, and is the default.
- **Unload/reload (hard):** `FreeLibrary` and load again. This enables hot
  reload while developing a mod. Tome loads a *shadow copy* of the DLL
  (`tome/cache/example-mod.<hash>.dll`), so the original can be rebuilt while
  the game runs. A file watcher (`ReadDirectoryChangesW`) on `mods/` notices
  new mods and rebuilt DLLs.

### 5.3 Mod lifecycle (C ABI)
```c
// exported by every code mod
TOME_EXPORT int  tome_mod_load(const tome_api* api, tome_mod* self);   // once per DLL load
TOME_EXPORT void tome_mod_enable(tome_mod* self);                      // register handlers here
TOME_EXPORT void tome_mod_disable(tome_mod* self);                     // undo game-side effects
TOME_EXPORT void tome_mod_unload(tome_mod* self);                      // free memory before FreeLibrary
```
States: `discovered → loaded → enabled ⇄ disabled → unloaded`.

### 5.4 What makes live toggling work: Tome owns every registration
Every registration API takes the mod's handle, and Tome records an owner
for each registration. On disable, Tome revokes all of a mod's
registrations, so a mod that forgets to clean up still turns off:

| Registration             | What disable does |
|--------------------------|-------------------|
| Event handlers           | Removed from the dispatch list. |
| Function hooks           | Tome's patch stays in the game code permanently. Only the mod's link is removed from the chain, so game bytes are never unpatched while a thread might be running them. |
| JASS natives             | Kept bound. They now point at a stub that returns a default value (0 / null / false) and logs once (see below). |
| Asset overrides          | Removed from the overlay. Files the game has already loaded and cached stay until it reloads them, so these are effectively `reload: "map"`. |
| Timers, UI frames, chat commands | Destroyed or removed. |

What Tome **cannot** undo automatically is the effect a mod has already had
on the game: units it spawned, changed unit stats, frames it created
through JASS. That's what `tome_mod_disable` is for, and it is documented as
part of the mod contract.

### 5.5 JASS natives need special handling
- Once a map script is compiled, references to natives are baked in. They
  can't be removed in the middle of a map.
- If a map declares a native the engine doesn't know, the **map fails to
  load**. So Tome binds natives for **every installed mod, enabled or not**.
  Each native goes through a Tome trampoline that checks whether its mod is
  enabled. If it is, the trampoline calls the mod's function; if not, it
  returns a default value.
- The native list comes from `tome.mod.json` (a `natives` array), so it is
  known without loading the DLL.

### 5.6 When toggles happen: one safe point on the main thread
The game simulation runs on one main thread. A toggle request (from the UI, a
chat command or a `state.json` edit) only queues the change, and Tome applies
it on the main thread while no mod code is on the stack.

**Implemented:** Tome subclasses the game window and posts itself a private
message (`loader/src/main_thread.cpp`). The handler runs from the game's
message loop, never in the middle of JASS execution, and needs no
reverse engineering, so it works on every build. The watcher thread only
compares file timestamps; the refresh itself is posted to the main thread.
The e2e test checks that disables run on the window's thread. The first mod
load is the one exception: it runs on the bootstrap thread, because nothing
can call into mods yet, and the InitNatives hook waits for it (at most 10 s),
so `-loadfile` maps always see enabled mods.

Still to do: if a mod starts its own threads, wait for them before
`FreeLibrary` (an in-flight counter per mod).

### 5.7 Rules for when a toggle is allowed
- `sync: "required"` mods are **locked during a multiplayer game or replay**.
  A toggle request is queued and applied when the game ends. Toggling one in
  the middle of a game would desync.
- `sync: "client"` mods can be toggled at any time.
- Dependencies: disabling a mod also disables the mods that depend on it (the
  UI lists them first). Enabling a mod enables what it needs first.
- Crash isolation: every call into a mod is wrapped in SEH. If a mod faults,
  Tome disables it, logs which mod faulted and where, and the game keeps
  running.

### 5.8 Ways to toggle
1. Edit `tome/state.json`; the file watcher picks up the change. Works from the first milestone.
2. Chat commands: `/tome list`, `/tome disable <id>`, `/tome enable <id>`, `/tome reload <id>`.
3. The in-game mod menu (§8).

---

## 6. Supporting multiple game versions

Goal: one Tome install works on every major version, and most mods work
on all of them **without being rebuilt**. This is the same problem Fabric
solves with intermediary names that stay stable across Minecraft versions.

### 6.1 Versions worth supporting
Each "major version" is in practice one specific build the community
standardized on. A suggested priority:

| Build | Why | Notes (to verify per build) |
|-------|-----|-----------------------------|
| **1.27b** | Development target | This repo. |
| **1.26a** | The most widely used modding version, with the largest existing reverse-engineering knowledge | Same engine generation as 1.27; many symbols carry over by signature. |
| 1.27a | Chinese platforms (KK/DzAPI) | Close to 1.27b, but different addresses. |
| 1.24e | Older custom-game communities | Before 1.24 the "return bug" handle exploit worked; on 1.24+ it doesn't. |
| 1.28.x | Last version before the 1.29 overhaul | Shares 1.27's executable layout. |
| 1.29.2, 1.30.4, 1.31.1 | Modern pre-Reforged builds | Larger layout changes: I believe `game.dll` was merged into the exe and data moved to CASC around 1.29–1.30 (needs checking). 1.31 adds a 64-bit build; Tome could support only the 32-bit one, or build twice. |
| 1.20e and earlier | Possible | Low priority. |
| Reforged (1.32+) | **Out of scope** | Different engine, live service, 64-bit only. |

### 6.2 What changes per version, and how Tome hides it
- **Version detection:** Tome identifies the build by hashing the game binary.
  It loads `mappings/<build>/` if that build is known, and refuses to start
  otherwise. Versions are compared as build numbers (`1.27.1.7085`); friendly
  names (`1.27b`) are aliases.
- **Mappings per build:** each build gets its own mapping folder using the
  **same names**. A mod written against `unit.get_armor` or
  `jass.bind_native` works on every build where that name is mapped. Byte
  signatures (§3.1) make it quicker to port a new build: run the scanner,
  then fill in the gaps by hand.
- **Tome API = the stable layer.** Mods call the Tome API (a C function table).
  Raw struct offsets and addresses are an escape hatch:
  `tome_resolve("CUnit.armor")` returns this build's value, or fails cleanly.
- **Capabilities:** some features exist only on some builds (native `Blz*`
  natives on 1.29+, Lua on 1.31). Tome reports capabilities (`has("lua")`,
  `has("native:BlzGetUnitArmor")`) so mods can adapt, and metadata can require them.
- **Injection per build:** `.mix` loading through Miles is verified only on
  1.27b. Other builds may need the proxy-DLL or launcher fallback (§2).
  Keep the bootstrap code separate from the version-specific parts.
- **Tome API versioning:** the function table is append-only and starts with
  a size field. Tome follows semver; a mod declares `"tome": ">=0.3"`. Old mods
  keep working on newer Tome versions.

### 6.3 Checking compatibility before loading
Each mod lists, in metadata, the mapped names and capabilities it needs
(`requires`). Before loading any DLL, Tome checks them against the current
build's mappings and can say exactly why a mod won't run ("needs
`CUnit.armor`, which isn't mapped for 1.24e"). The mod menu shows the same
message. This also tells mapping contributors which names are most wanted.

---

## 7. Mod metadata (`tome.mod.json`)

Modelled on `fabric.mod.json`. Required fields are marked `*`. The comments are
for illustration only; the real file is plain JSON.

```json
{
  "schema": 1,                                  // * metadata format version
  "id": "better-hp-bars",                       // * lowercase, unique, stable forever
  "version": "1.2.0",                           // * semver
  "name": "Better HP Bars",
  "description": "Colored, scalable health bars.",
  "authors": ["Someone"],
  "license": "MIT",
  "icon": "icon.blp",                           // .blp or .tga; shown in the mod menu
  "links": { "homepage": "...", "source": "...", "issues": "..." },

  "game":  ">=1.26a <1.29",                     // * supported game builds (range or list)
  "tome":  ">=0.3.0",                           // * Tome API version
  "depends":    { "tome-ui": "^1.0" },          // must be present and enabled
  "recommends": { "nicer-fonts": "*" },         // shown in the menu, not enforced
  "conflicts":  { "old-hp-bars": "*" },         // warn
  "breaks":     { "hp-bar-legacy": "<2.0" },    // refuse to enable both

  "entry": "better-hp-bars.dll",                // or per build: { "1.31": "x64/mod.dll", "*": "mod.dll" }
  "assets": "assets/",
  "requires": ["CUnit.hp_bar", "event:frame"],  // mapped names / capabilities (§6.3)
  "natives": ["native TomeHpBarSetColor takes unit u, integer argb returns nothing"],

  "sync":   "client",                           // * client | required (§4)
  "reload": "live",                             // live | map | restart (§5)
  "load":   { "after": ["tome-ui"], "before": [] },

  "config": "config.schema.json",               // generates the settings screen (§8)
  "keybinds": [{ "id": "toggle", "default": "Ctrl+H", "label": "Toggle bars" }],
  "provides": ["hp-bars"]                       // virtual ids other mods can depend on
}
```

Notes:
- Version ranges use the same syntax for mods and for game builds. Tome maps
  `1.26a` etc. to build numbers so `>=1.26a <1.29` works.
- `sync: required` mods are hashed (metadata + DLL + assets). The hash is the
  identity used in the multiplayer handshake and the replay record.
- Built-in Tome components (API modules, `tome-compat-131`, `tome-lua`) use
  the same metadata. They appear in the menu like any other mod.

---

## 8. In-game mod menu

The mod menu is required, and it must be reachable **from the main menu and
during a game**.

### 8.1 Rendering: the game's own UI frames (implemented on the main menu)

**Decision (2026-10-24): native frames, not an ImGui overlay**, so the menu
looks like part of the game. Implemented in `loader/src/mod_menu.cpp` and
`loader/src/core/main_menu_fdf.cpp`; covered by the e2e test (simulated clicks).

How it works:
1. Tome generates its own `UI\FrameDef\Glue\TomeModMenu.fdf` (a "Mods"
   button built from the same templates as the main menu buttons, and a panel
   with the Battle.net dialog backdrop) plus a `Tome.toc` that lists it.
2. It writes them into an overlay MPQ (`tome/cache/overlay-N.mpq`, written by
   Tome's own MPQ writer) and opens it through Storm at priority 15, above
   `War3Patch.mpq` (8). (See architecture.md, "Asset overlay".)
3. The game has already read `FrameDef.toc` by the time Miles loads Tome, so
   Tome calls the TOC *processor* on `Tome.toc` from `DllMain` (main thread).
   That parses Tome's definitions **on top of** the game's. Reloading all of
   `FrameDef.toc` instead clears the definition tables. Frames the game has
   already created still point into those tables, and the next map load
   crashed; this was observed.
4. A `CMainMenu` vtable hook (setup) creates Tome's frames inside the menu's
   `ControlLayer` and binds their buttons to new command ids. A second hook
   (the menu's event handler) receives those ids; the game's handler ignores
   ids above 0x21.
5. Definitions that already exist are **not** replaced by a later load, so
   Tome adds new top-level frames rather than patching Blizzard's `MainMenu.fdf`.
   No game file is modified or redistributed.
6. **Native placement:** the Mods button takes the upper slot of the main
   menu's bottom box, and Tome moves Quit down to the lower slot (SetPoint).
   These are the same positions the Options screen uses for OK and Cancel.
7. **Native window:** the panel hangs in the game's own chained panel model
   (`UI\Glues\SpriteLayers\Expansion\TopLeftPanel-Expansion.mdx`, a SPRITE
   frame). It plays the same animations as the Options screen's left panel:
   "Options Morph" to lower it, "Options Stand Alternate" while open, and
   "Options Morph Alternate" to raise it. The content is hidden while the panel
   moves, and the WarCraft III logo is hidden while it's open (as on Options).

Reverse-engineered for this (1.27b RVAs, in `loader/src/mappings.cpp`):

| What | RVA / detail |
|------|--------------|
| LoadFrameDefToc (clears the tables, then loads) | `0x0E48F0` `__fastcall(toc, context)`; startup call at `0x045426` |
| LoadTocFiles (loads without clearing) | `0x0BA580` `__fastcall(toc, tableA, tableB, status)`; tables `0xD47744`, `0xD47794`, default status `0xCE3A4C` |
| CreateFrame | `0x0E4740` `__fastcall(template, parent, 0, 0, 0)` |
| CLayoutFrame::SetPoint | `0x111600` `__thiscall(point, relative, relativePoint, float x, float y, update)` on `frame+0xB4`. Points use 1.31's FRAMEPOINT numbering (TOPLEFT=0 … BOTTOMRIGHT=8); 347 callers |
| CSpriteFrame::GetInstance | `0x0F7060` `__thiscall()` reads `[sprite+0x178]` |
| SetAnimation | `0x1A42B0` `__fastcall(instance, const int* tags, count, flags)`. Tag ids registered at `0x352A60`: birth 0, death 1, stand 2, morph 3, alternate 4, found 5, mainmenu 6, …, options 0x17. Sequences are picked by tag set, e.g. {stand, options, alternate} |
| Glue sprite layers | Created around `0x2E0BFA` from the skin keys `GlueSpriteLayerTopLeft`/`TopRight`; models in `UI\Glues\SpriteLayers\`, with one animation set per screen |
| GetFrameByName | `0x0F2CA0` `__fastcall(name, context)` |
| CTextFrame::SetText | `0x0FDE90` `__thiscall(text)`; text stored at `+0x1E8` |
| CFrame Show / Hide | vtable `+0xD4` / `+0xD0`. Hide sets bit 0 of `+0xB0`; children get bit 1 ("hidden by parent"). Easy to get backwards: I did at first. |
| Glue BindControl | `0x2E1460` `__thiscall(name, command, 1)`: registers event `0x40090064` with the menu as listener |
| `CMainMenu` vtable | `0xAB1894` (RTTI `.?AVCMainMenu@@`): slot 3 = event handler `0x2EFB70` (command id at event+8), slot 55 = setup `0x2DBE90` |
| Storm ordinals used by game.dll | 266 OpenArchive, 268 OpenFileEx, 265 GetFileSize, 269 ReadFile, 253 CloseFile |

Since done (2026-10-25): the same pages in the in-game Esc menu, details
with problems, generated settings pages, keybinds and a safe-mode switch
(architecture.md, "Mod menu"). An ImGui overlay remains an option for a
developer console, not for the mod menu.

### 8.2 Opening it
- Main menu: a "Mods" button (overlay, or injected into the main menu frame later).
- In game: an entry in the in-game Esc menu (done: a Mods button beside
  Options) and a hotkey (done: Tome's own keybind, Ctrl+F10 by default,
  changeable on the Keybinds page).
- While the menu is open it captures mouse and keyboard (WndProc hook), so
  clicks don't issue unit orders. **In multiplayer the game keeps running**;
  the menu never pauses the simulation.

### 8.3 What it shows
- **Mod list:** name, icon, version, authors, enabled state, a `sync` badge
  (required/client), and a "locked during this game" indicator (§5.7).
- **Toggle**, with the consequence shown up front ("applies now" / "next map" /
  "restart required"), and dependent mods listed before cascading (§5.7).
- **Problems:** wrong game build, missing dependencies, unmapped `requires`,
  crashed and auto-disabled (with the log excerpt).
- **Per-mod settings**, generated from `config.schema.json` (bool, int/float
  slider, enum, color, string, keybind). This is Fabric's Mod Menu + Cloth
  Config, built in. Mods can also register a custom panel.
- **Keybinds:** one screen for all mods' keybinds, with conflict detection
  against other mods and the game's hotkeys (`CustomKeys.txt`).
- **Tome:** version, game build, log viewer, "open mods folder", safe-mode toggle.

### 8.4 Before the menu exists
The `state.json` watcher and `/tome` chat commands (§5.8) cover the same
operations. The menu calls the same internal API, so nothing is duplicated.

---

## 9. Other things to plan for

Items easy to miss that would be painful to retrofit:

1. **Safe mode.** If a mod crashes the game at startup, the menu is unreachable.
   Starting the game with the `-tomesafe` flag, holding Shift during launch, or
   placing a `tome/safe` file should start Tome with every mod disabled. Also
   detect a crash loop (several crashes during startup in a row) and offer
   safe mode automatically.
2. **Security: mods are native code with full access to the machine.** Never
   auto-download DLL mods from other players in a lobby. At most, offer
   asset-only mods, or a link to the source. Show hashes in the menu. Signing
   can come later.
3. **Saved games.** A save made with `required` mods should record the mod set
   and warn or refuse when it's loaded without them. Mods need an API to store
   their own state in saves, because JASS natives and hooks don't persist on their own.
4. **Replays.** Record the mod set (ids, versions, hashes), and refuse or warn
   when playing a replay with a different set.
5. **Inter-mod APIs.** Mods expose services to other mods (like Fabric
   entrypoints and custom events): `tome_service_register("hp-bars", &vtable)`,
   plus custom event types other mods can subscribe to. `provides` in metadata
   supports this.
6. **Map ↔ Tome feature detection.** Natives such as `TomeIsLoaded`,
   `TomeHasMod("id")` and `TomeModVersion("id")` let a map adapt, with a
   documented pattern for maps that must also run without Tome (§3.3).
7. **Config storage:** per-mod files in `mods/<id>/config/`, versioned, so a
   mod can migrate old configs.
8. **Localization:** mod names, descriptions and settings labels in multiple
   languages, following the game's locale.
9. **Diagnostics:** crash reports that list the loaded mods and which module
   faulted; per-mod log channels; a debug overlay with frame times per mod
   (hot paths such as damage and tick events must stay cheap).
10. **Existing tools:** the goal is to *port* the major ones to Tome (§10),
    not just coexist with them. Until a port exists, Tome detects the
    original and avoids hooking the same place.
11. **Packaging and updates:** folder mods for development, plus a single-file
    archive (e.g. `.tome`, a zip) for distribution. A Tome self-updater and a
    mod index (like Modrinth) can come later; keep the metadata ready for it
    (`links`, `icon`, `license`).
12. **Testing and CI:** a reproducible Wine-based test run (launch a test map,
    run assertions through natives, exit), a mapping validator per build, and
    CI that builds the loader plus example mods.
13. **Project licensing and legal:** pick a license for Tome (Fabric uses
    Apache-2.0). It must be GPLv3-compatible so GPL mods can be ported (§10.3).
    Never ship Blizzard files. Mappings and patches are fine to
    commit; game binaries are not.
14. **Platforms:** Windows plus Wine. The native macOS builds of these
    versions would be a separate port, so declare them out of scope.

---

## 10. Porting the major existing mods

The end state is that the big existing Warcraft III mods and tools run *as
Tome mods*: one loader, one menu, one place to toggle things, and multiplayer
checks that cover them. Fabric's ecosystem grew the same way: popular mods
were ported until the loader became the default.

### 10.1 Candidates (licenses and sources still to confirm)

| Mod / tool | What it does | Source / license | Port shape |
|------------|--------------|------------------|------------|
| **UjAPI** | Hundreds of extra natives | Public, **MIT** | Direct code port. Also proves Tome's native pipeline at scale. |
| **WFE** (Warcraft Feature Extender) | FPS unlock, widescreen, map-size limit removal for online hosting, BLP 512 limit removal; supports 1.24e/1.26a/1.27a/1.27b/1.28f | Unknown (Hive release) | Clean reimplementation or author cooperation. Small, independent features, so a good early batch of `client` mods. |
| **RenderEdge** | Widescreen and rendering improvements (`.mix`) | Unknown, not found on GitHub | Reimplement, or ask the author. Overlaps with the 1.29-style widescreen work (§11.4). |
| **w3l** | PvPGN server connection loader (1.22a+) | Public, **GPL-3.0**, C | Port as a separate GPL mod (`tome-pvpgn`). Its network hooks feed the multiplayer handshake work (§4). |
| **YDWE** (+ japi natives) | Editor extension pack: JassHelper, debugger, extra (japi) natives (verify which parts are in the repo) | Public, **GPL-3.0** | Natives part → `ydwe-natives` mod. Editor part → depends on Tome loading in `worldedit.exe` (§11.7). |
| **DzAPI** (KK platform natives) | UI frames and platform natives used by many Chinese 1.27a maps | Proprietary platform | Clean-room reimplementation of the **same native names and signatures**, so existing DzAPI maps run on Tome. |
| **SharpCraft** | C# plugin framework | Check repo | Not a port, but a `tome-dotnet` language adapter could load its plugin style. Long term. |
| JNGP / WEX | Editor packs | Mixed | Editor-side; after World Editor support. |

### 10.2 How each port happens, best option first
1. **The author adopts Tome.** This is the best outcome and how Fabric's
   ecosystem actually grew. It needs a stable API, a good SDK and an
   example port to point at, so it only becomes realistic after milestone 6.
2. **Port the code** when the source is public and the license allows it
   (UjAPI under MIT, w3l and YDWE under GPL-3.0). Keep each port in its own
   repo with the original license and credit.
3. **Clean-room reimplementation** when the source is closed (WFE, RenderEdge,
   DzAPI). Match the features and, for native APIs, the exact names and
   signatures, so maps and users notice no difference. Don't copy code from
   their binaries.

### 10.3 Licensing consequence for Tome itself
GPL-3.0 code (w3l, YDWE) can be ported **as separate mods** that are GPL
themselves, as long as Tome's own license is GPLv3-compatible (Apache-2.0
and MIT both are). **Never pull GPL code into the Tome core** unless the
core is going to be GPL too. This affects the license choice in §9.13, so
make that choice before the first port.

### 10.4 Map compatibility comes from native ports
Many maps hard-depend on UjAPI, DzAPI or YDWE natives. A port that keeps
the exact names and signatures makes those maps playable on Tome. The
approach is the same as the 1.31 parity work (§11.1).

### 10.5 Migrating users
- When Tome detects the original next to the port (e.g.
  `RenderEdge_Widescreen.mix` plus a Tome widescreen mod), the mod menu offers
  to move the original aside, after confirming. It never deletes files itself.
- Ports use `provides` (§7) with the original's identity (e.g. `"provides":
  ["ujapi"]`), so other mods and maps can depend on the feature rather than
  on a particular implementation.
- Where possible, ports import the original's settings files, so users keep
  their configuration.

### 10.6 Ports drive the API
Each port is a real test of the Tome API, and gaps it hits become API work.
Line them up with the milestones:

| Port | Unblocked by | Exercises |
|------|--------------|-----------|
| UjAPI natives (first batch) | Milestone 3 (natives) | Native registration, JASS type marshalling |
| WFE features (FPS unlock, limit removers) | Milestone 4 (hooks) | Hook chain, `client` mods, live toggling |
| RenderEdge-style widescreen | Milestone 8 (menu/render hooks) | D3D9/GL hooks, UI layout |
| w3l / PvPGN | Milestone 7 (handshake) | Network hooks |
| DzAPI natives | Frame API (§11.1) | UI frames |
| YDWE natives + editor | World Editor support | Editor-side loading |

---

## 11. Long-term goal: 1.31 feature parity (last phase)

**Scheduled after everything else (loader, API, mod menu, multi-version) is
done.** The end goal is for Tome on older versions to offer what patch 1.31
offers mapmakers and modders. On a real 1.29–1.31 client, `tome-compat-131`
simply isn't needed: metadata (§7) lets it declare `game: "<1.29"`.

Patches 1.28 to 1.31 shipped in roughly five groups of features,
which differ a lot in how hard they are to reproduce. Each group becomes a
built-in Tome mod (for example `tome-compat-131`, `tome-lua`), so parity
features can be toggled and versioned like any other mod.

**Rule:** reproduce 1.31's API exactly: the same native names, signatures,
handle types and constants. Then existing 1.31 scripts, libraries and
tutorials work unchanged. Where 1.31 shipped with a known bug (for example,
`SetUnitAbilityCooldown` affected every unit with the ability), match the
later fixed behavior and document it.

### 11.1 Scripting API: 306 new natives (the core of parity)
Natives added in 1.29 to 1.31 (306 in all) are missing from 1.27b's
`game.dll`. Breakdown:

| Area | Approx. count | Notes / difficulty |
|------|---------------|--------------------|
| Bitwise, group helpers (`BlzBitAnd`, `BlzGroupGetSize`, …) | ~7 | Trivial. Good first natives to prove the pipeline. |
| Unit stats (armor, damage dice, attack cooldown, name, collision, …) | ~50 | Read/write fields on the `CUnit` struct. Easy once the struct is mapped. |
| Abilities (cooldown, mana cost, tooltips, icons, disable/hide) | ~60 | Ability object plus per-level data. Medium. |
| **Object modification API** (`Blz{Get,Set}{Ability,Unit,Item}{Integer,Real,Boolean,String}[Level]Field` plus field constants and new handle types) | ~40 + constants | Runtime editing of object data. Medium to hard: needs the per-object data layout for every field type. |
| Special effects (position, orientation, color, alpha, time scale, sub-animations) | ~25 | Effect wraps a model instance. Medium. |
| Items | ~20 | Easy to medium. |
| Mouse/key events (`GetTriggerPlayerMouseX`, …) | ~8 | Needs new trigger events, with input synced to all players. Medium. |
| **Damage events** (pre-armor `EVENT_PLAYER_UNIT_DAMAGING`, attack/damage/weapon type get/set, `BlzSetEventDamage`) | ~10 | Hook the damage pipeline before and after armor is applied. Hard but extremely popular. |
| **UI frame API** (`BlzCreateFrame`, `BlzFrameSetPoint`, …, plus loading `.fdf`/`.toc`) | ~45 | The largest piece. Needs the game's frame system reverse-engineered. The DzAPI frame natives used on Chinese 1.27a platforms prove it's doable on this engine generation. |
| **Sync** (`BlzSendSyncData`, `BlzTriggerRegisterPlayerSyncEvent`) | ~3 | Hooks the network layer. **Do this early:** it's also the channel Tome needs for its own lobby handshake and for syncing `required` mods (§4, §5.7). |

New types (`type abilityintegerfield extends handle`, …), constants and
`Convert*` natives go into an overlay copy of `common.j`. The matching
`blizzard.j` (BJ functions, including the 24-player `bj_` changes) and
`TriggerData.txt` updates go through the asset overlay (§3.4). Maps are
deterministic as long as every player runs Tome with the same compat version.

### 11.2 Engine limits and gameplay tweaks (medium)
From 1.29–1.31: JASS arrays 8,192 → 32,768, op limit raised to 1,666,666,
hashtable and game-cache key limits removed, food cap 999, resource cap
9,999,999, object limit 30,000, 16 tilesets, 256 bones per model, and stacking
of different buffs (1.30). Each one is a constant or allocation size to find and
patch. Some (array size) touch the JASS VM and saved games.

### 11.3 Lua scripting (medium-hard, high value)
1.31 added Lua 5.3 as an alternative to JASS (`war3map.lua`).
- Embed Lua 5.3. After InitNatives runs, walk the native table and
  **generate Lua bindings automatically from each native's signature
  string**. The same signature parser that registers mod natives handles
  this, so every native, including Tome's own, appears in Lua for free.
- Determinism is the hard part. Real 1.31 had desyncs from `pairs` order and
  GC timing. Lua handles must be ref-counted like JASS handles, and the
  `pairs` order must be deterministic.
- This also gives mods a scripting option (similar to Fabric's language adapters), not just maps.

### 11.4 Widescreen UI (medium; prior art exists)
1.29 added real widescreen with UI bookends. RenderEdge already does this on
1.27b, so this is a matter of getting that behavior right or integrating it,
plus making the frame API (§11.1) aware of the wider screen.

### 11.5 24 players and 480×480 maps (very hard; research spike before committing)
- **24 players:** player arrays and loops sized for 16 (12 + 4 neutral)
  throughout the engine, neutral players moving from slots 12–15 to 24–27,
  12 new team colors, lobby UI, the network protocol, map format (w3i),
  replays, and fog (1.29.1 itself shipped with fog bugs for players 13+).
  This is the riskiest item in the whole project.
- **480×480 maps:** terrain, pathing, fog and minimap buffers sized for 256×256.
- Recommendation: do these last, behind a spike to find out how many places hardcode 16 players / 256 cells.

### 11.6 Map compatibility (decide scope)
Maps saved by the 1.31 editor use newer file versions (w3i, object data
with new fields, 24-player slots, optionally `war3map.lua`). 1.27b can't open
them. Options:
1. An offline converter (1.31 map → 1.27b+Tome map). The easiest way to start; w3x2lni-style tools cover parts of it.
2. Translate on load through hooks and the asset overlay (seamless, but more work).
3. Out of scope: Tome offers 1.31's *features* to maps made for Tome.

### 11.7 World Editor
`worldedit.exe` also imports `mss32.dll`, so `tome.mix` probably loads in the
editor too (**to verify**). Parity there means: the editor sees the overlay
`common.j`/`blizzard.j`/`TriggerData.txt` so that new natives appear in GUI
triggers and script checking, plus vJass/JassHelper (1.30) and a Lua map
option. Lower priority: nested trigger folders, folder-based maps,
tooltip previewer.

### 11.8 Explicitly out of scope
Direct3D 11/Metal and 64-bit (1.31): 1.27b is a 32-bit binary. CASC and
`.w3mod` overlays: Tome's `mods/` folder is the equivalent.

### 11.9 Suggested parity order
1. Overlay infrastructure for `common.j`/`blizzard.j`, plus trivial natives (bitwise, groups) to prove the pipeline end to end.
2. Sync natives (unblocks Tome's own multiplayer features).
3. Unit, item, ability and special-effect natives (mostly struct mapping).
4. Damage events → object modification API → mouse events.
5. UI frame API.
6. Engine limits.
7. Lua.
8. Map converter, then World Editor integration.
9. Spike on 24 players / 480×480, then decide.

---

## 12. Toolchain and dev loop

- **Compiler:** `i686-w64-mingw32-g++` (Fedora `mingw32-gcc-c++`), C++20, with
  libgcc/libstdc++/winpthreads linked statically so `tome.mix` only depends on
  system DLLs. See the README for build commands.
- **Known MinGW limitation:** GCC doesn't support `__try/__except` on 32-bit
  Windows. Crash isolation for mod calls (§5.7) uses a hand-rolled SEH
  frame with `RtlUnwind` (`loader/src/guard.cpp`; see architecture.md). A
  vectored handler was the other option, but it sees every exception before
  the mod's and the game's own handlers do.
- **Mod ABI = plain C.** Mods get a function table (`tome_api*`) and export
  `tome_mod_load`/`enable`/`disable`/`unload` (§5.3). Mods can then be written in C, C++, Rust or Zig, and CRT
  versions don't need to match.
- Run the game under Wine (already installed) with `-window -opengl`. A
  `tome.log` plus `winedbg`/x32dbg (under Wine) covers runtime debugging.
- RE tooling: Ghidra (not installed), plus the existing `objdump` for quick checks.
- Never commit Blizzard files. `.gitignore` already excludes `WarCraft III/`.
  Mappings (addresses and names) are fine to commit.

Proposed layout:
```
loader/      tome.mix: bootstrap, version gate, mod discovery, hook manager
api/         tome-api: events, natives, assets, UI (as built-in "mods")
mappings/    one folder per game build (same names), plus header generator
sdk/         public C headers, CMake template, example mods
tools/       signature scanner, mapping validator
docs/
```

---

## 13. Prior art (to read before building; licenses still to check)

- **UjAPI** (MIT): many custom natives for Warcraft III. Good for which natives people want and how they're implemented. https://github.com/UnryzeC/UjAPI
- **SharpCraft** (C#, EasyHook): a plugin framework and successor to "Reinventing the Craft". This is the closest prior art to Tome. Hive thread 244317 (Hive blocks automated fetches; read it in a browser).
- **YDWE / japi**: the Chinese editor ecosystem. Native extensions plus a JASS VM debugger that hooks the VM main loop. https://deepwiki.com/actboy168/YDWE/4.1-jass-debugging
- **war3-Dev-Tool** (1.27a): a JassHook.dll with a native registry and dispatcher. https://github.com/MaSuo1032/war3-Dev-Tool
- **wc3env-127a**, **warcraft-cs**: 1.27a hook ports with offsets. They're 1.27a, so don't reuse their offsets for 1.27b blindly.
- **warcraft3onlinefix**: 1.27b-specific, uses an ijl15.dll proxy. https://github.com/jhoniwana/warcraft3onlinefix
- **jasm / JHCR**: JASS bytecode work (no VM documentation exists; this is the best available). https://lep.duckdns.org/blog/2020-09-25-jhcr-a-high-level-overview.html
- **RenderEdge**: an existing `.mix` plugin . Prior art for the 1.29-style widescreen UI.
- **jassdoc**: annotated `common.j`, with `@patch` tags that give the version each native was added. https://github.com/lep/jassdoc
- **DzAPI** (Chinese KK platform, 1.27a): UI frame natives on this engine generation. Prior art for §11.1 and a port target (§10).
- **WFE** (Warcraft Feature Extender): QoL tool for 1.24e–1.28f. Port target (§10). Hive thread 310863.
- **w3l** (GPL-3.0): PvPGN loader. https://github.com/w3lh/w3l
- **YDWE** (GPL-3.0): editor pack plus natives. https://github.com/actboy168/YDWE

---

## 14. Open questions

1. ~~When does Miles load `.mix` files?~~ After game.dll is loaded (verified). Still open: relative to the three InitNatives call sites, i.e. is it before the first map load in every path, including launching straight into a map with `-loadfile`?
2. What is the second native table (`0x713450`)? Not needed for natives declared in map scripts (verified); still unknown what uses it.
3. ~~JASS native calling convention~~ Verified: cdecl, `float*` real arguments, real results in `eax`. String and `code` marshalling: done (API version 6).
4. Where exactly to hook present/swap. (Answered in part: 1.27b uses D3D9 through `Direct3DCreate9`, or OpenGL.)
5. ~~Lobby packet format, to carry the mod-set handshake.~~ Lobby chat events (`0xB0010`) to single players, plus SyncStoredInteger actions in games (`reverse-engineering.md`, "The network client, the lobby and sync actions"). Still open: the host's net id.
6. Are this install's binaries identical to retail 1.27b? (See §1.)
7. Does reloading a map re-read assets through Storm, or does the game cache some (SLK data, textures) for the whole process? This decides which mods can be `reload: "map"` and which need `"restart"`.
8. Does `tome.mix` load inside `worldedit.exe` too (it imports `mss32.dll`)?
9. 24-player feasibility: how many places hardcode 16 players / 256×256 maps?

## 15. Suggested milestones

1. ✅ **Hello world** (done 2026-10-24): `tome.mix` loads, identifies the build by SHA-256, writes `tome/tome.log`, and appends `[Tome x.y.z]` to the window title. The main-menu message waits for the mod menu.
2. ✅ **Mod loading** (done): discover `mods/*/tome.mod.json` (full schema, §7), resolve dependencies, run the lifecycle (§5.3), and persist `tome/state.json`.
   Build per-mod ownership tracking now; live toggling (§5) depends on it, and adding it later is painful.
3. ✅ **Natives** (done): the `hello` example mod registers three natives; the e2e test builds a map that calls them, including with the mod disabled.
4. ✅ **Hook manager and first events** (done): the main-thread safe point (§5.6), the `state.json`/`mods/` watcher with hot reload, general inline hooks with a shared chain (§3.2), map load, game start and end, tick, every screen for `on_ui`, chat commands including `/tome` (API version 5).
5. ✅ **Asset overlay** (done): mods' `assets/` folders in one archive at Storm priority 12, applied per `reload` mode; `load_fdf` folded into Tome's generated overlay. Script overrides work (`Blizzard.j` tested).
6. **SDK:** generated headers, an example mod repo, documentation.
7. **Multiplayer handshake** for `sync: required` mods (done; verified in LAN games between two instances).
8. **Mod menu** (§8): ✅ native "Mods" button and panel on the main menu and in the Esc menu, with live Enable/Disable, paging, details and problems, generated settings, keybinds and a safe-mode switch (done). Since then also free-text settings in a text box, CustomKeys.txt in conflict detection, mod icons and a hotkey for the menu (Ctrl+F10).
9. **Second game version** (1.26a): proves the per-build mapping layer (§6), then the remaining builds in §6.1.
10. **Porting the major mods** (§10). Small ports begin as soon as their milestone exists (table in §10.6), so this runs alongside 3–9; the full set finishes here.
11. **1.31 parity**, in the order in §11.9. Done last.
