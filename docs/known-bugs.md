# Known bugs and exploits in 1.27b

Public bugs and exploits in Warcraft III: The Frozen Throne **1.27b**
(game.dll 1.27.1.7085, released 13 Dec 2016), collected October 2026.
The biggest risk is the JASS typecast exploit: a downloaded map can
likely read game memory and may be able to write it. The rest is crash
bugs, one desync and script limits that Blizzard fixed in 1.28 to 1.29.1.

**How to read this:** a bug counts as open in 1.27b when a later patch
note lists its fix, or when a source says 1.27b has it.

- *Official*: Blizzard patch notes or developer quotes.
- *Forum*: community posts. Hive Workshop refuses automated fetches,
  so these come from search snippets and haven't been verified here.

No exploit code or cheat tools are included.

## Script and memory exploits

| Exploit | Mechanism | Status in 1.27b | Source |
| --- | --- | --- | --- |
| Return bug | A function with two `return`s of different types casts any handle to an integer and back | **Fixed in 1.24** (hashtables replaced it). CVE-2009-4768: a crafted map could run arbitrary code on 1.24b and earlier | Official ([JASS Tools](https://jass.sourceforge.net/doc/retbug.shtml), [CVE](https://cve.circl.lu/cve/CVE-2009-4768)) |
| Local/global name shadowing | A local declared with a global's name changes the global's type while the script is parsed, which brings typecasting back | Reported working on 1.26 and 1.27 | Forum ([Hive](https://www.hiveworkshop.com/threads/we-are-back-to-square-one-warcraft-iii-is-unsafe-again.276196/)) |
| `I2C` and raw bytecode | With the typecast, an integer becomes `code`, which runs hand-built JASS VM bytecode. That reads and writes process memory ("memhack"), and maps that use it are called virus or ACE (arbitrary code execution) maps | **Unclear.** One post says "after patch 1.27b the exploit has been fixed, and it's no longer possible to write to memory". Another says writes were fixed in the 1.28 patches. Read-only access was reportedly kept on purpose. All of it was gone by 1.29 | Forum ([memory thread](https://www.hiveworkshop.com/threads/accessing-memory-from-the-script-its-time-of-the-revolution.279262/), [memory hack](https://www.hiveworkshop.com/threads/memory-hack.289508/page-7), [ACE note](https://www.hiveworkshop.com/threads/playing-maps-that-only-works-in-1-26-in-warcraft-1-28-5.355596), [detecting viruses](https://www.hiveworkshop.com/threads/memhack-how-to-detect-if-map-has-a-virus.351435/)) |
| `PreloadGenEnd` file writes | Maps write files through the preload natives (and `InitGameCache` and the save-game natives). Until 1.28.1 they go to the game folder, not the user data folder, so a traversing/absolute name writes anywhere | **Fix implemented** for every native that takes a file path, reads included (path sanitizer, see below); path changed in 1.28.1 | Official ([1.28.1](https://warcraft.wiki.gg/wiki/Warcraft_III/Patch_1.28.1)) |

Memhack libraries built on this (RenderEdge Memory Hack, XGM MemoryHackAPI)
target 1.26a ([RenderEdge](https://www.hiveworkshop.com/threads/renderedge-memory-hack.299926/),
[XGM](https://xgm.guru/p/wc3/memoryhackapi)).

**Open question:** does the typecast still write memory on 1.27b? A test
map run under Tome would settle it better than more reading.

**CONFIRMED on 1.27b (Oct 2026, dynamic RE).** The typecast gateway is open.
Measured in game (a dev probe mod, see `docs/reverse-engineering.md`, "The
memhack gateway: mechanism"): a `local` that shadows a `global` variable of a
different type **permanently repoints the global's type** for the rest of the
script's compile. Each name is one shared symbol whose type descriptor lives
at `symbol+0x18`; the local-variable declaration overwrites that pointer (ctor
`0x91DCF0`, swap `0x91DD5E`) and the compiler **never restores it** at
`endfunction`. Verified with `unit` global + `integer` local: a later function
sees the `unit` global as `integer` (type-ids: unit=7, integer=4, real=5),
i.e. the handle↔integer typecast. 1.27b does carry the full static type
checker (`type mismatch in assignment`, `expected a handle type`); the memhack
*defeats* it via this shadow, it isn't missing. The reads-vs-writes nuance is
moot: an open typecast coerces an integer to `code` and calls an arbitrary
address (ACE), worse than a memory write.

**Precise fix rule:** refuse/neutralize a `local` whose name equals a `global`
variable of a *different* type.

**Fix implemented and verified (transparent scope restore, option 3).** It is
a **built-in patch**, `loader/src/patches/memhack_guard.cpp` (always on, not a
toggleable mod; see `docs/architecture.md`, "Patches"). It hooks
`JassLocalVarDesc::ctor` (`0x91DCF0`, BEFORE: remember the global descriptor a
local is about to overwrite at `symbol+0x18`) and `JassScanner::nextToken`
(`0x9249D0`, AFTER: when it yields the `endfunction` keyword, token code
`0x115`, write the saved global descriptor back). It saves any descriptor
that isn't another local's, with no cap on how many, and `Jass::compile`
(`0x9146F0`) drops the list when a compile starts or ends, so a script that
stops inside a function can't leave stale entries. The three functions are
**reserved** (`hooks::reserve`), so the mod hook/unhook API refuses them: a mod
can't drop the fix or add a handler that re-opens the typecast (the boundary is
API-level — a native mod DLL can still write memory directly). This restores
the missing function-scope pop: the local
still works as its own type inside the function, and the global's type is
correct everywhere after `endfunction`, closing the typecast with no change to
legitimate code. Verified in game with a `unit` global / `integer` local
repro: inside the shadowing function `zzqG` reads the local (kind `0x29`,
type-id 4 = integer); in a later function it reads the restored global (kind
`0x2a`, type-id 7 = unit). Without the fix the later function saw type-id 4.

Function **parameters are NOT a vector** (verified in game): a parameter that
shadows a global of a different type leaves the global's descriptor unchanged
(`k=0x2a`, correct type) in the shadowing function and after it. Parameters use
a separate ctor (`0x91DD70`, kind `0x28`) that does not write `symbol+0x18`.
Only `local` variable declarations (kind `0x29`, `0x91DCF0`) carry the swap, so
the implemented fix covers the full vector.

**Built-in detector (option 1), implemented.** `tome::jass::scan_shadow_exploit`
(`loader/src/core/jass.*`, unit-tested in `tests/test_jass.cpp`) scans a
`war3map.j` for a `local` whose name matches a `global` of a different type.
`events.cpp::check_map_script` runs it at `map_load` (reading `war3map.j`,
or `scripts\war3map.j`, through Storm) with the globals of `Scripts\common.j`
and `Scripts\Blizzard.j`, so a local named after one of theirs counts too. It
logs a warning naming each finding, so a downloaded
virus/ACE map is visible in `tome.log` even though the compiler fix already
neutralizes it. At game start it also tells the player in the game's message
area (the map still plays: the guard has blocked the trick, and a map that
depends on it doesn't load at all). Mods see the count in
`tome_event_info.script_shadows` (map load, game start, game end) and the
details from `map_script_shadows` (API version 13). Verified in game: it reads and scans the loading map's script
(e.g. Lost Temple, 72,697 bytes) with no false positive on clean maps.

The e2e test map (`tests/e2e/patch_map_script.py`) declares two locals that
shadow a `unit` global with `integer`. `tools/e2e-test.sh` checks that the
detector reports them, that the guard restores the global, that each
function's last statement (`return`, `set`) still uses the local, and that a
later function sees the `unit` global.

### Natives that take a file path (`PreloadGenEnd`, `InitGameCache`, saves, assets)

`PreloadGenEnd(string filename)` writes the accumulated `Preload` buffer to a
file, `InitGameCache(string campaignFile)` opens/creates a cache file, and
`SaveGame`, `CopySaveGame`, `RenameSaveDirectory` and `RemoveSaveDirectory`
write, move and delete saves and save folders (1.27b has no
`CreateSaveDirectory`). On 1.27b the write is rooted in the **game folder**
(Blizzard moved it to the user-data folder in 1.28.1), so a map-controlled
name that traverses (`..\..\...\Startup\x.bat`), is absolute (`C:\...`), or
is a UNC/rooted path turns "write a file" into "plant a file anywhere the
process can write" — a persistence / code-execution primitive, not just a
cheat. The natives that read a file by name complete that chain or leak
through it: `Preloader` runs a file as script, `LoadGame` and `ChangeLevel`
load a save or a map, `StartMeleeAI`/`StartCampaignAI` load an AI script,
`SaveGameExists` probes for a file, and the model, texture, movie, music and
sound natives open assets by path.

**Fix implemented (path sanitizer, built-in patch)** for every native in
1.27b's `common.j` that takes a file path: the six writes above, the reads
above, `Preload`, `SetDayNightModels`, `SetSkyModel`, `SetIntroShotModel`,
`PlayModelCinematic`, `PlayCinematic`, `SetAltMinimapIcon`,
`CreateTrackable`, `QuestSetIconPath`, `MultiboardSetItemsIcon`,
`MultiboardSetItemIcon`, `SetCinematicCamera`, `SetCineFilterTexture`,
`CreateSound`, `CreateSoundFilenameWithLabel`, `SetMapMusic`, `PlayMusic`,
`PlayMusicEx`, `PlayThematicMusic`, `PlayThematicMusicEx`,
`GetSoundFileDuration`, `AddSpecialEffect`, `AddSpecialEffectLoc`,
`AddSpecialEffectTarget`, `AddSpellEffectTarget` and `CreateImage` (38 in
all). Other string arguments (sound labels, lightning codes, ubersplat and
ability names, attach points) are keys into the game's tables, not paths.
Tome's own `BlzLoadTOCFile` checks its path the same way, and also every FDF
path the TOC lists (`path_guard::toc_is_unsafe`), since the game loads those.

`loader/src/patches/path_sanitizer.cpp` (always on; see `architecture.md`,
"Patches") hooks the mapped `BindNative(fn, name, signature)` — called for
every built-in native on each map load — and, when the engine binds one of
these, remembers the engine's real `fn` and substitutes a sanitizing wrapper
as the bound pointer. No native address is hardcoded: the patch keys off the
native *name* at bind time, so any build that maps `BindNative` is covered.
Each wrapper has its native's `__cdecl` ABI (one 32-bit word per parameter,
the string argument an `RCString*` per `docs/reverse-engineering.md` "Native
ABI"), reads every path argument, and, if `tome::path_guard::is_unsafe`
rejects one, skips the call and returns 0 — nothing, `false`, 0 or a null
handle. The e2e test checks that the cache natives take a refused
`InitGameCache`'s null cache without crashing (`StoreInteger`,
`GetStoredInteger` reads 0, `SaveGameCache` returns false). A plain relative
name, subdirectories included (`Logs\\run.txt`), is untouched: that is these
natives' intended use. `is_unsafe` rejects an empty name (for the writes;
reads allow it, as maps pass `""` for an effect with no model), any `:` (drive
letter or NTFS alternate data stream), a leading separator (absolute,
drive-relative or UNC), any `..` component, and a reserved DOS device name
(`CON`, `PRN`, `AUX`, `NUL`, `CONIN$`, `CONOUT$`, `COM1`–`COM9`,
`LPT1`–`LPT9`, and `COM`/`LPT` with a superscript 1–3). It is portable and
unit-tested (`loader/src/core/path_guard.*`, `tests/test_path_guard.cpp`).
Refused calls are logged (`path-sanitizer: refused ...`; the first 20 in
full, then every 1000th, the name cut to 160 characters with control
characters replaced). The `BindNative` hook is **not** reserved: the threat
is malicious maps, not mods, and a mod can bypass any API boundary anyway, so
reserving it would only cost mods a useful hook. Content isn't touched — the
escape vector is the path, not the bytes.

### BLP texture loader reads past the file (`CBLPFile::Open`)

Warcraft III's own BLP1 texture loader trusts the file's header. For each
mipmap it reads an offset and a size from the 16-slot arrays at header offsets
`0x1C` and `0x5C` and copies that span out of the loaded file; for a JPEG
texture (any `compression` at `0x04` but 1) it reads a JPEG-header size at `0x9C`
and copies that many bytes. None of these is checked against the file's actual
length, and the mip count is derived from the width/height at `0x0C`/`0x10`
(down to 1×1), so a crafted `.blp` — a mip offset or size past the end, an
over-long JPEG header, or dimensions implying more than 16 mip levels — makes
the loader read past the buffer. In a downloaded map or a mod's assets that's
a crash (a denial of service) and an out-of-bounds read. Separately,
`CBLPFile::Open` copies a fixed 0x127-dword (1180-byte) header out of the
buffer *before* it checks the magic, so a file shorter than that is over-read
regardless of its contents.

**Fix implemented (BLP guard, built-in patch).** Tome doesn't decode BLPs
itself and the bug is the game's, so `loader/src/patches/blp_guard.cpp`
(always on; see `architecture.md`, "Patches") hooks `CBLPFile::Open`, the one
BLP1 header parser every texture load reaches. Open loads the whole file and
stores the buffer pointer and its length in the object (`CBLPFile.buffer` at
`+0x4A4`, `CBLPFile.size` at `+0x4A8` on 1.27b); an AFTER handler reads them
back and runs the portable validator `tome::blp::reject_reason` over the
bytes. The validator parses the BLP1 header itself and rejects a file whose
magic is `BLP1` but which is shorter than its header, has a zero dimension or
dimensions implying more than 16 mip levels, whose JPEG header runs past the end or past the game's 1180-byte header
copy (which it's copied from), whose decoded mips run past the end (a
paletted mip is sized by its dimensions and alpha bits, since the game
ignores `mipSizes` there), or which is paletted without room for its
256-entry palette. On a rejection the handler sets the result to 0 — exactly what Open
returns for a wrong-magic file — so the caller skips the mip/JPEG decode and
the texture fails to load instead of crashing. The hook is reserved
(`hooks::reserve`): a mod that dropped it would reopen the hole for every
texture.

The fixed-size header copy runs inside `CBLPFile::Open` before that AFTER
handler, so the handler can't prevent its over-read. The patch also rewrites
the copy site itself (`CBLPFile.header_copy`: the `mov ecx,0x127; rep movsd`)
to an absolute indirect call to a small detour (`tome_blp_header_copy`) that
copies only `min(1180, size)` bytes — the length the loader just stored — and
zero-fills the rest, so no file is ever read past its end. The detour runs
with the loaded buffer and the destination already in registers and reads the
size through a variable, so no struct offset is baked into the assembly; for a
file of 1180 bytes or more it's byte-for-byte the original copy. This rewrite
is best-effort and keyed to the exact bytes at the site (`patch_bytes` refuses
a mismatch): if a build doesn't map the site or its bytes differ, the clamp is
skipped and only the AFTER guard applies, which still refuses every malformed
file — just after that one bounded, read-only copy.

The decision logic is portable and unit-tested (`core/blp.*`,
`tests/test_blp.cpp`) and fuzzed (`fuzz/fuzz_blp.cpp`); the e2e test feeds the
menu a mod whose icon is a malformed BLP shorter than the 1180-byte header
copy, with a mip past its end, checks the
clamp is applied, and checks the guard refuses the file without crashing the
game. Covers maps and mods alike.

## Fixed only after 1.27b

These are all still present in 1.27b.

| Fixed in | Bug | Kind | Source |
| --- | --- | --- | --- |
| 1.29.1 (23 Apr 2018) | Hashtable storage limit; game cache mission key limit | Script limit | Official ([1.29.1](https://warcraft.wiki.gg/wiki/Warcraft_III/Patch_1.29.1)) |
| 1.29.1 | Neutral units in custom games don't accept item sales, don't show on the minimap and don't drop bounties | Gameplay | Official ([1.29.1](https://warcraft.wiki.gg/wiki/Warcraft_III/Patch_1.29.1)) |
| 1.29.1 | Founding of Durotar crashes | Crash | Official ([1.29.1](https://warcraft.wiki.gg/wiki/Warcraft_III/Patch_1.29.1)) |
| 1.29.1 | Custom textures crash the World Editor | Crash (editor) | Official ([1.29.1](https://warcraft.wiki.gg/wiki/Warcraft_III/Patch_1.29.1)) |
| 1.28.5 (6 Jul 2017) | Switching between Reign of Chaos and The Frozen Throne crashes | Crash | Official ([1.28.5](https://warcraft.wiki.gg/wiki/Warcraft_III/Patch_1.28.5)) |
| 1.28.2 (10 May 2017) | Raise Dead desyncs Windows and Mac players | Desync | Official ([1.28.2](https://warcraft.wiki.gg/wiki/Warcraft_III/Patch_1.28.2)) |
| 1.28.2 | CD key dialog crashes on Mac in some languages | Crash (Mac) | Official ([1.28.2](https://warcraft.wiki.gg/wiki/Warcraft_III/Patch_1.28.2)) |
| 1.28.1 (27 Apr 2017) | `PreloadGenEnd` writes to the game folder | Script | Official ([1.28.1](https://warcraft.wiki.gg/wiki/Warcraft_III/Patch_1.28.1)) |
| 1.28 (5 Apr 2017) | Command card buttons at invalid (negative) coordinates in `CommandFunc.txt` crash maps; 1.28 ignores them and keeps only (0, -11) and (-2147483648, -2147483648) for hiding | Crash | Official ([1.28](https://warcraft.wiki.gg/wiki/Warcraft_III/Patch_1.28), [BNETDocs](https://bnetdocs.org/news/125/what-s-next-for-warcraft-3)) |
| 1.28 | Disconnects from Battle.net when a game ends | Network | Official ([1.28](https://warcraft.wiki.gg/wiki/Warcraft_III/Patch_1.28)) |
| 1.28 | "Fixed various bugs and exploits", no details given | Unknown | Official ([1.28](https://warcraft.wiki.gg/wiki/Warcraft_III/Patch_1.28)) |

The 1.29.0 notes weren't checked (the wiki URL tried returned 404).
1.29 also added many natives and 24 player slots, which brought its own
new bugs; those don't apply to 1.27b.

## 1.27-era regressions

| Issue | Details | Status | Source |
| --- | --- | --- | --- |
| Ambient sound disabled | 1.27a known issue: "Disabled ambient sound while a MIDI issue is being resolved." | Still off in 1.27b. **Fixed in Tome** (ambient-sound): see "Ambient sound" below | Official ([1.27a](https://bnetdocs.org/news/105/warcraft-iii-1-27a-patch)) |
| High CPU use | "We're aware that the CPU is being used more than it should" (Dec 2016) | No fix found in 1.28.x notes | Official ([BNETDocs](https://bnetdocs.org/news/125/what-s-next-for-warcraft-3)) |
| Cinematics | Missing codec; "fixed internally and will be pushed out in a future patch" | Promised after 1.27b | Official ([BNETDocs](https://bnetdocs.org/news/125/what-s-next-for-warcraft-3)) |
| Mac replays | 1.27b made Mac players unable to save replays | Fix promised for 1.27c | Official ([BNETDocs](https://bnetdocs.org/news/125/what-s-next-for-warcraft-3)) |
| Saves need admin | Windows 8.1 and 10 saved games sit in a folder that needs admin rights | 1.27b known issue | Official ([PCGamingWiki mirror](https://community.pcgamingwiki.com/files/file/1180-warcraft-3-standalone-patches-all-languages-windows/)) |

1.27a also shipped without the Visual C++ runtime when patching from
older versions ([BNETDocs](https://bnetdocs.org/news/105/warcraft-iii-1-27a-patch)).

## JASS natives that crash or misbehave

*Forum*, from the [natives list](https://www.hiveworkshop.com/threads/producer-update-natives-list.301405/)
and crash threads. Which patch each was observed on isn't confirmed.

- Assigning `GetLocalizedString` to a `constant` variable crashes.
- Destroying a trigger whose threads are still waiting can corrupt the
  handle stack and often crashes.
- `MultiboardSetRowCount` is only safe when changing the count by one
  at a time.
- `CreateQuest` during map initialization can crash.
- `SetItemPosition` crashes in some situations (`CreateItem` plus
  `RemoveItem` is the reported workaround).

## Stretched picture on widescreen

In a window or resolution that isn't 4:3 (16:9, 16:10, 21:9), 1.27b draws
the 4:3 picture stretched to fit: units and buildings are a third too wide
at 16:9, the view shows no more of the map than at 4:3, and the UI is
stretched the same way (round minimap icons become ovals). 1.29 fixed it
with real widescreen (official). Checked under Wine on Booty Bay at 800 ×
600 and 1280 × 720: the same framing in both, every object 4/3 too wide.

The cause is the camera: its projection is built from its viewport in the
0.8 × 0.6 UI space, so the world camera always gets aspect 4/3
(`C44Matrix::Perspective`, `docs/reverse-engineering.md`, "Camera
projection"). **Fixed in Tome for the world view (widescreen, built-in
patch, on by default, Tome > Settings > Widescreen):** an AFTER handler on
Perspective scales the world camera's matrix (only the calls made from
`WorldView::SetupCamera`) by (4/3) / the window's aspect, so the view keeps
its height and shows more at the sides, as in 1.29; a window narrower than
4:3 (5:4) shows more at the top and bottom instead. At 4:3 nothing changes.
3D models inside the UI (portraits, the main menu's background) are left
alone, since their 2D frames are still stretched. Picking the ground uses
the same matrix (a move order at 90% of the width lands under the cursor).
Unit picking builds its own projection, through the world frame's
`CSpriteFrame::SetupCamera`: until the patch scaled that one too, a unit's
click area sat further from the middle of the screen than the unit (at
16:10, a peasant at 75% of the width answered a click about 100 px further
out, and near the sides off screen, so a building's larger area got the
click). Checked at 1600 × 1000 on Booty Bay: a click on empty ground there
selects nothing, and a peasant at 98% of the width selects. **Still open:** the 2D UI
(console, top bar, command card, minimap) stays stretched, and the
minimap's camera box keeps its 4:3 size. Like 1.29, a wider window sees more
of the map in multiplayer.

## Multiplayer cheating

Nothing reliable specific to 1.27b turned up. Client-side maphacks
(revealing the map) exist for the classic patches, and private servers
ship their own anti-hack launchers ([iCCup](https://www.winehq.org/pipermail/wine-bugs/2011-May/275877.html)).
Most "1.27b hack" search results were spam or malware downloads.

## Under Wine

Fullscreen Alt+Tab, found while running the game under Wine 11 (both
renderers lose the window's device or context when it's minimized).
Both are fixed by built-in patches:

- **OpenGL: textures gone.** Models, frames and text draw white after
  coming back. On restore the game releases its textures and makes a new
  context itself, and Wine also replaces the driver context behind an HGLRC
  (`win32u_wgl_context_reset`, `+wgl`). Textures uploaded in between are
  lost while the game keeps their names, and the new context hands the same
  small names out again, so `glIsTexture` still says yes (measured on the
  main menu: 29 of 118 textures came back). Reported as
  [Wine bug 13547](https://www.winehq.org/pipermail/wine-bugs/2009-June/182765.html)
  (2008). **Fixed in Tome (gl-context-restore, built-in patch, only under
  Wine):** on the first frame `CGxDeviceOpenGl::CanDraw` is true again after
  being false (minimized) in fullscreen, it runs the game's own
  `DestroyContext` and `CreateContext`, so every texture uploads again. If
  CreateContext fails, it's tried again on each frame until it works. Checked on the main menu,
  with no mods: complete after three Alt+Tabs.
- **Direct3D: the scene gets darker** after each Alt+Tab. The UI stays the
  same; on the main menu the water and the ice lose their blue light
  first, then the rest of the lighting. The cause isn't gamma: after the
  device Reset, `CGxDeviceD3d::RestoreState` sends the ramp again, and the
  game's D3D9 calls in a frame are the same before and after (`+d3d9`
  trace). It's wined3d: `wined3d_stateblock_reset` frees every light but
  doesn't clear its table of active lights (8 slots, outside the
  `stateblock_state` it clears), so the lights that were on keep their
  slots. When the game enables its lights again, wined3d refuses the ones
  that don't fit (`warn+d3d`: "Too many concurrently active lights"). With 5
  lights on, 2 are lost on the first reset, and more on each one after.
  **Fixed in Tome (d3d-reset-lights, built-in patch, only under Wine):** a
  BEFORE handler on `CGxDeviceD3d::ReleaseForReset`, which the game calls
  right before Reset, disables lights 0–7, which frees their slots;
  RestoreState enables them again. Not installed on Windows, where Reset
  clears the lights anyway.
  Checked on the main menu: no warning and the same colours after four
  Alt+Tabs. Not reported to Wine yet as far as we know.
- **Movies: a black screen forever.** Before a movie (the intro, the
  campaign cinematics) and after it, a windowed game puts the desktop's
  registry display mode back with `ChangeDisplaySettingsA(NULL, 0)` and
  tries again every 50 ms until that works (`Movie::SetDisplayMode`,
  `docs/reverse-engineering.md`). On a display where Wine can't change
  modes (Xvfb) it fails every time, so the movie never starts.
  **Fixed in Tome (movie-display-reset, built-in patch, only under
  Wine):** the innermost REPLACE handler skips the reset when the display
  is already in its registry mode, and otherwise gives up after about a
  second and plays the movie in the current mode.
- **Movies: small, with black all around (fullscreen).** A fullscreen
  game sets the first of its 800x600 and 640x480 cinematic modes that
  passes and makes the movie's window that size at 0,0. Proton takes the
  mode but emulates it (the monitor stays at its own size), so on a 16:9
  screen the movie sits in a 4:3 box with bars at the sides, besides its
  own letterbox. **Fixed in Tome (movie-display-reset):** under Wine the
  cinematic case puts the desktop's own mode back, also when the game runs
  in another, and writes it to `Movie.cinematic_mode` (as the game does
  when no mode passes); the window covers the screen and the
  video is scaled (smoothly) to its width. The bars above and below are the
  movies' own: they are 800x368, wider than 16:9. A smaller mode of the
  desktop's shape, scaled by Proton, was tried and dropped: Proton's
  scaling leaves a light line (one screen row) along the edge of Wine's
  video window (`wine_quartz_window`), wherever it is and whatever its
  background. Checked on GE-Proton11-11 in Xvfb at 2560x1440 and 2844x1600
  (the scale of 864x486 on 1440 lines): no light line in the desktop's
  mode, and the picture as smooth as in a scaled mode.
- **Movies: washed out, grainy, purple blacks (fullscreen).** For a
  fullscreen movie, Movie::Play sets a display gamma ramp of
  pow(x, 1 / 2.2) and puts the old one back after (`Movie.gamma_call`).
  Wine takes the ramp and Proton applies it to the movie's output (wined3d
  OpenGL; with `WINE_D3D_CONFIG=renderer=no3d` the movie is drawn without
  it and looks right). Black comes out at about 50 of 255, and the dark
  scenes' compression noise and chroma blotches show several times over.
  **Fixed in Tome (movie-display-reset):** under Wine that call, and the
  restore when a key skips the movie (`Movie.gamma_restore`, which after a
  windowed movie would set an all-zero ramp), go to a no-op, and the movie
  shows as encoded. Checked on GE-Proton11-11 at
  2560x1440 in Xvfb against frames decoded from the file: before, file
  levels 0-10, 20-30 and 90-100 came out at about 50, 90 and 164; after, at
  8, 27 and 97 (correlation 0.998).
- **Movies: a crash on the first frame (GE-Proton 11).** The movies' BLZ0
  video is decoded by the game's own DirectShow filter, `blizzard.ax`,
  whose DecideBufferSize asks for 4 bytes a pixel whatever format it
  connects with, and whose frames all claim the whole buffer
  (`docs/reverse-engineering.md`, "blizzard.ax"). Windows' renderer copies
  a frame by its surface's size. Wine's VMR7 copies the sample's length:
  connected as YUY2 (800x368, 588,800 bytes), it copies 1,177,600 into the
  surface, and the game crashes (an access violation writing in msvcrt's
  memcpy, called from `quartz.dll`). It only happens where Wine hands the
  compressed stream to `blizzard.ax` (the winedmo splitter: GE-Proton11-11);
  GE-Proton10-34's splitter decodes the video itself and plays the movies.
  It crashes without Tome too. **Fixed in Tome (movie-frame-length,
  built-in patch, only under Wine):** before the first movie Tome loads
  `blizzard.ax`, keeps it, and wraps its Transform to set each frame's
  length to one frame of the format it's in. Checked on GE-Proton11-11:
  the Sentinels opening cinematic plays to the end, windowed and fullscreen.
  The intro at startup plays before Tome loads, so with `seenintromovie` 0
  it still crashes there.

## Sound in the background

The game keeps playing its music and sound effects after Alt+Tab, or
whenever another window has the foreground, on Windows and under Wine.
**Fixed in Tome (background-mute, built-in patch):** on `WM_ACTIVATEAPP`
the game window's subclass mutes the game process's default Core Audio
session (`ISimpleAudioVolume::SetMute`) and unmutes it when the game comes
back, unless the user had muted it in the mixer already. Only the game's
own session: other programs and the system volume stay as they are. The
session covers everything Miles plays (2D mixer, 3D providers, music).

Windows' volume mixer remembers an application's mute for its next run,
so while Tome's mute is on, `tome/background-mute.flag` exists; if the game
crashes or is killed while away, the next start unmutes the session and
removes the flag. A normal quit doesn't need that: destroying the active
window deactivates the game first (which mutes it), so the `WM_DESTROY` that
follows undoes Tome's mute and removes the flag. Wine mutes inside its own mixer, so PipeWire/PulseAudio show the
stream as not muted.

Checked under Wine on the main menu by giving a second Wine program the
foreground: the game's stream was silent (`parec --monitor-stream`, peak 0)
and the flag existed; once the game was back, the music played again and the
flag was gone. Killed while away, the next start removed the flag. Not checked on
Windows yet.

## Ambient sound

A map's day and night ambience (birds, crickets, wind) never plays in
1.27b: 1.27a turned it off ("Disabled ambient sound while a MIDI issue is
being resolved") and no later patch turned it back on. Those sounds are MIDI
sequences with a DLS bank, made by `CreateMIDISound` from Blizzard.j's
`SetAmbientDaySound`/`SetAmbientNightSound`, which every melee map and most
custom maps call. Since 1.27a the native makes a plain sound from the label
instead, which can't find MIDI labels, so it's empty and never plays. The
MIDI driver, the DLS synthesizer and the rest of the MIDI sound code are
still there and work (saved games still use them).

**Fixed in Tome (ambient-sound, built-in patch):** a REPLACE handler on
`CSound::InitMIDI` (the innermost, so mods' handlers still run) makes the sound with the game's own MIDI constructor
(`CreateMIDISnd`) and gives it the ambient category, so the Ambient sounds
option still turns it off. Labels that aren't MIDI ones fall back to the
game's code. Only what each player hears changes: the native returns the
same handle as before.

Checked under Wine on Booty Bay with music off: without the patch the
game's audio stream was silent (`parec --monitor-stream`, RMS 0); with it
the Lordaeron summer ambience played (RMS ~170), and with Ambient sounds
off it was silent again. Details in `docs/reverse-engineering.md`, "Sound".
Not checked on Windows yet; what "MIDI issue" Blizzard had isn't known (the
game always uses Miles' software synthesizer for MIDI, never the system's
MIDI device).

## What Tome could do

1. **Map script guard:** before a map's script runs, refuse or warn on
   local/global name collisions and `I2C`/code-array patterns. That
   closes the exploit for players who download maps from others.
2. **Measure the memhack on this build** with a test map, to settle
   whether 1.27b still lets scripts write memory.
3. **Port the cheap 1.28 crash fixes:** ignore invalid command card
   coordinates. (Path traversal in `PreloadGenEnd` and every other native that
   takes a file path is already handled by the path-sanitizer patch, above.)
4. **Raise the hashtable and game cache limits** as 1.29.1 did, if a
   mod needs them (it changes game behaviour, so every player needs
   the same mod).

## Sources

- [JASS Tools: return bug](https://jass.sourceforge.net/doc/retbug.shtml)
- [CVE-2009-4768](https://cve.circl.lu/cve/CVE-2009-4768)
- [Hive: We are back to square one! WC3 is UNSAFE again](https://www.hiveworkshop.com/threads/we-are-back-to-square-one-warcraft-iii-is-unsafe-again.276196/)
- [Hive: Accessing memory from the script](https://www.hiveworkshop.com/threads/accessing-memory-from-the-script-its-time-of-the-revolution.279262/)
- [Hive: Memory hack](https://www.hiveworkshop.com/threads/memory-hack.289508/page-7)
- [Hive: RenderEdge Memory Hack](https://www.hiveworkshop.com/threads/renderedge-memory-hack.299926/)
- [Hive: MemHack, detecting map viruses](https://www.hiveworkshop.com/threads/memhack-how-to-detect-if-map-has-a-virus.351435/)
- [Hive: Maps that only work in 1.26](https://www.hiveworkshop.com/threads/playing-maps-that-only-works-in-1-26-in-warcraft-1-28-5.355596)
- [Hive: producer update natives list](https://www.hiveworkshop.com/threads/producer-update-natives-list.301405/)
- [XGM: MemoryHackAPI](https://xgm.guru/p/wc3/memoryhackapi)
- [BNETDocs: Warcraft III 1.27a Patch](https://bnetdocs.org/news/105/warcraft-iii-1-27a-patch)
- [BNETDocs: What's Next for WarCraft 3](https://bnetdocs.org/news/125/what-s-next-for-warcraft-3)
- [PCGamingWiki: standalone patches](https://community.pcgamingwiki.com/files/file/1180-warcraft-3-standalone-patches-all-languages-windows/)
- Warcraft Wiki: [1.27b](https://warcraft.wiki.gg/wiki/Warcraft_III/Patch_1.27b), [1.28](https://warcraft.wiki.gg/wiki/Warcraft_III/Patch_1.28), [1.28.1](https://warcraft.wiki.gg/wiki/Warcraft_III/Patch_1.28.1), [1.28.2](https://warcraft.wiki.gg/wiki/Warcraft_III/Patch_1.28.2), [1.28.5](https://warcraft.wiki.gg/wiki/Warcraft_III/Patch_1.28.5), [1.29.1](https://warcraft.wiki.gg/wiki/Warcraft_III/Patch_1.29.1)
- [WineHQ: iCCup Anti-Hack bug](https://www.winehq.org/pipermail/wine-bugs/2011-May/275877.html)
