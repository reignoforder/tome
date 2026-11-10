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
`CreateSaveDirectory`). On 1.27b the write lands inside the **game folder**
(`PreloadGenEnd`: `<game folder>\CustomMapData\<name>`, saves:
`save\<profile>\`; Blizzard moved it to the user-data folder in 1.28.1), so a
map-controlled
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
reads allow it, as maps pass `""` for an effect with no model), a name longer
than `kMaxPathLen` (259; see "Over-long path" below; not for `Preload`), any `:` (drive
letter or NTFS alternate data stream), a leading separator (absolute,
drive-relative or UNC), any `..` component, and a reserved DOS device name
(`CON`, `PRN`, `AUX`, `NUL`, `CONIN$`, `CONOUT$`, `COM1`–`COM9`,
`LPT1`–`LPT9`, and `COM`/`LPT` with a superscript 1–3). It is portable and
unit-tested (`loader/src/core/path_guard.*`, `tests/test_path_guard.cpp`).
Refused calls are logged (`path-sanitizer: refused ... -- too long` or
`-- escapes the data folder`; the first 20 in full, then every 1000th, plus
the first of each native for each reason, the name cut to 160 characters with
control characters replaced). The `BindNative` hook is **not** reserved: the threat
is malicious maps, not mods, and a mod can bypass any API boundary anyway, so
reserving it would only cost mods a useful hook. Content isn't touched — the
escape vector is the path, not the bytes.

#### Over-long path crashes `PreloadGenEnd`

**Seen on 1.27b (Nov 2026, in game); cause read from the code.**
`PreloadGen::End` (game.dll `0x230170`, `PreloadGenEnd`'s body) copies the
name with `SStrCopy(buf, name, 260)`, adds `.pld` if it has no extension, and
builds `<game folder>\CustomMapData\<name>` with `strncpy` into 260-byte
stack buffers. When the whole path reaches 260 characters, `strncpy` leaves
them unterminated, and the code after it reads them as strings past their
end: it scans for the last `\` and writes a NUL after it (`0x91F10`), and
copies the path with an unbounded `strcpy` into the file writer
(`0x21EE10`). Whether that overwrites the stack cookie depends on the bytes
next to the buffers, so the crash is random: one call with a 4009-character
name (built by concatenation) fast-failed the game in `__report_gsfailure`
(`c0000409`, no crash report, as `int 0x29` skips exception handlers). In
two other games, 13 calls with names of 224 to 4002 characters did not. In the
e2e game that works out to 215 characters
(`Z:\tmp\tmp.XXXXXXXXXX\game\CustomMapData\` is 41, plus 4 for `.pld`). A deeper game folder lowers it. Any downloaded map
can try this: a denial of service at least.

**Fix implemented (length limits in the path sanitizer).** For
`PreloadGenEnd` the sanitizer refuses a name too long to fit after the
game's own prefix, worked out the way the game does it (`war3.exe`'s folder
through `GetFullPathNameA`, plus `CustomMapData\`):
`path_guard::max_name_after(prefix)` is 259 − 4 − the prefix's length. So
every buffer stays terminated. The tome.log line `sanitizes paths of 38 file
natives (PreloadGenEnd names up to N characters)` gives N. Every other path
native refuses names over `kMaxPathLen` (259, `MAX_PATH` less its NUL). The
game copies names into `MAX_PATH` buffers with truncating copies, and a cut
can turn a checked-safe name into an unsafe one (`..x` into `..`, `CONSOLE`
into `CON`). No legitimate name is that long, since the ANSI file APIs can't
open one. `Preload` is exempt: it keeps its string as is (no fixed buffer),
and maps use it as a data channel, because `PreloadGenEnd` writes it out word
for word (File I/O, codeless save/load), so a long one is legitimate, and so
is one with a `:` or `..` in it ("Gold: 500"). So `Preload` is never
refused. `Preload` itself opens nothing: the game opens the preload list's
files in `PreloadStart`, `PreloadEnd`, `PreloadEndEx` and `PreloadRefresh`
(`docs/reverse-engineering.md`, "The preload list"). When
`is_unsafe_any_length` (the whole string and its first 259 bytes checked
for traversal, drives, roots and devices) rejects a `Preload` argument, it
is kept (`path-sanitizer: kept Preload(...) for PreloadGenEnd`), and those
four natives are refused until `PreloadGenClear` empties the list.
Unit-tested (`tests/test_path_guard.cpp`). The e2e test map calls
`PreloadGenEnd` with a 239-character name and checks that it is refused
(`-- too long`). It also checks that a 307-character `Preload` argument is
not refused, that `Preload("e2e save: ..\Gold")` is kept and the
`PreloadEnd` after it refused, that `PreloadRefresh` runs again after
`PreloadGenClear`, and that the script carries on (9609).

#### Long string literal overruns the JASS compiler's buffer

**Seen on 1.27b (Nov 2026, in game). Fixed in Tome (jass-token-guard).** The
JASS scanner (`JassScanner::nextToken`, game.dll `0x9249D0`, a flex-style
DFA) keeps the token it reads in two heap buffers of `capacity + 1` bytes
(the text at `[this+0x98]` and each character's state at `[this+8]`; the
constructor `0x9246A0` gets the capacity from `Jass::compile`: 2100). Its
character loop compares the token's length with the capacity, but then
takes the error exit (return -2) only if `[this+0x1C]` is set, which the
function clears when it starts, so the check never fires. A `war3map.j`
with a string literal (or comment, or name) over 2100 characters writes past
both buffers while the map loads: ~4000 characters crashed the game (access
violation writing at `0x924AE4`), shorter overruns corrupt the heap
silently. A string of the same length built by concatenation at run time is
fine (4009 characters checked): it's the scanner's buffers, not JASS strings.
1.26a has the same code (`0x45FF30`).

The fix makes the check's `jne` a `jmp` to the same exit: a token that
fills the buffers ends the compile as the parser's fatal errors do. That
path crashed on its own: the parser's constructor (`0x91E9A0`) doesn't set
the parse's root node (`JassParser.root`, `+0x34`), only a finished parse
does, and after a failed one `0x913D44` calls a destructor through whatever
it holds. The patch zeroes it before each parse. With both, a map with a
3000-character literal goes back to the main menu (checked in game, 1.27b
and 1.26a), as a map whose script doesn't compile does, and `tome.log` says
why. `tests/game` runs the real scanner on such a literal: without the patch
it writes past capacity + 1, with it nothing. Maps whose literal is between
2101 characters and the length that crashed may have loaded before (with a
corrupted heap); they don't load now.

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

### Map downloads from a host

**Read from the code on 1.27b (Nov 2026; same on 1.26a).** When a player
joins a LAN or Battle.net game, the host sends the map's path as it has it
(`Maps\FrozenThrone\(2)EchoIsles.w3x`), its size and checksums. The
client checks the path with the map transfer's `MapTransfer::IsValidName`
(game.dll `0x979000`), then looks for the map at that path under the game
folder (`0x9789E0`), then by its base name anywhere under `Maps\`, and
only if neither matches downloads it into `Maps\Download\<base name>`
(`0x979090`; files up to 128 MB). The game's check refuses an absolute
path, any of `*/:><|&+^?"`, an extension other than `.w3m` and `.w3x`, and a
base name that is a DOS device (`nul`, `con`, `prn`, `aux`, `com1`–`9`,
`lpt1`–`9`), compared up to the last dot. So the download can't be written
outside `Maps\Download\`, nor get another extension. Two gaps remain:

- **`..` components pass.** A host can point the client's lookup at a
  `.w3m`/`.w3x` anywhere relative to the game folder, and with enough of
  them anywhere on its drive: the client opens and checksums the file, tells
  the host whether it matches, and plays it if it does. That leaks whether a
  file the host knows exists on the player's disk, and loads a map from
  outside `Maps\`.
- **A device name with a second dot passes** (`com1.x.w3x`, also `conin$`,
  `conout$` and the superscript ports, which the game's table lacks). Windows
  before 11 still opens the device for such a name: the lookup reads from a
  port or the console (a read that may never return), and the download
  writes the map to it.

Not reached: UNC paths and other drives (the game folder is put in front),
and the base name the download is written under.

**Fix implemented (lan-map-guard, built-in patch).** An AFTER handler on
`MapTransfer::IsValidName` also refuses what the path sanitizer's
`path_guard::is_unsafe` refuses: `..` components, device names by Windows'
own rules, `:`, rooted paths, names over 259 characters. The transfer is
then not made, as for a name the game refuses, and the client can't join
with that map. A host's own maps have plain relative paths. The hook is
reserved. `tests/game` calls the real check through the guard (a host's
real paths pass, the game's own refusals stay, the gaps above are
refused); not checked against a modified host in a LAN game.

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
and crash threads. **Checked on 1.27b in game (Nov 2026)**, one small map
per report (a melee map whose script runs the case a second into the game):

| Report | On 1.27b | Fix |
| --- | --- | --- |
| `ExecuteFunc` naming a function that takes parameters | **Crashes**: access violation reading `0x21` at game.dll+`0x91B5EA` (the VM reads arguments nobody pushed). Blizzard.j functions too (`ClearTextMessagesBJ`) | **execute-func-guard** refuses the call |
| `ExecuteFunc` with a missing name, a native's name, or a function that returns a value | Fine: the first two are ignored, the last runs | — |
| `GetLocalizedString` in a `constant` global | **Crashes** while the map loads (reading `0x2858` at game.dll+`0x91AF8B`). Not `constant` nor `GetLocalizedString` in particular: any native returning a string in any global's initial value (`I2S` too). Integer natives there are fine | **global-string-guard**: the string is null instead |
| Destroying a trigger whose threads are waiting | Fine: the waiting threads resume and finish | — |
| `MultiboardSetRowCount` changed by more than one | Fine: 1 → 12 → 1, 0 rows and columns, 30 × 9, an item kept from a removed row | — |
| `CreateQuest` during map initialization | Fine (in `main`, before `InitBlizzard`) | — |
| `SetItemPosition` | Fine on a held item, a removed one, off the map and a dead owner's | — |

The two crashes: `ExecuteFunc` (`0x1FE090`) looks the name up
(`0x1DD520` → `0x915F60`) and runs it in a new thread with no arguments.
Every native that returns a string goes through `CreateJassString`
(`0x1F7F60` → `0x1DD4B0` → `0x90C760`), which adds the text to the table of
`JassVM::Get(CGameWar3.vm_index)`; while the globals block runs, that VM is
still null, and `0x91AF80` reads `[vm + JassVM.strings]`. Both patches:
`architecture.md`, "Patches".

## Command card button positions

1.28's note ("Fixed command cards to be hidden again with special coordinates
(0, -11) and (-2147483648, -2147483648) within CommandFunc.txt; invalid
coordinates will now be ignored") and a Blizzard post from Dec 2016 ("We
intended to fix all the negative coordinates that cause maps to crash")
describe two bugs, and only one is in 1.27b. **Checked in game (Nov 2026)**,
with `Buttonpos` values in `CustomKeys.txt` of (-1, 0), (-5, -5), (9, 0),
(0, 7), (-100, 2) and (100000, 100000): no crash.

- **1.27b:** every setter of a button's position stores x only if it's at
  most 3 and y only if it's at most 2, as unsigned numbers, so negatives are
  dropped too (`0x3CA1A0`, which `CommandFunc.txt`'s `Buttonpos` reaches
  through `0x377220`; `0x36B470`; `0x373A90`, `0x373AB0`, `0x373AD0`). A bad
  value leaves the default: (-100, 2) put the Peasant at (0, 2). The same
  check drops (0, -11) and (INT_MIN, INT_MIN), the two positions maps used to
  hide a button, so such a button shows up instead of hiding. 1.28 brought
  them back.
- **1.26a:** the setters store any value (`0x2F19E0`, `0x2F1A00`,
  `0x2F4710`), and the card places a button by indexing its grid with it
  unchecked (on 1.27b `0x3AA430`: the row array at `[card+0x154]`, 16 bytes
  a row, the cells at `+8`, 4 bytes each), so a bad position reads outside
  the grid: the crash Blizzard fixed in 1.27a.

**Fixed in Tome (command-card-guard).** `CCommandCard::PlaceButton`
(`0x3AA430`; 1.26a `0x36D3E0`) skips a button whose position is outside the
grid, which is how the card leaves out a button it doesn't show. On 1.27b
the setters that `CommandFunc.txt`, `CustomKeys.txt` and abilities' buttons
go through (`0x3CA1A0`, both paths of `0x36B470`) keep the two hiding
positions, as 1.28 does; `0x373A90`-`0x373AF0`, single-coordinate setters
with the same checks, have no callers. On 1.26a every bad position is left
off the card (1.28 would put it at its default; 1.26a's setters keep no
default). Checked in game on both builds with `CustomKeys.txt` (Attack at
(0, -11), Move at (INT_MIN, INT_MIN): both hidden; Patrol at (9, 9): at
(0, 0) on 1.27b, as without the patch, left out on 1.26a). Only the local player's card changes:
nothing goes over the network. Whether a hidden button's hotkey still works
wasn't checked.

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
selects nothing, and a peasant at 98% of the width selects.

**The 2D UI: fixed in Tome too (Nov 2026, same patch and setting).** The
game converts UI units to the screen as if it were always 0.8 wide; the
patch gives those conversions the window's width, so the UI's units are
square and its root spans the window. Everything 2D keeps its shape (round
minimap icons, square command buttons, unstretched text), and what the game
anchors to the screen's edges goes to the window's edges, as in 1.29: the
minimap and the menu buttons at the left, the resource bar and the command
card at the right. The console's art is laid out again in pieces (the
minimap's end at the left edge, the portrait's arch, info panel and inventory
centered, the command card at the right edge, wall between, and slots in the
top bar each side of the clock), and the portrait, info panel, life and mana
text, clock and inventory cover move to the middle. The world view then fills
the window at its own shape. Clicks land where things are drawn (checked: a
peasant selected by clicking it, a command card button at 97% of the width),
and the cursor is drawn at the pointer: until Nov 2026 its frame (a
full-screen sprite) was stretched like the menus' art, so it was drawn a
third further from the left edge than the pointer at 16:9.
The menus fill the window as in 1.29 (Nov 2026): the border on the window's
edges, the left panels on its left and the right panels (behind the buttons)
on its right, each in proportion, with the 3D background filling the
window. Before, those panels (models drawn for 4:3) were stretched across
the window, a third too wide at 16:9, under buttons that weren't. The
loading screen and the score screen, each one piece of art with frames
placed on it, are laid out at 4:3 in the middle of the window, with the same
art stretched, blurred and darkened filling the sides: stretched itself,
the art left the frames off their places (the loading screen's minimap
preview off its border). Checked at 1280 x 720 on the main menu, Single
Player, Campaign, Options (and its Video page), Custom Game, LAN, LAN game
creation and the lobby, the loading screen, the score screen and in a game.
**Still open:**

- Untested with the enhanced-options mod's HUD scale below 100% (both move
  the console's frames).
- A minimap camera box that keeps its 4:3 size (unchanged).
- Changing the setting applies to the 2D UI from the next screen (the next
  menu screen or game), not at once: text keeps the layout it was made with.

Like 1.29, a wider window sees more of the map in multiplayer.

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
  background. Checked on GE-Proton11-11 in Xvfb at two 16:9 desktop sizes
  (one of them 864x486 scaled to 1440 lines): no light line in the desktop's
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
3. ~~**Port the 1.28 command card fix**~~ Done (command-card-guard; see
   "Command card button positions"). (Path traversal in `PreloadGenEnd` and
   every other native that takes a file path is already handled by the
   path-sanitizer patch, above.)
4. **Raise the hashtable and game cache limits** as 1.29.1 did, if a
   mod needs them (it changes game behaviour, so every player needs
   the same mod).

## Further hardening (surveyed Nov 2026)

A survey of the game's remaining attack surface for malicious maps,
downloaded files, and (for private/Battle.net servers) the network. Roughly
most-valuable first. Each needs reverse engineering on 1.27b before it can be
built; items marked done have been. The built-in guards above (memhack,
path, BLP, the JASS token and command card guards) plus the path-length
limits are what exists today.

### Parsers of untrusted bytes with no guard yet

1. **Redirect map writes to a dedicated subfolder (1.28.1's fix).** On this
   1.27b build, `PreloadGenEnd`/`InitGameCache`/`SaveGame` already write under
   `CustomMapData\` and `save\<profile>\` (confirmed in game, Nov 2026;
   `PreloadGen::End` builds `<war3.exe folder>\CustomMapData\<name>` and
   refuses `\..\`, `\..` and `..\` in it itself), so the "inside the game
   folder" worry is milder than feared, and the
   path-sanitizer's `..`/rooted/device/length checks keep a write inside those
   folders. **Done (Nov 2026) for defence in depth:** the writes also refuse
   a name a program or script would have (`path_guard::is_executable_name`:
   `.exe`, `.dll`, `.bat`, `.cmd`, `.vbs`, `.js`, `.ps1`, `.lnk`, `.reg`,
   Miles' `.mix`/`.asi`/`.flt`/`.m3d`, ..., after the trailing dots and spaces
   Windows drops, so `x.bat. ` too), and a component of only dots and spaces
   with two dots or more counts as `..`. 8.3 short names are no way around
   either: `GAME~1.DLL` has the extension; a long name whose short name
   would have one (`x.exe1` is also `X~1.EXE`, `x.mixer` `X~1.MIX`: an
   extension is cut to three characters there) is refused too; and a short
   name can only name something inside the folder the write is rooted in,
   which already keeps it out of `tome\` (unit tests, and one write in the
   e2e map). Game scripts
   (`.j`, `.ai`, `.pld`) aren't refused: `Preloader` runs a file of any name,
   and maps write `.pld` files on purpose.
2. **TGA image guard.** Browsing the Custom Game list parses each map's
   minimap and preview images before a game ever starts. A `CBLPFile`-style
   guard for TGA (and a check of the JPEG stream inside a JPEG BLP, which the
   BLP guard doesn't look into — see #3) would cover that pre-game surface.
3. **Outdated bundled libraries.** game.dll links **OpenSSL 1.0.1h** (Jun
   2014) and **libcurl 7.37.0** (2014); Storm.dll uses **zlib 1.1.4** (2002)
   for MPQ sectors; **ijl15.dll** is Intel JPEG Library 1.5 (~2001), which
   decodes JPEG BLPs. All have known CVEs. Next step: find what reaches each
   (OpenSSL/curl look tied to Battle.net/ads/news; zlib and IJL are on the
   map/asset path). Tome's import hooks could wrap `ijlRead` to validate the
   JPEG structure (markers, SOF dimensions against the BLP's, Huffman/quant
   tables) the way the BLP guard validates the BLP header.
4. **Other map data parsers:** MDX models (chunk sizes), `w3e`/`wpm`/`doo`
   terrain, object data (`w3u`/`w3t`/`w3a`, modification counts), SLK/profile
   text, and map-supplied fonts (which TrueType rasterizer the game uses, and
   how old, is unknown — no FreeType strings found). `tests/game` loads
   game.dll without running it, so it could host a direct fuzzer over these
   parsers (slow under Wine, but it would find real bugs).
5. **Replays (`.w3g`) and saves (`.w3z`).** Files players receive from
   others. Replays feed the same action parser as live network play (#6).
   (LAN map downloads: done, lan-map-guard; "Map downloads from a host".)
6. **Network actions from other players.** A modified client can send
   malformed action blocks (selection counts, slots, handle ids). Tome already
   hooks the action dispatch for events, so a validation layer could sit there;
   it's substantial work and needs research into which actions crash.

### Server-pushed code (private/Battle.net servers only)

7. **Warden and `CheckRevision`.** game.dll has a `WardenClient`: a server can
   send code modules the client loads and runs — RCE by design, and BNCS
   traffic can be tampered with in transit. Options: a hash allowlist of known
   modules, a setting to refuse unknown ones, or at least logging + a popup
   when one arrives. Only matters for players who connect to such servers.

### JASS runtime crashes (forum-reported, unverified on 1.27b)

8. ~~JASS runtime crashes~~ Checked on 1.27b (Nov 2026): `ExecuteFunc` on
   a function with parameters and a string native in a global's initial
   value crash, and are fixed (execute-func-guard, global-string-guard); the
   other reports don't crash on 1.27b ("JASS natives that crash or
   misbehave"). The JASS scanner's long string literal overrun (above) and
   the command card positions are fixed too (jass-token-guard,
   command-card-guard).
9. **Other typecasts.** Confirm the memhack guard is the only JASS→memory
   gateway left: check the hashtable `Save*Handle`/`Load*Handle` + `Convert*`
   tricks to see whether loading into a mismatched handle type is refused on
   1.27b.

### Tome's own surface

10. ~~**`TomeLogString`/`TomeLogInteger` flooding and log forgery.**~~ Done
    (Nov 2026): each map load logs at most 1000 of the map's lines, its
    strings are cut to 1000 bytes with control characters replaced, and
    `tome.log` keeps only warnings and errors past 32 MiB, nothing past
    33 MiB (`core/log_guard.*`, unit and host tests; `architecture.md`,
    "Natives"). Still open: a fuzz target over Tome-native arguments
    (`BlzFrame*`, huge strings, bad ids).

### Process-level mitigations (weigh against the stability-first rule)

11. game.dll/Storm.dll/war3.exe already have ASLR + NX (DEP on); **Mss32.dll,
    ijl15.dll and blizzard.ax have neither**, and none has SafeSEH tables or
    CFG (checked, Nov 2026). Cheap, low-risk additions Tome could set at
    startup: `HeapSetInformation(HeapEnableTerminationOnCorruption)` and
    `SetProcessMitigationPolicy` image-load policy (no remote/low-integrity
    images). Not viable: ACG (would break Tome's own hooks and mod loading),
    strict handles (may break the game). Each should be off-by-default or
    individually tested, per the stability-first rule.
    **Heap termination: done** (Nov 2026) as a setting, off by default
    (Tome > Settings > Stop on Memory Corruption, `architecture.md`,
    "Settings"); the image-load policy is still open.

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
