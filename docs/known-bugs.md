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
| Ambient sound disabled | 1.27a known issue: "Disabled ambient sound while a MIDI issue is being resolved." | No later patch note mentions turning it back on, so it may still be off in 1.27b. Check in game | Official ([1.27a](https://bnetdocs.org/news/105/warcraft-iii-1-27a-patch)) |
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

## Multiplayer cheating

Nothing reliable specific to 1.27b turned up. Client-side maphacks
(revealing the map) exist for the classic patches, and private servers
ship their own anti-hack launchers ([iCCup](https://www.winehq.org/pipermail/wine-bugs/2011-May/275877.html)).
Most "1.27b hack" search results were spam or malware downloads.

## What Tome could do

1. **Map script guard:** before a map's script runs, refuse or warn on
   local/global name collisions and `I2C`/code-array patterns. That
   closes the exploit for players who download maps from others.
2. **Measure the memhack on this build** with a test map, to settle
   whether 1.27b still lets scripts write memory.
3. **Port the cheap 1.28 crash fixes:** ignore invalid command card
   coordinates. (Path traversal in `PreloadGenEnd` and every other native that
   takes a file path is already handled by the path-sanitizer patch, above.)
4. **Ambient sound:** confirm whether it's off and whether the game has
   a switch to turn it back on.
5. **Raise the hashtable and game cache limits** as 1.29.1 did, if a
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
