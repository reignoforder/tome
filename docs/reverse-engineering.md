# Reverse-engineering reference: Warcraft III 1.27b

Everything Tome knows about the game's internals, how it was found, and how to
find more. The addresses Tome actually uses live in `mappings/1.27b.json`
(format: `architecture.md`, "Mapping database"); this document explains them. For design and plans, see `research.md` and
`ROADMAP.md`.

## Target build

| File | Version | PE timestamp | SizeOfImage | SHA-256 |
|------|---------|--------------|-------------|---------|
| game.dll | 1.27.1.7085 (1.27b) | `0x584a499e` | `0x00e6e000` | `d51e5680243fc90e19c9d6074f7fac433c466d3cf5f46e2364291725574d8236` |
| war3.exe | 1.27.1.7085 | `0x584a4994` | | `0ea3434d2bdfd9609eda5fce2a68febac2137af72846de52e0008b142297ffbd` |
| Storm.dll | | `0x584a49bf` | | `36339f69727f0f3dcb4431a9567b84f56007c331cfa3b183862af43e8314eb72` |

- 32-bit x86, MSVC 2013 (msvcr120). Preferred base `0x6F000000`, but **ASLR is
  on**: under Wine game.dll loads at `0x766B0000`. Always store **RVAs** and
  add the module base at runtime.
- game.dll keeps **MSVC RTTI** (`.?AVCMainMenu@@` etc.), so classes and their
  vtables can be found by name (`tools/re/rtti.py`).
- Section layout: `.text` RVA `0x1000`, `.rdata` `0xA7C000`, `.data` `0xCD5000`.

Below, "RVA" means offset from game.dll's base; `0x6Fxxxxxx` addresses are
VAs at the preferred base (RVA = VA − `0x6F000000`).

### 1.26a

Tome also maps 1.26a (`mappings/1.26a.json`; `make test 1.26a`, `make e2e 1.26a`).
Everything below is about 1.27b; RVAs in this document are 1.27b's.

| File | Version | PE timestamp | SizeOfImage | SHA-256 |
|------|---------|--------------|-------------|---------|
| game.dll | 1.26.0.6401 (1.26a) | `0x4d83bb00` | `0x00bb5000` | `6d21bd9a0f9fbc8446f455c9e89ac994fed68174426fe608fcb9baefd4dec53c` |
| war3.exe | 1.26.0.6401 | `0x4d83baa9` | | `28173101fcbeb4cdcac0f83e0e724aa444209b479a2ac1c1784e0d4310889909` |
| Storm.dll | | `0x4d83b8d2` | | `eecc42b05a91413dee10588a654c6cca05836e7513af6c7c568a56124bf97d0a` |

- MSVC 2005 (msvcr80), no frame pointers (`[esp+n]` where 1.27b has
  `[ebp-n]`), x87 floating point where 1.27b has SSE, and no ASLR (it
  loads at `0x6F000000`; Tome still uses RVAs). `.text` `0x1000`, `.rdata`
  `0x86D000`, `.data` `0xA4F000`. RTTI as in 1.27b.
- **How it was mapped:** both builds register the same 1,291 natives by
  name (the BindNative calls in InitNatives) and have the same RTTI classes;
  pairing those, then every pair's callees and globals in the order the
  code uses them, matched about 40,000 functions. Each symbol was then
  checked: constants, strings, call order and `ret N` against 1.27b's, and
  by hand where the match was weak (the code below). The struct offsets are
  1.27b's: the same fields at the same displacements in the matched code,
  except the Direct3D device's.
- **Differences that matter:**
  - **No local-only variable constructor.** 1.26a has one constructor for
    every JASS variable kind, `JassVarDesc::ctor` (RVA `0x4641F0`, kind as
    its first stack argument, `ret 0x14`); the compiler's local declaration
    calls it with `0x29` (`0x4648DE`), and it writes `sym+0x18` as 1.27b's
    `JassLocalVarDesc::ctor` does. The local-only wrapper (`0x464260`) has
    no callers. memhack-guard hooks the shared one on 1.26a, for kind
    `0x29` only (its 1.26a variant).
  - **Direct3D device layout:** the fields Tome uses are where 1.27b has
    them (`+0x574`, `+0x580`, `+0x584`, `+0x594`, `+0xD8`), but the state
    caches moved (render states `+0x6E8`, texture stages `+0xEE8`, lights
    `+0x14F8`), and the device vtable has one slot fewer before CanDraw
    (`+0x38`, 1.27b `+0x3C`), for Direct3D and OpenGL.
  - **BLP header copy** (`CBLPFile.header_copy`, `0x78A854`): `mov
    ecx,0x127; mov edi,eax; rep movsd` (9 bytes, 1.27b's has no `mov`), and
    the magic check after it reads `[eax]`. blp-guard's 1.26a variant keeps
    the `mov edi,eax` in front of its call, and the detour preserves eax.
  - **No ambient-sound bug:** 1.26a's `CSound::InitMIDI` (`0x3DBDE0`) still
    makes the MIDI sound (CreateMIDISnd into `CSound.midi`), so
    ambient-sound's 1.26a variant skips it.
  - **Movie mode:** `Movie.cinematic_mode` is `0xACC698`; SetDisplayMode
    stores width, height and bits in a different order than 1.27b.
  - **Registers kept across calls:** the compiler knows when a leaf
    function leaves ecx alone and keeps a value there across the call:
    `0x4C5DD0` calls RCString::GetText with `this` in ecx and passes it on to
    `0x4C5CF0` without reloading it. A hook must give back the caller's
    ecx and edx, and pass its eax into the original (the hook engine does:
    `architecture.md`, hooks).
- **Mapped for mods:** COptionsMenu::SetPanel (`0x55F950`; 1.26a's tab
  cases in COptionsMenu::OnEvent call it through one-line wrappers,
  `0x55FA70`/`0x55FA80`/`0x55FA90`, where 1.27b inlines them),
  COptionsMenu::Save (`0x55F4E0`, the third call of the OK handler
  `0x55FAA0`, as in 1.27b's) and Scheduler::SetPeriod (`0x62B040`, called
  only by Options::ApplyValue's maxfps case).
- **The 2D UI's coordinates** (widescreen): the six conversions are at
  `0x4C6480` (NDCToDDC), `0x4C64B0` (NDCToDDCRect), `0x4C64E0` (DDCToNDC),
  `0x4C6510` (DDCToNDCRect), `0x4C6540` (DDCToNDCWidth) and `0x4C6580`
  (NDCToDDCWidth), the heights' twins after each Width one. x87: they
  `fmul`/`fdiv QWORD` by doubles, `DDCWidth` `0x8768B0` (0.8f widened,
  `0x3FE99999A0000000`) and 0.6 at `0x93C218`; the Rect forms load the 0.8
  once (`fld QWORD`, then `fmul st(1),st`). Found from
  `CScreenFrame::UpdateScreenRect` (`0x60A140`, the layout vtable's slot
  `+0x4` as in 1.27b), which calls NDCToDDCRect. `C44Matrix::Ortho` is
  `0x7B6250` (`ret 0x18`, called by the sprite camera's setup `0x4E7010`),
  `CScreenFrame::GetCurrent` `0x60A030` (`.data 0xACE754`, set once by
  `0x60A210`), `CInfoPanel.inventory_cover` `+0x14C` (stored before the
  `SetPointAbsolute` call at `0x371615`).
- **Not mapped (no use in Tome):** CSpriteFrame.view, CBattleNetGateways::LoadRegistry,
  CSlider::SetValue, CameraPresets (not a table in 1.26a), CrashHandler::*,
  CGxDeviceD3d::WndProc.

## How Tome gets in

`Mss32.dll` (Miles Sound System) `LoadLibrary`s every `*.mix` file in the game
folder as a sound provider; RenderEdge uses the same route. The `.mix` needs no
exports. Observed under Wine:

- `DllMain` runs on the **game's main thread**, and **game.dll is already
  loaded and initialized**.
- The startup `FrameDef.toc` load and the first InitNatives call have
  **already happened**. Map loads come later.
- Tome pins itself (`GET_MODULE_HANDLE_EX_FLAG_PIN`) so Miles can't unload it.

## Calling conventions seen

| Convention | Meaning | GCC/MinGW spelling |
|------------|---------|--------------------|
| `__fastcall` | args 1–2 in ecx, edx; rest on stack; callee pops | `__fastcall` |
| `__thiscall` | `this` in ecx; args on stack; callee pops | `__thiscall` (GCC warns "used for non-class method" but **does** apply it; verified by reading the generated assembly) |
| `__cdecl` | all args on stack; caller pops | default |
| `__stdcall` | all args on stack; callee pops | `WINAPI` (Storm) |

`ret N` at the end of a function tells you how many stack-argument bytes it pops.

## JASS natives

| What | RVA | Signature / notes |
|------|-----|-------------------|
| BindNative | `0x90D120` | `void __fastcall(void* fn, const char* name, const char* sig)`, `ret 4`. Parses the signature. |
| InitNatives (common.j) | `0x207490` | `void()`. Calls `0x90CAC0` first (probably a table reset), then ~1,170 BindNative calls. |
| InitNatives call sites | `0x04542B`, `0x1DFD64`, `0x1E37A2` | `E8 rel32`. The first is the startup path; the other two are map-load paths (preceded by `call 0x207480`). Tome redirects all three. Quitting a game to the score screen calls it once more. |
| Second native table | `0x713450` | Registers ~1,290 of the same names, all pointing to the stub `0x727800`. **Not needed** for natives declared in map scripts (verified). Purpose unknown. |
| AI natives | `0x9BDEC0` | ~124 natives, probably common.ai. |

**Native ABI (verified in game):** `__cdecl`, one 32-bit stack argument per
parameter. integer/boolean/handle are passed as values. **real is passed as
`float*`**, and **a real result is returned as raw float bits in eax** (not
x87). **A string argument is an `RCString*`:** `RCString::GetText`
(`0x06E8D0`, `__thiscall`, 544 callers, e.g. the Preload native at
`0x20FA40`) returns `[[str+8]+0x1C]`, the text of its `CStringRep`, or NULL.

**A string result is a string-table id**, not a pointer. Every built-in
native that returns a string (`I2S` `0x204CA0`, `SubString` `0x216400`,
`GetPlayerName` `0x201790`) ends with `CreateJassString` (`0x1F7F60`,
`uint32_t __fastcall(const char*)`, 0 for NULL): `CGameWar3::CreateString`
(`0x1DD4B0`, thiscall) with the instance at `.data 0xD687A8`, which passes
`[game+8]` (the script's VM index) to `0x90C760`. That fetches the VM
(`0x90AB10`: `[[instance table 5]+0x90][index]`) and calls `0x91AF80`, which
looks the text up in the VM's string hash (`+0x287C`) and returns the
existing entry's id (`[entry+0x18]`, with a reference added) or appends a
16-byte entry (an RCString plus a reference count of 1) to the table at
`+0x2874`. It doesn't check the instance for NULL.

**`code`** is an index into the VM's code table: `0x916880` (thiscall on
the VM, the function's address) looks the address up in the hash at
`+0x2890` and returns its index, or appends it to the array at `+0x2888`
(capacity, count, data) and returns the new index. Each map's VM starts
from scratch, so the same small numbers come back with every map. The natives that run
one: `ForGroup` (`0x1FE690`) hands it to the group (`0x22FF30`); `Condition`
(`0x1F9000`) looks it up in a hash table at `+0x1F4` of `0x1E0D70`'s object
(`0x284BF0`) and creates the conditionfunc only once per value, so
`Condition(f) == Condition(f)`; `ExecuteFunc` (`0x1FE090`) finds a function
by name (`0x1DD520` → `0x90C7A0` → VM `0x915F60`) and runs it on a new
thread object (`0x298060`). Tome runs code through the natives
`CreateTrigger` `0x1FC7E0`, `TriggerAddCondition` `0x216CE0`,
`TriggerEvaluate` `0x216D70` (→ `0x297520` on the trigger) and
`DestroyTrigger` `0x1FD210` (vtable `+0x5C`), plus `Condition`, and for
code that waits `TriggerAddAction` `0x216CA0` and `TriggerExecute`
`0x216D90` (→ `0x2979A0`); natives are plain cdecl, so they can be called
from C with handle ids.

Signature letters: `I` integer, `R` real, `B` boolean, `S` string, `C` code,
`V` nothing, `H<type>;` handle (e.g. `(Hunit;R)V`).

**Wrapping a built-in native without its address (path-sanitizer patch).**
`InitNatives` binds every built-in through `BindNative(fn, name, signature)`
(mapped) on each map load, pushing the native's address as an immediate `fn`.
Hooking `BindNative` and rewriting the `fn` argument for a chosen `name` makes
the engine bind a wrapper of ours instead — so the engine's real native is
captured (and called) at runtime and never has to be located in the binary.
`loader/src/patches/path_sanitizer.cpp` uses this to sanitize the path
arguments of every native that takes a file path (all 38 are listed in
`docs/known-bugs.md`; the writes among them are rooted in the game folder on
1.27b): the wrapper reads each path (string arg = `RCString*`, Native ABI
above) and skips the call for a traversing/absolute name. Those natives' addresses are therefore deliberately **not** mapped;
only `BindNative` and `RCString::GetText` are needed.

The same wrapper also limits the path length (docs/known-bugs.md, "Over-long
path"). `PreloadGenEnd`'s body, `PreloadGen::End` (`0x230170`, thiscall,
`ret 4`, generator from `0x231DF0`), copies the name with Storm's `SStrCopy`
(ordinal 501) into a 260-byte buffer and adds `.pld` with `SStrPack` (503)
if it has no extension. It builds the folder from `GetModuleFileNameA(NULL)`
cut after its last separator (`0x91C20`), runs it through `GetFullPathNameA`,
and adds `CustomMapData\`. It refuses `\..\`, `\..` and `..\` in the
result, creates the folder and writes through a writer object
(`0x21EE10`, an unbounded `strcpy` of the path). The copies into its
260-byte stack buffers are `strncpy`, so a path of 260 characters or more
leaves them unterminated. `0x91F10` (cut after the last `\` or `/`) and the
writer then run past their end. A 4009-character name once ended in
`__report_gsfailure` (`0x78F443`, `int 0x29` fast fail). The sanitizer
therefore limits a `PreloadGenEnd` name to what fits after that prefix
(`path_guard::max_name_after`). Other path natives get `kMaxPathLen` (259),
and `Preload` has no limit (its native at `0x20FA40` keeps the string in a
heap string via `0x2407C0`).

**The preload list (1.27b).** `0x231DF0` returns the process's one preload
generator (global `0xD68A24`, kept across maps). `Preload` (`0x20FA40`)
only adds an entry to its list (`0x2407C0`, ret 4: the name at entry+8,
flags for a model, `.mdx`/`.mdl`, at +0x14 and a texture, `.blp`/`.tga`, at
+0x18); it opens nothing. The files are opened by the natives that work the
list: `PreloadStart` (`0x20FB90`) and `PreloadEnd` (`0x20FA70`) through
`0x249B80`, and `PreloadEnd`, `PreloadEndEx` (`0x20FAC0`) and
`PreloadRefresh` (`0x20FB80`) through `0x2441B0`, which loads each entry as
a model (`0x17DF00`), a texture (`0x1B55A0`) or another file (`0x65DA0`)
and removes it. `PreloadGenClear` (`0x20FB10`) frees the whole list
(`0x229A30`, by its `jmp` thunk `0x229B50`), as does `0x2441B0` past its
time budget. `PreloadGenEnd` writes the list out. So the path sanitizer lets
`Preload` record any string (maps use it as data) and, while the list holds
one that isn't a safe path, refuses the four natives that open it until
`PreloadGenClear`.

### JASS compiler (Jass2), for the memhack guard

The map script (`war3map.j`, textual JASS) is compiled at runtime by the
Jass2 compiler, source `scanner.cpp` (lexer) and `parser.cpp` (type name
strings at `0xBD77AC`/`0xBD77A0`), engine version `1.0.3` (`0xBD78DC`). The
compiler and VM occupy roughly `0x90xxxx`–`0x92xxxx`: the VM code table
(`0x916880`), string table (`0x91AF80`), function lookup (`0x915F60`), and
an AST source writer (`0x9259A0` onward, which re-emits keywords like
`endfunction`/`globals` through a writer helper `0x925690`).

**Static type checker confirmed present on 1.27b.** The compiler reports, among
others (error table of `{char* msg; int code; int flags}` 12-byte records at
`.data 0xCE4B30`, searched by code — its base isn't referenced by a plain
immediate, so find the reporter by the `printf`-style formatter, not by xref):
`type mismatch in assignment` (code `0x29`), `expected a handle type` (`0x18`),
`invalid argument type '%s' in call` (`0x24`), `attempt to set a constant
variable` (`0x4`), `unregistered native function` (`0x2F`). So 1.27b does
type-check assignments and calls; the memhack works by *defeating* this
check through local/global name shadowing (see `docs/known-bugs.md`), not by
its absence.

**Front-end pipeline (traced):**

| What | RVA | Notes |
|------|-----|-------|
| Compile driver (`Jass::compile`) | `0x9146F0` | `ret 8`, `__fastcall` (ecx context, edx callback, two stack args). Builds the parser object (`0x9122E0`) and the scanner (`0x9246A0`, `0x834` capacity) in its own stack frame, runs the parse (`0x913C30`), tears both down (scanner dtor `0x9245F0`). Four callers (`0x20D36F`, `0x20FEB2`, `0x719A9C`, `0x9CBCAB`). The scanner ctor's only other caller, `0x9120E0`, is unreferenced, so all compilation runs inside this function. |
| Parse entry | `0x913C30` | `__thiscall(src, len*)`. Sets up the parse context; `0x913F40` just flattens the source rope into one buffer (not the parser proper). |
| Scanner ctor / dtor | `0x9246A0` / `0x9245F0` | Holds the symbol table(s). |
| Symbol-table hashtable | ctor `0x9143D0`, dtor `0x9124D0` | count `+0x18`, capacity `+0x18`?/buckets `+0x1C`; inner container ctor `0x9152D0`. Instantiated in the scanner at `+0x4` and `+0x2C`, and at scope sites `0x912508/0x912786/0x912790/0x912B22/0x913F05/0x9222CB/0x922E1E/0x922FA1`. A single script-wide, name-keyed symbol table — the structure that makes local-shadows-global type confusion possible. |

**Symbol table and resolver (traced):**

| What | RVA | Notes |
|------|-----|-------|
| Hashtable insert-or-get | `0x914640` | `ret 0xC`, `__thiscall(key, a, b)`. Masks key by `[tab+0x24]`, finds the bucket, allocates an entry through the vtable method `[[tab]+4]` (entries are polymorphic). Caller then sets `[entry]=symbol`, `[entry+0x14]=key`. |
| Hashtable find-by-key | `0x9247F0` | `__thiscall(key)` → entry or 0. |
| Bucket-chain maintenance | `0x914900` | Grows a bucket when its chain passes length `0xD` (via `0x914090`); not the key compare. |
| Identifier hash | `0x908500` | `(text, scanner[+0x28])` → interned key. |
| Scanner intern + classify | ~`0x925380` | Hashes an identifier, looks it up with `0x9247F0` in the scanner's **two** symbol tables at `+0x58` then `+0x30` (insert into one chosen by the mode flag `[scanner+0x2C]`), then classifies the symbol into a token type (`0x130`–`0x133`) via the kind tests below. |
| Scope add-symbol | `0x921FA0` | `__thiscall(key)`, one caller (`0x924068`). Inserts into a scope's table `+0x4` or `+0x2C` by its mode flag `[scope]`. |
| Symbol kind tests | `0x91CC30` / `0x91CC50` / `0x91CCA0` | Read `[sym+0x18]` (the symbol's *descriptor*) then `[descriptor+0x4]` (**the kind enum**): e.g. `0x91CCA0` true for kinds `0xC/0x29/0x2A/0x2B`, `0x91CC30` for `6/9`, `0x91CC50` for `0xA`. |

**Symbol struct:** `+0x14` = interned name/key, `+0x18` = descriptor pointer;
`descriptor+0x4` = kind enum (keyword / type / function / variable …). A
variable's declared type lives in/off the descriptor. The scanner interns
each name once and **reuses the same symbol object for every occurrence** —
so declaring a `local` whose name matches a `global` resolves to the same
interned symbol (`0x925380` returns the existing entry rather than inserting
a new one). That shared-symbol design is the structural root of the
local-shadows-global type confusion; the write that repoints the symbol is
the local-variable descriptor ctor's `0x91DD5E` (next section).

**`+0xB8` is NOT the variable type (corrected by a dynamic run).** GetType
`0x91B340` / SetType `0x91B330` access `[this+0xB8]` but are **dead** (zero
callers/refs). `0x9121D0` is *not* the general variable-reference ctor: a
dev probe mod (a hook on it, since removed) logged it across a full melee-map compile and it
fired for only **3 distinct `this` objects** over 204 references, always with
node `+0x4=0x27` and node `+0x10=5`, reading a *small, monotonically
increasing* value out of a singleton's `+0xB8` (a running string-id counter,
not a per-variable type). So the earlier "`+0xB8` = type-object" reading
conflated two different objects' `+0xB8`; the live per-variable type lives
elsewhere (most likely the `+0x10` type-id, set from the type nonterminal, or
a descriptor field). Numeric coercion (`0x91D940`+) does run on node `+0x10`
type-ids (values 2..7).

### The memhack gateway: mechanism (CONFIRMED dynamically on 1.27b)

Confirmed in game with a dev probe mod (since removed) that hooked the
scanner's intern+classify `0x9253E0` (`ret 0`, `__thiscall(scanner)`; token
text at `[scanner+0x98]`, resolved symbol at `[scanner+0x24]`) and dumped the
symbol it resolves, against a map whose `war3map.j`
declares `unit zzqG` (global) and, in a function, `local integer zzqG`.

- Each name is interned to **one shared symbol** `E`. A **variable's declared
  type lives on a descriptor** at `E+0x18`; `descriptor+0x4` = kind
  (`0x2A`=global var, `0x29`=local var), **`descriptor+0x10` = JASS type-id**
  (observed: `unit`=7, `integer`=4, `real`=5; `descriptor+0x8` is a
  global/local flag: 2/6).
- Declaring a variable **overwrites** `E+0x18` with a freshly built
  descriptor. Local-var descriptor ctor `0x91DCF0` (`ret 0x10`): sets
  `desc+0x4=0x29`, copies the declared type-id into `desc+0x10`, and at
  **`0x91DD5E` (`89 72 18  mov [edx+0x18], esi`)** writes `E+0x18 = this new
  descriptor`. Global-var ctor is `0x91D710` (kind `0x2A`, same shape). The
  local-decl handler is `0x922EE8`/`0x922F2A` (two variants, calling
  `0x91DCF0` at `0x922EF9`/`0x922F3A`); it passes `E` but **does not save the
  old `E+0x18`**.
- **No scope restore.** A reference to `zzqG` in a *different function compiled
  after* the shadowing one still resolves `E+0x18` to the **local integer**
  descriptor (type-id 4), not the global `unit` (type-id 7). So a `local` that
  shadows a global of another type **permanently repoints the global's type**
  for the rest of the compile — the handle↔integer typecast primitive (memhack
  gateway).

**Precise detection rule (for the guard):** a `local` declaration whose name
equals a `global` variable of a *different* type. Low false-positive; legit
maps don't do this. The engine-patch sites, if a binary fix is wanted, are the
swap `0x91DD5E` or the local-decl handler `0x922EE8`/`0x922F2A`; a correct
in-compiler fix must add scope restoration of `E+0x18` (invasive, since the
handler saves nothing). A hook on `0x91DCF0` can instead neutralize the
exploit by forcing a cross-type shadow's `desc+0x10` to the global's type-id.
**Shipped** as the scope-restore fix: the built-in `memhack-guard` patch
(`loader/src/patches/memhack_guard.cpp`) hooks:

- `0x91DCF0` (BEFORE): saves the symbol and its current descriptor unless
  there is none or it's another local's (kind `0x29`). Every other kind is
  saved, not just `0x2A`: the compiler's variable test `0x91CCA0` also
  accepts `0xC` and `0x2B`. The list has no cap, or a function could declare
  that many harmless shadows before the one that matters.
- `JassScanner::nextToken` `0x9249D0` (AFTER, the `endfunction` token
  `0x115`): writes the saved descriptors back, newest first. It stores each
  character of a token at `[[this+8]+n]` with a length check that never
  fires: a string literal over 2100 characters writes past that heap buffer
  (access violation at `0x924AE4` with ~4000, seen in game; next section).
- `Jass::compile` `0x9146F0` (BEFORE and AFTER): drops the list. A script
  that stops inside a function never reaches its `endfunction`, and the next
  compile's first one would write into the old compile's freed symbols. This
  is a safety net. In game, the parser recovers from a syntax error or a
  missing `endfunction` and still reaches a later `endfunction`, and a script
  that ends inside a function crashes 1.27b itself, with or without Tome.

Both functions are reserved (mods can't hook them), so their mappings have
a `size`, the length to the `int3` padding after them: `Jass::compile`
`0x102` (1.26a `0x458900`, `0x10A`) and `JassScanner::nextToken` `0x5C8`
(1.26a `0x45FF30`, `0x5F4`), which takes in its `switch`'s jump table
(`jmp [ebx*4+0x924EC0]`, 1.26a `[eax*4+0x46044C]`) at its end. A linear
disassembly that stops at the first `ret` past every branch target ends
`nextToken` in the middle: the table's cases end in `ret`s too.

Restoring when the scanner hands out `endfunction` isn't too early: the
e2e map's functions end with `return <local>` and `set <local> = 7` right
before `endfunction`, and both compile and run against the local
(`tools/e2e-test.sh`, "memhack guard"). A map whose later function assigns
an `integer` to the shadowed `unit` global fails to compile and doesn't load:
the typecast is closed. With `-loadfile`, the map script is compiled twice
before the loading screen (the guard logs each restore twice). The map scan in `events.cpp`
(`jass::scan_shadow_exploit`) logs the same pattern at map load, including
locals that shadow a `common.j` or `Blizzard.j` global. See
`docs/known-bugs.md` and `docs/architecture.md`, "Patches".

### The scanner's token buffers and a stopped parse

The scanner (`JassScanner`, constructor `0x9246A0`, `thiscall(files, a1,
a2, a3, capacity)`, `ret 0x14`; 1.26a `0x460570`) is a flex-style DFA.
`capacity` (`JassScanner.capacity`, `+0xC`; `Jass::compile` passes `0x834`,
2100) sizes two heap buffers of `capacity + 1` bytes: `[this+0x98]` the
text read so far (the current token at its start, NUL-terminated after
`nextToken`, its length at `+0xC4`) and `[this+8]` the DFA state after each
character. It reads characters through its vtable (`+0` the next character
or -1, `+4` wrap at the end: nonzero, no more input), so a test can make one
on a buffer of its own and call `nextToken` (`tests/game`,
`game_jass_token_guard_stops_long_tokens`).

`nextToken`'s character loop (`0x924A68`) checks `esi`, the token's length,
against the capacity: `cmp esi, [edi+0xC]; jl next; cmp [edi+0x1C], 0; jne
0x924BF1`, and `0x924BF1` returns -2. But `nextToken` sets `[edi+0x1C]` to 0
when it starts (`0x9249F3`), so the loop goes on and writes each further
character at `[[edi+0x98]+esi]` (`0x924AA5`) and its state at
`[[edi+8]+esi+1]` (`0x924AE4`): a heap overrun from `war3map.j`. 1.26a has
the same code (`0x45FFD0`, exit `0x460166`). jass-token-guard makes the `jne`
(`JassScanner::nextToken.overflow_check`, `0x924A6D`; 1.26a `0x45FFD5`) a
`jmp`.

The parser (`JassParser::parse`, `0x922A50`, `thiscall(context)`, `ret 4`;
1.26a `0x464460`) takes -2 from the scanner (`0x922B96`, `0x922C17`) as
fatal and returns 1 (`0x924250`). Its caller `0x913C30` builds the parser
on its stack (constructor `0x91E9A0`, which sets `+0x38` but not `+0x34`)
and after a failed parse destroys the root node at `+0x34`
(`JassParser.root`, set only when the whole script parsed, `0x923F3B`)
if it isn't null: `[[root]](root, 0)` at `0x913D52`. After a stopped parse
that is stack garbage, and the game crashed (a breakpoint in `0xCC` fill,
seen in game). jass-token-guard zeroes it in a BEFORE handler on the parser;
the failed compile then sends a `-loadfile` map back to the main menu (the
compile runs three times on the way, each logged).

`0x9121D0` is a specialized ident-node ctor, *not* the general var-ref: it
reads a string-id counter off a singleton's `+0xB8`, which earlier misled a
static reading of `+0xB8` as "the type". GetType `0x91B340`/SetType
`0x91B330` (`[this+0xB8]`) are real but dead.

## Frames (UI)

| What | RVA | Signature / notes |
|------|-----|-------------------|
| LoadFrameDefToc | `0x0E48F0` | `__fastcall(const char* toc, void* status)`, plain `ret`. **Clears** the definition tables (`0x0BC2F0`/`0x0BBF90` on `0xD47744`, `0xD4776C`, `0xD47794`), then calls LoadTocFiles. Startup call at `0x045426` with `"UI\FrameDef\FrameDef.toc"`. |
| LoadTocFiles | `0x0BA580` | `int __fastcall(toc, tableA=0xD47744, tableB=0xD47794, status=0xCE3A4C)`, `ret 8`. Loads **without** clearing. Existing names are **not** replaced by later definitions. |
| CreateFrame | `0x0E4740` | `CFrame* __fastcall(const char* template, CFrame* parent, int unused, int priority, int context)`, `ret 0xC`. The parent owns the child. Returns 0 for an unknown definition. The first stack argument isn't read in 1.27b; the second goes to the constructor (likely 1.31's priority); the third goes with the definition's name to `0x0F50E0` (set name), so the frame **and its children** register under their FDF names with that context (verified in game). |
| GetFrameByName | `0x0F2CA0` | `CFrame* __fastcall(const char* name, int context=0)`. Frames are hashed by (name, context). Finds glue frames and some in-game ones (`MinimapImage`, `CinematicScenePanel`); most of the in-game console is simple frames, which this doesn't find. |
| CTextFrame::SetText | `0x0FDE90` | `void __thiscall(const char* text)`. Copies with Storm (SStrLen/SStrDup); text at `+0x1E8`. `0x0FDE40` is SetText(int). |
| CLayoutFrame::SetPoint | `0x111600` | `void __thiscall(int point, CLayoutFrame* rel, int relPoint, float x, float y, int update)`, `ret 0x18`, 347 callers. **Call on `frame + 0xB4`** and pass `other + 0xB4`. Anchors are stored at `[layout + point*4 + 8]`; setting a point replaces that point's anchor. |
| Frame points | | 1.31's FRAMEPOINT numbering: TOPLEFT 0, TOP 1, TOPRIGHT 2, LEFT 3, CENTER 4, RIGHT 5, BOTTOMLEFT 6, BOTTOM 7, BOTTOMRIGHT 8. |
| Show / Hide | vtable `+0xD4` / `+0xD0` | Both `__thiscall()`. **Hide sets bit 0** of `[frame+0xB0]` (it is a *hidden* flag) and marks children with bit 1 ("hidden by parent"); Show clears it. **Show also shows every child**, so hide sub-frames after showing a parent. Visible frames read `0x2C`, hidden ones `0x2D`. |
| Destroy | vtable `+0xD8` | `__thiscall(int flags)`; CreateFrame's helper calls it with 1 when a frame fails to initialize, and CGlueMgr::SetScreen and CGameWar3::Reset destroy screens and the in-game UI with it: the deleting destructor. Not called by Tome yet. |
| Setup | vtable `+0xDC` | `int __thiscall(int, int)`. CreateFrame calls it on every new frame after construction (failure → `+0xD8(1)`). CMainMenu's override binds its buttons; CGameUI has none. |
| CLayoutFrame::SetWidth | `0x1116C0` | `void __thiscall(float width)` on `frame+0xB4`: stores `+0x58`, then tail-calls the layout update `0x111390` (`ret 4`). SetHeight is `0x111520` (`+0x5C`). GetWidth/GetHeight are the layout vtable's `+0x18`/`+0x1C` (results on the x87 stack). |
| `0x0F07B0` | | `__thiscall(byte value, int force)`. Writes a byte at `+0xAC` on the frame and recursively on its children, then calls vtable `+0xA4`. Likely alpha/fade; not needed so far. |
| Layout base | `+0xB4` | The CLayoutFrame subobject inside every frame (secondary RTTI vtables have this-offset `0xB4`). |

**Checking a frame's class:** read the MSVC RTTI behind its vtable (`vtable[-1]` →
CompleteObjectLocator → ClassHierarchyDescriptor → base class names);
`loader/src/core/rtti.cpp` does it at runtime. A `TEXT` frame is a
`CTextFrame`, a `SPRITE` a `CSpriteFrame`, and every button or check box
derives from `CControl` (vtable slot `+0x10C`, which plain `CFrame`s
don't have).

Frame classes in RTTI include `CFrame`, `CLayoutFrame`, `CTextFrame`,
`CBackdropFrame`, `CSpriteFrame`, `CModelFrame`, `CButtonFrame`,
`CTextButtonFrame`, `CCheckBox`, `CListButton`, `CSimpleFrame`,
`CSimpleButton`, `CSimpleTexture`, `CScreenFrame`. Their vtables:

| Class | Primary vtable (VA) |
|-------|---------------------|
| CTextFrame | `0x6FA8B9C0` (secondary `0x6FA8BAD4`, +0xB4) |
| CCheckBox | `0x6FA8C1F4` |
| CSpriteFrame | `0x6FA8B214` (secondary `0x6FA8B304`) |
| CHighlightFrame | `0x6FA8B744` (1.26a `0x6F96F974`; a sprite frame: highlights, and the layer that dims a glue screen behind a dialog) |
| CMainMenu | `0x6FAB1894` (secondary `0x6FAB197C`, +0xB4) |
| COptionsMenu | `0x6FAA7170` |
| CSinglePlayerMenu | `0x6FAB19E8` |

### FDF keywords

The FDF lexer maps keywords to ids through a table of `{const char*, id}`
pairs in `.rdata` (around `0x6FA89594`): `SetAllPoints` `0x1B8`, `SetPoint`
`0x1B9`, `BOTTOM` `0x1BA`, `BOTTOMLEFT` `0x1BB`, `BOTTOMRIGHT` `0x1BC`,
`CENTER` `0x1BD`, `TOP` `0x1BE`, `TOPRIGHT` `0x1BF`, `TOPLEFT` `0x1C0`,
`LEFT` `0x1C1`, `RIGHT` `0x1C2`. There is no FDF keyword for sprite
animations; they're set from code.

## Glue menus (main menu and other screens)

| What | RVA | Notes |
|------|-----|-------|
| CMainMenu vtable | `0xAB1894` | |
| slot 3: OnEvent | `0x2EFB70` | `int __thiscall(CEvent*)`, `ret 4`. Command id at `event+8`; jump table over ids `0`–`0x21`; anything higher returns 1 and does nothing. |
| slot 55: Setup | `0x2DBE90` | `int __thiscall(int, int)`, `ret 8`. Binds the buttons, then shows the realm-select frame etc. |
| Glue BindControl | `0x2E1460` | `CFrame* __thiscall(menu, const char* name, int command, int enable)`, `ret 0xC`. GetFrameByName (context 0) + `0x112D80` (registers event `0x40090064` with the menu as listener) + vtable `+0x10C(enable)` + adds to the menu's control list at `+0x210`. Crashes if the frame doesn't exist, so check first. |
| Main menu command ids | | SinglePlayer 0, (BattleNet) 1, (LAN) 3, Options 4, Credits 5, Exit `0xC`. Tome uses `0x7100`+ for its menu and `0x7200`+ for mods' buttons. |
| `MainMenuFrame` | | GetFrameByName("MainMenuFrame") **is** the CMainMenu object (same address, verified in game). |

### Glue screens

The glue manager (`CGlueMgr`, singleton from `CGlueMgr::Get` `0x2DE540`)
owns one screen at a time. A table at `.data 0xCE50B8` lists the screens as
`{FDF frame name, factory, id}`: Title 0, Loading 1, MainMenuFrame 2,
OptionsMenu 3, SinglePlayerMenu 4, Skirmish 5 (custom game),
CustomCampaignMenu 6, CampaignMenu 7, LocalMultiplayerJoin 8,
LocalMultiplayerCreate 9, LocalMultiplayerLoad 10, GameChatroom 11,
ScoreScreenFrame 12, ViewReplayScreen 13, LoadSavedGameScreen 14,
BattleNetMainFrame 15, BattleNetChatroom 16; 17 means none. Later entries
(TeamSetup, MapListBox, ...) are frame types without a screen id.

| What | RVA | Notes |
|------|-----|-------|
| CGlueMgr::SetScreen | `0x303C90` | `void __fastcall(int screen, int force)`, plain `ret`, 20 callers. Destroys the old screen (`+0xD8(1)`), sets `[mgr+0x180]` = id, creates the screen with CreateFrame(name, mgr) (which runs its Setup) and stores it at `[mgr+0x184]`. |
| CGlueMgr::EnterGame | `0x30BE20` | `void __fastcall(int)`. Loading has finished: destroys the glue screen (id 17) and switches to the game. CGameUI exists by then. 6 callers. |
| `[mgr+0x180]` / `[mgr+0x184]` | | current screen id / its frame (CMainMenu, CSinglePlayerMenu `0xAB19E8`, the custom game screen `0xAB1F70`, CLoading `0xAB12E0`, CScoreScreen `0xAB2648`, ...). |
| UITimer::Fire | `0x8B5D40` | `int __fastcall(const float* elapsed, void* timer)`, plain `ret`, returns 1. Mapped. A UI timer's callback (scheduled by `0x8B5C60`, `__thiscall(timer, float seconds, int event, CObserver* target, int repeat)`, `ret 0x10`): sets the shared event at `.data 0xCEF108` (id at `+8` from `timer+0x18`, the time at `+0x10`) and calls `target->OnEvent` (vtable `+0xC`); the target is at `timer+0x8`. Glue screens animate on these (one at `+0x234`, others) and schedule event `0x1A` once they have appeared. Developer builds hook it for "Screen ready" and the input scripts' `settle` (events.cpp). |

There is no common glue menu class in RTTI: every screen derives from
CFrame directly.

### Options screen

`COptionsMenu` (vtable `0xAA7170`) is the `OptionsMenu` frame of
`Glue\OptionsMenu.fdf`: tabs Gameplay, Video and Sound on the right, one
panel of controls (`GameplayPanel`, `VideoPanel`, `SoundPanel`) at a time
on the left, over a backdrop sprite that morphs open and closed. The
separate `enhanced-options` mod adds a fourth tab through SetPanel.

| What | RVA | Notes |
|------|-----|-------|
| COptionsMenu::OnEvent | `0x2B4470` | Slot 3, `int __thiscall(CEvent*)`, `ret 4`. Jump table over command ids `0`–`0x18`: Gameplay 0, Video 1, Sound 2 (each enables the control at `+0x29C`, then calls SetPanel through a small thunk: `0x2B4910`, `0x2B7040`, `0x2B6490`); `0xF` hides the old panel (`+0x1E8`) after its fade and runs the backdrop step `0x2AA3A0(0)`; `0x11` fades the new panel (`+0x1E4`) in; `0x12` comes from the opening morph. |
| COptionsMenu::SetPanel | `0x2BA780` | `void __thiscall(int panel)`, `ret 4`. Returns at once if `+0x16C` is already `panel`. Stores it, fades the current panel frame out (event `0xF` when done), moves it to `+0x1E8`, and picks the new one into `+0x1E4`: `+0x1FC` (0), `+0x234` (1), `+0x26C` (2), none (3). With no old panel it plays the opening morph (`0x2AA3A0(1)`). Only the three tab thunks call it. |
| `+0x16C` | | the shown panel: 0 gameplay, 1 video, 2 sound, 3 none (the constructor `0x2A6A00` sets 3). |
| `+0x1E4` | | the panel frame being shown; NULL for 3. Event `0x11` uses it unchecked, so SetPanel(3) from another panel crashes when the backdrop has reopened. Putting another CFrame there before `0x11` makes the game show, fade, and later fade out and hide it like its own panel (verified in game). |
| `0x2AA3A0` | | `void __thiscall(int open)`, `ret 4`: plays the backdrop sprite's (`+0x184`) sequence {options, morph} to open (then {options, alternate, stand}), or the reverse to close. |
| COptionsMenu::Save | `0x2B8E80` | `void __thiscall()`, plain `ret`. OK (command 3, `0x2B5EE0`: disables the controls, applies the video choices with `0x2AA490`, Save, then leaves with `0x2B92D0(2)`). Reads every control into Options::SetInt and ends with Options::Save(`0x50`). The ids it writes are the options the screen shows. |

**The options store** (`Options::Get`, `.data 0xD3C5F0`) keeps strings in a
hash table at `+0x1C` (mask `+0x24`) and integers in one at `+0x44` (mask
`+0x4C`), keyed by option id (0 to `0x4E`; the table at `.rdata 0xA7EFE8`
has 16 bytes per id: name, category (index into the names at `0xA7EFCC`:
Video, Sound, Gameplay, ...), type (0 integer, 1 string), default text).

| What | RVA | Notes |
|------|-----|-------|
| Options::GetInt | `0x040FA0` | `bool __thiscall(int id, int* value)`, `ret 8`. A missing value is the default text through atoi, then stored. |
| Options::GetString | `0x040F30` | `bool __thiscall(int id, char* buffer, int size)`, `ret 0xC`. |
| Options::SetInt | `0x042B40` | `bool __thiscall(int id, int value)`, `ret 8`. Store only. |
| Options::Save | `0x042620` | `bool __thiscall(int id)`, `ret 4`. Writes one value to the registry, or all for `0x50`. |
| Options::ApplyValue | `0x0403E0` | `bool __thiscall(int id, int value)`, `ret 8`. The switch Options::Apply (stored value) ends in: cases for gamma, the video details, sound, scrolling, tooltips, health bars, maxfps, ally and creep filters, autosaveReplay. Others are read where they're used. |

`adapter` (3) is Direct3D's adapter index: start-up's mode picker takes
only that adapter's modes, and Save writes the chosen mode's adapter to it.

Options the Options screen never writes: `lockfb` (`0xD`), the
`cinematic*` ones (`0x10`–`0x14`), `maxfps` (`0x16`), `nomidi` and
`softwaremidi` (`0x22`, `0x23`), `formations` (`0x2C`, the in-game
formation button's state), `herobar` (`0x2E`, read when the hero bar is
built: 0 hides the hero icons), `inputsprocket` (`0x30`) and
`multiboardon` (`0x3C`, read when a multiboard is created: 0 minimizes it).

The hero bar (`CGameUI.hero_bar`, a CHeroBar, which is a frame grid of 7
CHeroBarButtons, one per row, `0x39E1D1`) can be hidden or shown during a
game, as the option does when the bar is built: CSimpleFrame.show_flag
(`+0x90`) to 0 and Hide (vtable `+0x64`), or to 1 and Show (`+0x68`). A
simple frame's Hide and Show don't reach its child frames, though, and a
hero button's OnHide and OnShow (vtable `+0x30`, `+0x2C`) hide and show the
glow of a hero with points to spend (a CSpriteFrame at `+0x1D8`, a model
drawn on its own): the buttons need Hide and Show too, and Show keeps the
ones without a hero hidden (their own flag). While the bar is hidden the
game still shows a new hero's button (its parent isn't the bar), and the
glow when a hero gains a point whether the button shows or not, so
Enhanced Options hides them again each frame. Checked in game: hidden and
shown twice, with a Paladin that had a point to spend.

**Health bars** (`healthbars`, `0x2B`, the Gameplay panel's Always Show
Health Bars): applying the option, and the game UI as it's built, store it
at `CGameUI.healthbars`. Names of the functions guessed.

| What | 1.27b | 1.26a | Notes |
|------|-------|-------|-------|
| CGameUI.healthbars | `+0x1D8` | `+0x1D8` | int: the option. HealthBars::Update inverts the health bar keys with it: on, bars show unless a key is held. |
| HealthBars::Update | `0x386620` | `0x39B2D0` | `void __thiscall()`, plain `ret`. A per-frame update of an in-game UI object (called from `0x385FDC` on its own `this`; not the CGameUI, class not known). Reads the keys into its flags: friendly bars (`+0x180`) while key 2 (Alt) or `0x112` is down, enemy bars (`+0x17C`) while 2 or `0x113` is; both inverted when `CGameUI::Get(1, 0)`'s `+0x1D8` is on. With neither, hides every shown bar but the hovered unit's (`0x390320`); else `0x38E4E0` runs HealthBars::Consider on an eighth of the units each frame. |
| HealthBars::Consider | `0x397B90` | `0x37D0D0` | `void __fastcall(CUnit* unit, int friendly, int enemy)`, `ret 4`. Shows a living, visible unit's bar if it isn't shown yet (`+0x280` bit `0x100`) and its owner is allied to the local player (or `+0x60` bit 1) when friendly, or not when enemy: CUnit::SetHealthBarShown(1, hovered), then lists it among the shown bars. |
| CUnit::SetHealthBarShown | `0x654E20` | `0x2C7460` | `void __thiscall(int shown, int hovered)`, `ret 8`. Shows or hides the unit's health bar (made on first use at `+0x50`). Every path ends here: HealthBars::Consider, hiding all, and the hovered unit's. |

**Controls** (`CControl` and its subclasses: CHECKBOX, SLIDER, POPUPMENU):

| What | RVA | Notes |
|------|-----|-------|
| CControl::HasState | `0x1120C0` | `bool __thiscall(uint32_t flags)`, `ret 4`: all `flags` set in the state at `+0x1D4`. `0x20` is a checkbox's checked bit. |
| CControl::AddState / RemoveState | `0x112030` / `0x113050` | `void __thiscall(uint32_t flags)`, `ret 4`. If the state changed, tail-call vtable `+0xF0` (the look). No listeners are notified; `0x100080` sends a checkbox's CCheckBoxChangeEvent (`0x400C0064`). |
| CSlider::SetValue | `0x0FF110` | `void __thiscall(float value, int notify)`, `ret 8`. The value is the float at `+0x1F4`. |
| CPopupMenu::SetItem | `0x107F00` | `void __thiscall(int item)`, `ret 4`. The selected item is at `+0x1F0`. |
| CPopupMenu::AddItem | `0x107850` | `int __thiscall(const char* text, int value)`, `ret 8`: adds to the MENU at `+0x1EC` (item count at its `+0x1F0`); the first item becomes the selection and the title. The Resolution menu is filled with it (`0x2B7304`, value -2, as FDF `MenuItem`s). |
| Enabled | state bit 1 | vtable `+0x10C` (`0x0FD350`) is SetEnabled: AddState(1) or RemoveState(1). The Options tabs are disabled through it while the panels move. |

### Sound start-up

`0x07F8B0` (`__fastcall`, from `0x0820F0`, once) sets Miles up:
`AIL_set_redist_directory`, `AIL_startup` (`0x07F902`; Miles loads its
`.mix`/`.asi` plugins then, Tome's `tome.mix` among them), closes a previous
driver (`.data 0xD3CEE8`) if there is one, then `AIL_open_digital_driver`
(`0x07F93A`) and keeps the HDIGDRIVER at `0xD3CEE8`. Miles opens the default
device: DirectSound (it loads `DSOUND.DLL` with LoadLibraryA and looks up
`DirectSoundCreate` with GetProcAddress, both from its own imports) or WINMM
`waveOutOpen` (`WAVE_MAPPER`). Nothing reopens it while the game runs (the
Sound panel's checkboxes switch channels, `0x35A9F0`), so a device change
needs a restart. Tome makes the open wait for the first mod load
(`mods_ready.cpp`); the `enhanced-options` mod redirects Miles' two calls.

### Camera

The natives SetCameraField (`0x211E00`) and GetCameraField (`0x1FF430`)
work on the local player's camera, `CGameUI+0x254` (`0x36B190` returns it),
through GameCamera::SetField (`0x362ED0`, `void __thiscall(int field, float
value, float duration, int)`, `ret 0x10`) and GetField (`0x360740`, float
on the x87 stack, `ret 4`). Fields as common.j numbers them: 0 target
distance (1650), 1 far z (5000), 2 angle of attack (304: 56 degrees below
the horizon), 3 field of view (70), ...; SetField takes degrees, GetField
gives radians. The classic mouse
wheel steps through four zoom levels, `.rdata 0xA9F4E0` (heights 160,
angles 304/311/318/325, distances 1650/1600/1500/1400), loaded with movaps at
`0x35EAA2` and `0x360122`: it only zooms in from the default. CGameUI::Get
(`0x36CB20`) makes the UI when called with create = 1; read it with 0.

### Local actions and their delay

Every local player action is serialized into a CDataStore and scheduled by
NetScheduleAction (`0x32C920`, `__fastcall(store, flags)`): the 67 senders,
one per action type (NetSendAction `0x26CE10` among them), copy the action
byte from `event+0x14` with `0x6B170` first. In the store, the byte at
position p is at `[store+4] - [store+8] + p`. Action ids are those of
replays: 0x10–0x14 unit orders (0x10 no target, as Stop and Hold Position),
0x16 a selection change (with 0x19 and 0x1A around it), ... Received actions
come back as game events with id `0xA0000 | action`, their handlers
registered with `0x32D150(id, handler, flag)`. An order's round trip, from
NetScheduleAction to the order event for the unit, measured 130 ms in
single player.

### Text tags

The texttag natives run on the handle manager (`CGameWar3+0x1C`, what
`0x1E0D70` returns, making it if missing) after checking the id against
`.data 0xBDB0F0`: Create `0x283BE0` (returns the id), Destroy `0x284600`,
SetText `0x28BA20` (a C string, height), SetPosUnit `0x28B970` (a CUnit,
z offset), SetColor `0x28B910` (an 0xAARRGGBB by pointer), SetVelocity
`0x28BA50` (two floats by pointer), SetLifespan `0x28B940`, SetFadepoint
`0x28B920`, SetPermanent `0x28B960`, SetVisible `0x28BA60` (it stores a
hidden bit, 4 at `+0x30` of the tag's record; showing hands the tag to the
renderer, `0x122750`). All `thiscall`; several tail-call into `0x9A0xxx`,
which pop as many words as their callers push. Text tags are drawn locally
and aren't agents. A unit's visibility to a player is its vtable `+0xFC`
(slot, 0, 4), as IsUnitVisible (`0x2068D0`) asks. Alliances: GetPlayerAlliance
(`0x2016A0`) reads `CPlayerWar3+0x38` (made on first use by `0x25BA40`),
16 bytes per type from `+0x38`, a reference at `+8`/`+0xC` that `0x54530`
resolves to an object with the players' bitmask at `+0x78`.

### Unit data and custom keys

The unit and ability data (`.data 0xD69FBC`) is built by `0x350DA0` from
the files listed at `.rdata 0xAB6008` (`Units\*Strings.txt`,
`Units\*Func.txt`, CommandFunc.txt, ItemFunc.txt), after start-up's sound
(so after the first mod load). With Option.customkeys (`0x3D`) set, it then
lays `CustomKeys.txt` over them with GameData::LoadOverrides (`0x3534D0`,
`thiscall(path, table .data 0xCE56CC, 0xB, 1)`, `ret 0x10`; the path goes to
the ini loader `0x1A8EF0`, which also opens an absolute path). The ally
color mode (Alt+A) is Option.allyFilter (`0x38`): 0 player colors, 1 team
colors on the minimap, 2 also on the units (checked in game).

### Command card and cooldowns

The command card is a frame grid (made by `0x39D410` and kept at
`CGameUI+0x3C8`): 3 rows of 4 `CCommandButton`s (vtable `0xABCFF4`,
constructor `0x39D5A0`). `0x10EE50` sets a grid's rows and columns (`+0x124`,
`+0x128`) and `0x10E490(column, row, frame, flag)` fills a cell; the rows
are 16 bytes each from the pointer at `+0x154`, their cells through `+8`.
The inventory's six buttons are in the inventory bar (`0x39F380`,
`CInfoPanel+0x148`, the info panel at `CGameUI+0x3C4`): 8 bytes a slot from
the pointer at `+0x130`, the button at `+4`. Command buttons are **simple
frames**: their CLayoutFrame is at `+0`, not at CFrame's `+0xB4` (the game
calls SetPoint and SetWidth on them directly), and a frame can't be made
as their child (CreateFrame crashed adding it to a list of named children
they don't have), so a mod anchors its frames to them from another parent.

A button's `+0x190` is its `CCommandButtonData`, whose `+0x6D4` is the raw
`CAbility*` behind it (an item's ability in the inventory) and `+8` what it
tells it. Starting the sweep (`0x3D2F30`) makes the sprite at `+0x19C` on
first use (`0x3AD950`, skin key `CommandButtonCooldown`) and plays it with
the callback `0x3AD550`, which reads the remaining time through `0x3B62D0`
and stops the sweep (`0x3B7C70`) at 0. Both check the ability first: its
reference (`+0xC`/`+0x10`) through ObjectLookup must give a record tagged
`'+agl'` at `+0xC` with `+0x20` zero. Then they call the ability's vtable
`+0x2DC` (the seconds left) or `+0x2E0` (the fraction left), `thiscall(out,
command)`, `ret 8`. Abilities without a cooldown return 0; those with one
(`0x4149F0`, `0x4148A0`) read their timer at `+0xD0` (vtable `+0x18`) while
bit `0x200` of `+0x20` is set and `0x400` isn't, and keep the cooldown's
length at `+0xB0`. Checked in game: Divine Shield showed 35 to 0, Dust of
Appearance 20 to 0.

**Button positions.** A `CCommandButtonData`'s column and row are at `+0x59C`
and `+0x5A0` (`CCommandButtonData.x`, `.y`). `0x3ACAF0`, which
`CCommandCard::AddButton` runs on a command's existing data before it
stores the new values, resets both to 0, so a coordinate it drops ends up
0 (a Patrol button at (9, 9) shows at (0, 0)). The card places a button with `CCommandCard::PlaceButton`
(`0x3AA430`, `thiscall(data)`, `ret 4`; one caller, `0x3AAED6`, which skips
the call for a button it doesn't show): it indexes the grid with them
unchecked (the row at `[+0x154] + 16 * y`, the cell's button at `[row+8] +
4 * x`), moves to the next free cell while the cell's button has a nonzero
`+0x94`, then fills it (`0x3AA2F0`). The setters on 1.27b store a
coordinate only if it's at most 3 (x) or 2 (y), unsigned:
`CCommandCard::AddButton` `0x3CA1A0` (`thiscall`, nine arguments, `ret
0x24`; x and y the sixth and seventh; adds or updates a command's data in
the card's table at `+0x160`; reached from abilities through `0x377260` ->
`0x372A40`, and from `0x36B6D0`), `CGameUI::SetCommandButtonPos` `0x36B470`
(`thiscall(a, b, {x, y}*)`, `ret 0xC`, from `0x375F20`; for a button on the
card, `0x3B5F20` gives the `CCommandButton`, data at `+0x190`, otherwise
`0x3B60F0` gives the data), and `0x373A90`/`0x373AB0`/`0x373AD0`/`0x373AF0`
(one coordinate each, no callers found). 1.26a's (`0x2F19E0`, `0x2F1A00`,
`0x2F4710`) store any value, and its `PlaceButton` (`0x36D3E0`) reads past
the grid. command-card-guard: `docs/architecture.md`, "Patches".

### Frame scale and the in-game UI's layout

Every CLayoutFrame has a scale at `+0x60` (1 by default; 1.31's
BlzFrameSetScale is the same thing): it multiplies the width (`+0x58`) and
height (`+0x5C`) the frame reports (`0x110BA0`, `0x110B50`) and the offsets
of its own anchor points (a point, made by `0x11E3C0`, resolves to
`x * scale + the relative frame's edge`, `0x11E420`). SetScale is the
layout's vtable `+0x14`: the base (`0x111540`) stores it and updates the
layout, CSimpleFrame's (`0x10DD00`) passes the same value to its child
frames (`+0xD8`) and regions (`+0x120`), CFrame's (`0x0F6410`) to its child
frames, CSpriteFrame's (`0x0F7700`) also scales the model. A layout's points
are pointers at `+8 + 4 * point` (TOPLEFT 0 to BOTTOMRIGHT 8); the frames
anchored to it are a list from `+0x40` (node: next `+8`, dependent `+0xC`;
`0x111280` adds them). SetAllPoints (`0x1114B0`) clears the points and sets
TOPLEFT and BOTTOMRIGHT. `0x111390(layout, 0)` queues a layout for update.

CGameUI is itself a CFrame without anchors: its rectangle is set directly
at `+0x44` (0, 0, 0.6, 0.8), and writing it didn't move what hangs from it.
Anchored to it are the world frame (`+0x3BC`, BOTTOMLEFT 0.13 up, TOPRIGHT
0.02 down: the 3D view stops at the console and the top bar), the console
(`+0x428`, a CSimpleConsole from the FDF frame "ConsoleUI" with the top
bar's and console's textures; the command card, info panel, resource bar
and menu buttons are its children), a second CSimpleConsole (`+0x420`),
two full-screen CFrame layers (`+0x41C` with the minimap `+0x3C0` and the
clock `+0x3F8`, `+0x424` with the portrait `+0x3F4`), the hero bar
(`+0x3DC`), the idle worker button (`+0x3E0`), four text message frames and
three other full-screen frames.

Scaling the in-game UI (Enhanced Options' HUD scale, checked in game at 75%,
60% and 50%, and changed during a game between them and 100%): scale the
console, the second console, resource bar, hero bar, idle worker button,
both layers and the minimap (it isn't in its layer's child list); move the
scaled frames' left and right anchors to CGameUI in by `0.4 * (1 - s)`,
divided by their scale (moving the full-size frames too shifted the
console's art a second time); and move the world frame to the screen's
edges, so it draws beside the narrower console and top bar. Every change
starts from the anchors and sizes recorded as the UI was built, so nothing
moves twice. Behind the bars the world must stay hidden (the console's art
is open around the portrait and info panel, black only because the world
stopped at 0.13): two BACKDROP frames with a black texture, over the bars'
width up to `0.13 s` and down from `0.6 - 0.02 s`, made as children of the
world frame. Those draw after the world and before everything else. The
portrait's model is drawn before every other frame, simple frames
included: a simple frame from CreateSimpleFrame (`0xE47E0`) at level 0
(`+0x84`, its parent's + 1, orders simple frames; the console's is 1, raised
by the game at `0x368006` between a hide and a show, vtable `+0x64` and
`+0x68` with `+0x90` set to 0 and 1 before them) hid the console's art's
gaps but covered the portrait too, and so did a backdrop in the portrait's
own layer. Two 3D models need more: the portrait showed nothing when
scaled, so it stays at 1 with its size and offsets multiplied by hand; the
clock's model is placed in screen units from its frame's corner, so the
frame moves to where the model lands at the top center, and it disappeared
once that offset passed about 0.3, so it's scaled no smaller than 0.7.

Changing the scale while a game runs needs two fixes:

- Text: CSimpleFontString::SetScale (`0x114AF0`, the font string's
  CLayoutFrame.vtable.SetScale) loads the font again through SetFont
  (`0x1148A0`) with the height multiplied by 0.6 (`0x8B38A0`) and flags 0,
  where the string's definition passed its own (`0x116402`). The text came
  out smaller than at that scale, and SetFont keeps the smaller height
  (`+0x90`), so each change shrank it again: back at 100% after one change
  it was about 0.6 of its size. Enhanced Options hooks both: SetScale
  records the height it starts from, and SetFont, called from inside it,
  gets that height and the flags the string was last given. Back at 100%
  the top bar then matched a fresh game (the fix also makes the first
  scale right). GetFont (`0x19D580`) divides the size by 0.6 and caches
  fonts as `"%s-%d-%f"` (name, flags, size).
- The minimap: it lays its picture and its map-to-screen transform
  (`+0x750`) out from its frame only while it sets itself up (vtable
  `+0xE4`, `0x3B7E30`), which its update (`+0x2C`, `0x3C1CB0`) does while
  `+0x1CC` is 0. CMinimap::ReleaseResources (`0x3AEB40`) frees its textures
  and clears it, so the next update sets it up again from the new size.
  The game does the same at `0x211DEA`, and the setup calls it first itself.

The clock and the portrait follow without more: they're laid out again
from their frames. Mouse clicks follow the scaled layout (a click on the
scaled learn-skill button opened the submenu). Edge scrolling doesn't depend
on the layout: the camera controller (vtable `0xAB8E60`, constructor
`0x35D500`) gets the edge as direction events (ids 1 to 10, handler
`0x361540`) and scrolls at the `mousescroll` speed (`+0x4FC`) unless
`mousescrolldisable` (option `0x28`, `+0x30`) is set or user control is off
(`CGameUI+0x1AC`, which cinematic mode clears, `0x36C350`). Checked in game
by logging the camera target (`0x360900` on CGameUI.camera, what
GetCameraTargetPositionX reads) with `TOME_DEBUG_CLICKS` `move:` steps:
every edge, and the bottom edge both over and beside the console, scrolled
the same at 75% as at 100%.

### Battle.net gateways

The servers the Battle.net button connects to (the magnifier next to it
opens the Gateway Selection panel) are a `CBattleNetGateways` at
`CMainMenu+0x4A0`, made by CMainMenu's constructor (`0x2C5A40`) each time the
main menu is built, and gone with it while another screen shows: first
LoadRegistry (`0x30E010`) from `HKCU\Software\Blizzard Entertainment\Warcraft
III\Battle.net Gateways`; if that's missing, `0x30DF80` reads
`BattleNet\bnserver-WAR3.ini` from the archives (`[Server List Version]`
`VER=1001`, `[Server Gateways]` `1=uswest.battle.net` ..., then a section per
host with `ZONE=8` and `ENU=Lordaeron (U.S. West)`); then `realms.ini` is
merged into a second list.

| What | Where | Notes |
|------|-------|-------|
| Registry value | `Battle.net Gateways` | REG_MULTI_SZ: `1001`, the selected index, then host, time zone, name for each gateway. The index is **decimal and counts from 0** (checked in game: `02` selected the third, `11` the twelfth, `0B` the first). Choosing a gateway in the panel and pressing OK didn't write it in a test; the writers (around `0x2E9CF0` and `0x2EFCE3`, through `0x300730`) weren't followed further. |
| CBattleNetGateways | `+0` version, `+4` selected, `+0xC`/`+0x10` count and entries, `+0x1C`/`+0x20` the second list's | 0x204-byte entries: zone `+0`, host `+4`, name `+0x104`. GetGateway `0x2DECC0` (index across both lists), the selected index or -1 `0x2DF620`. |
| Connecting | `0x2E5D65` | Formats `%s.WAR3.battle.net;%s` with the host: the game tries `<host>.WAR3.battle.net` first, then the host itself. |

### Control clicks and observers

`CObserver` (vtable `0xA84314`, 12 bytes: vtable, reference count at `+4`,
registration list at `+8`; constructor `0x071750`) receives events through
slot 3, `int __thiscall OnEvent(CEvent*)`. Slot 0 drops the last reference
(calls slot 1, the deleting destructor, which frees with Storm).

| What | RVA | Notes |
|------|-----|-------|
| CControl::SetListener | `0x112D80` | `void __thiscall(int command, CObserver* listener, int enable)`, `ret 0xC`. Calls the control's vtable `+0x08`(event `0x40090064`, command, listener): a registration node holding a reference to the listener. |
| CControl::Click | `0x112100` | `void __thiscall(int button)`, `ret 4`. Mapped. Returns at once unless the control is enabled (CObserver::HasEvent with `0x40090064`, mapped as `ControlEvent.click`); fills the event at `0xCE3BA8` (`ControlClickEvent`; 1.26a `0xA9A7B4`; id `0x40090064` at `+8`, the button at `+0x10`, the control at `+0x14`) and dispatches it through the control's vtable `+0x10`. The listener's OnEvent sees its registration's command at `event+8`, in that same event object: UI timers' events come in others (seen in CMainMenu's OnEvent: 0x17–0x1A at `0xCE54B0`/`0xCE56F8`, button commands at `0xCE3BA8`), which is how Tome's main-menu hook tells a button from a timer. Mouse paths pass the mouse event's button, a shortcut path (`0x0FEA8F`) passes 1. Tome clicks the Esc menu's `ConfirmQuitQuitButton` with it to restart from a game (closing the window there asks Confirm Exit first). |
| CObserver::HasEvent | `0x072190` | `int __thiscall(uint32_t event)`, `ret 4`. Mapped. Nonzero if the object's registration list (`+8`) has the event id (lookup `0x0721D0`; no list: 0). The check in CControl::Click; Tome's developer builds call it from their Click hook so `until:` only counts clicks that reached a listener. CPlayerWar3's `On*` functions ask it the same about trigger events. |

Glue BindControl is GetFrameByName + SetListener(menu) + enable + the
menu's control list at `+0x210`, so it only suits glue menus. Tome
registers its own observer (a static object with a copy of CObserver's
vtable whose slot 3 is Tome's, reference count started high so it's never
freed) on any control instead; that works on every screen and in games.

### Games

| What | RVA | Notes |
|------|-----|-------|
| CGameWar3 instance | `.data 0xD687A8` | `CGameWar3*` (vtable `0xA93DF4`). A new object is made for each game. `[game+0x30]` is the map info: RCStrings with the name at `+0x4`, the description at `+0x10` (trigger strings resolved) and the archive path at `+0x24` (what /mapname shows). |
| CGameWar3::Reset | `0x1DFAF0` | `void __thiscall(int leaving, int)`, **`ret 8`**. Destroys CGameUI (`+0xD8(1)`, then CGameUI::Get(0, 1)) and calls InitNatives (`InitNatives.call.map_load_1`). Runs with leaving = 0 before a single player game loads (from the menus and with `-loadfile`), but also when a LAN lobby is created or left, and inside the saved game loader; with leaving = 1 when the player leaves the score screen. The natives a map uses are bound here. So it marks no clear point in a game's life; Tome uses the loading screen instead. |
| Saved game loader | `0x1E3730` | `int __fastcall(setup, ?, int)`, `ret 4`, contains `InitNatives.call.map_load_2` and the loading progress calls (`0x2D11B0`). One caller (`0x2A2633`, right after SetScreen(loading)). Runs when a saved game is loaded, and calls CGameWar3::Reset itself (through `0x1DF880`) partway through. Not on the paths of new single player or LAN games, nor replays. |
| CGameUI::Get | `0x36CB20` | `CGameUI* __fastcall(int create, int forget)`, global `0xD6A8B8`, 479 callers (nearly all with create = 1). |
| CGameUI::OnEvent | vtable slot 3, `0x36F140` | Enter in the chat box ends at `0x36EE10` (send chat): reads the line from the chat bar (`[ui+0x3FC]`, CChatEditBar; text via `0x3B6DE0`), calls HandleSlashCommand, then sends it to the chosen recipients. |
| CGameUI::HandleSlashCommand | `0x371E80` | `int __thiscall(const char* text)`, `ret 4`, one caller. Lines starting with `/` are compared with a table at `.rdata 0xAB8F78` of `{name, length}`: mapname, squelch, unsquelch, ignore, unignore, fps, showminimapsignals, hideminimapsignals. Returning 1 means handled: the line isn't sent and the chat box closes. |
| CGameUI::DisplayText | `0x374DE0` | `void __thiscall(float x, float y, const char* text, float duration, uint32_t color)`, `ret 0x14`. What DisplayTimedTextToPlayer (`0x1FD8B0`) calls, with color `0xFFFFFFFF`. |
| PresentFrame | `0x136FE0` | `void __fastcall(int flags)`: the graphics device's vtable `+0x48` (D3D `0x140950`, OpenGL `0x144580`) on the device at `0xD52EB8`, then `0x092F70`. Runs once per frame on the main thread everywhere: about 165 times a second under Wine in menus, loading screens, games and the score screen. |
| Net client update | `0x32D670` | Each frame, adds the real time passed, scaled by the game speed (`+0x22B4`/`+0x22B8`), to a bank at `+0x2250`, and while there are at least 25 ms in it and the turn data allows, dispatches `CNetEventGameTick` (event `0x40090092`, vtable `0xAB4610`) a speed-dependent number of times per 25 ms, plus idle ticks (`0x40090093`, `0x40090094`). Nothing accrues while the game is paused. |
| OnNetGameTick | `0x36ABA0` | `int __fastcall(CNetEventGameTick* event)`, plain `ret`; registered with `0x32C490` by `0x372920` (which Reset calls with 0 to unregister). Calls `0x04C1A0` with `event+0x14`, which advances the game's schedulers (agent timers: what JASS timers and waits run on) by that step. The step is a float, 0.005 s, from `.data 0xD69C48` (set at startup). Measured: 200 calls per second at the normal (Fast) speed, none while paused. |
| GameShutdown | `0x044740` | `void __cdecl()`. GameMain (`0x046B60`, the export) runs init (`0x045080`), the main loop (`0x072CF0`), then this. Exit Program (after its Confirm Exit dialog) and closing the window (`WM_CLOSE` shows the same dialog) both leave the game with Reset first, then get here. |
| `0x0ED680` | | `void __stdcall(float dt)`, draws the UI layers. Only called while a loading screen draws, so it is no frame tick. |

### Frame rate

The frame rate has nothing to do with the game's speed: the simulation runs
on the net client's 25 ms turns (above). What limits frames, measured under
Wine (the separate `enhanced-options` mod, once `fps-unlock`, is built on it):

| What | RVA | Notes |
|------|-----|-------|
| Main loop | `0x07B1C0` | `int __stdcall(int)`, `ret 4`: a scheduler of timed tasks per thread; on the main thread `0x07A1C0` calls it with 1 (seen in a backtrace). Drawing a frame is one of the main thread's tasks. It waits for the next task's time through its own wait (`0x096820`), not `Sleep`, and measures every deadline with GetTickCount (the thunk `0x092A10`). |
| Scheduler::SetPeriod | `0x073630` | `void __fastcall(int period_ms, void* context)`, plain `ret`. Stores the loop's period at `+0x48` of the record for `context` (0: the calling thread's, through `0x06C180`). The main thread's period spaces its frames. |
| Options::Get | `0x040ED0` | `void* __cdecl()`: the options object (`.data 0xD3C5F0`), made on first use. Options are numbered; their table is 16 bytes per id from `.rdata 0xA7EFE8`: registry name, ?, type, default text. |
| Options::Apply | `0x040310` | `int __thiscall(int option)`, `ret 4`: applies an option's stored value through the switch at `0x0403E0` (`ret 8`, id − 5 indexes `0x04098C`). Startup applies a list of them (`0x043ED0`). |
| maxfps | option `0x16` | Video\maxfps in the registry, default 200, also registered by name by `0x37AA70` with the callback `0x379E20` (store, then apply). Its case (`0x040607`) calls Scheduler::SetPeriod((int)(1000.0 / maxfps), 0). maxfps 60 gave 64.5 fps, 200 gave ~218, 1000 gave ~430 (vsync off, menu). |
| GetTickCount | import slot `0xA7C148` | 118 calls through the thunk `0x092A10`, a few `call [slot]`. Windows advances it every 15.6 ms; frames can't come faster than that (Wine: 1 ms). Rounding it down to 16 ms steps under Wine dropped maxfps 200 to 62.5 fps, the classic ~64 cap. The net clock `0x092A20` uses QueryPerformanceCounter instead when `.data 0xD3D5C4` is set, else GetTickCount; the net client subtracts readings unsigned and caps its bank at 4000 ms (`0x32D812`), so a clock that steps back would run the game up to 4 s ahead. |
| CGxDeviceD3d::BuildPresentParams | `0x13FED0` | `void __thiscall(D3DPRESENT_PARAMETERS*)`, `ret 4`. Callers: CreateDevice (`0x13FC20`, IDirect3D9 vtable `+0x40`) and the reset after a lost device (`0x1404DD`, IDirect3DDevice9 `+0x40`). In fullscreen the size, format and refresh rate (`+0x30`) are those of display mode `+0x98` (picked at start-up) of adapter `+0x94` from the IDirect3D9 at `+0x580` (EnumAdapterModes, vtable `+0x1C`); in a window `Windowed` (`+0x20`) is 1 and the format the desktop's (GetAdapterDisplayMode, `+0x20`). PresentationInterval (`+0x34`) is always 0 (DEFAULT): vsync on. OpenGL never calls `wglSwapIntervalEXT`, so the driver decides. |
| CGxDeviceD3d::CanDraw | `0x140410` | `bool __thiscall(device)`, plain `ret`. Device vtable `+0x3C`, before every frame. TestCooperativeLevel; on `D3DERR_DEVICENOTRESET` (or, in a window only, a resize pending at `+0x730`) ReleaseForReset(0), BuildPresentParams, `IDirect3DDevice9::Reset` (`+0x40`), RestoreState. False while the device is lost. Running the same four steps from a BEFORE hook on it resets the device on demand, in a window or fullscreen: Enhanced Options does so to change vsync (and the fullscreen refresh rate) during a game. Checked in a window under Wine: Reset returned 0 every time, the game drew on, and the frame rate went from 165 (vsync, a 165 Hz monitor) to 190 and back. |
| Fullscreen ↔ window | | The device's fullscreen display mode is 5 ints at `+0x98` (CGxDevice.display_mode): its index in the adapter's list for the format, width (`+0x9C`, the "fullscreen flag" everything reads), height, bits and refresh rate; all 0 in a window. The window is made at `0x1400C0` with style `0x06CF0000` and ex-style `WS_EX_APPWINDOW` in a window, `0x86080000` and `WS_EX_APPWINDOW \| WS_EX_TOPMOST` in fullscreen, and a window is centered (`0x140020`). Switching during a game (Enhanced Options, checked under Wine in both directions, from a fullscreen start and a windowed one): fill or clear the mode (the Video options' size and depth, an EnumAdapterModes index), restyle the window (fullscreen: the mode's size at the monitor's corner, before the Reset; a window: after it, as Direct3D gives back the style the window had when it went fullscreen), then the Reset above. Under Wine (wined3d) a device created fullscreen then drew nothing into the window until it was minimized and restored once (wined3d also warns it can't unhook the window: its procedure was replaced after its own); the switch does that. OpenGL makes its mode as it starts (ChangeDisplaySettingsExA, `0x13ACFA`), so it still needs a restart. |
| CGxDeviceD3d::ReleaseForReset | `0x13FDC0` | `void __thiscall(device, int release_textures)`, `ret 4`. Clears the state caches (render states `+0x748`, 256 hashed `{kind, state, value}` slots of 12 bytes, which `0x142C60` checks before every SetRenderState; texture stages `+0x1348`/`+0x1358`; lights `+0x1B58`, 8 × `0x74`) and releases the surfaces at `+0x6C0..+0x6D8`; with 1 also the textures (`0x143B00`). Called with 0 before Reset (`0x1404D2`), with 1 by the teardown (`0x13FD2B`). The d3d-reset-lights patch hooks it. |
| CGxDeviceD3d::RestoreState | `0x142E50` | `void __thiscall(device)`, plain `ret`. After CreateDevice (`0x13FC9A`) and Reset (`0x1404FA`): z, lighting and fog states, the 8 lights from `+0x3C0` (enabled 0–4 on the main menu), the cull mode, then SetGamma(gamma, force 1). |
| CGxDeviceD3d::SetGamma | `0x13F870` | `void __thiscall(device, float exponent, int force)`, `ret 8`. Vtable `+0x1C`. Returns early if the exponent equals the cached one (`CGxDevice.gamma`, `+0xD8`) and force is 0. In fullscreen (`+0x9C` nonzero) with `D3DCAPS2_FULLSCREENGAMMA` (`0x20000` in `+0x594`) sends `SetGammaRamp` with `(i/255)^(exponent + rand()/32767/1000) × 65535`. The Gamma option g gives exponent `(100 − g)/100 + 0.2` (Options::Apply, `0x04057F`, through `0x136B80`). |
| CGxDeviceD3d::WndProc | `0x140210` | `LRESULT __stdcall(hwnd, msg, wparam, lparam)`. Tracks focus in `.data 0xD54400`; on WM_SYSKEYUP Tab while focused (leaving with Alt+Tab) it sets a neutral ramp (SetGamma(1.0, 1)) for the desktop and puts the user's exponent back in the cache unapplied; RestoreState applies it after the Reset on return. |
| CGxDeviceOpenGl::CanDraw | `0x1442F0` | `bool __thiscall(device)`, plain `ret`. Device vtable `+0x3C`, called before every frame (`0x1365C0`). True if the viewport (`+0xBE4..+0xBF0`) has an area: false while minimized. Nothing recovers on restore (Direct3D's `0x140410` resets the device). |
| CGxDeviceOpenGl::ReleaseTextures | `0x13ED30` | `void __thiscall(device)`, plain `ret`. glDeleteTextures for the `+0x540` count / `+0x544` array of textures, clears each name (`+0x30`) and marks it dirty (`0x13A1E0`); the upload (vtable `+0x00`, `0x13EDB0`) makes new ones. |
| CGxDeviceOpenGl::DestroyContext | `0x13AFA0` | `void __thiscall(device)`, plain `ret`. ReleaseTextures, wglMakeCurrent(0), wglDeleteContext (`+0x580`), release the DC (`+0x57C`). |
| CGxDeviceOpenGl::CreateContext | `0x13AF40` | `bool __thiscall(device)`, plain `ret`. GetDC of the window `+0x578`, wglCreateContext (`0x13F5C0`), make current, driver queries (`0x13F2E0`), GL state setup (`0x13DCC0`). |
| Movie::SetDisplayMode | `0x13C3A0` | `void __fastcall(int cinematic, const int* mode)`, plain `ret`. The movie player's display mode (GxMovie\GxVideo.cpp): Movie::Play (`0x13D520`) calls it before the movie with cinematic = (fullscreen game), `0x13D760` with 0 after it. cinematic is `cl` only (`sete cl`; the rest of `ecx` is junk). Set: EnumDisplaySettingsA(ENUM_CURRENT_SETTINGS), then the 24 {width, height, bits, frequency} modes at `.data 0xCE4230` (with `mode`'s fields where its flag bits say) through ChangeDisplaySettingsA(CDS_TEST \| CDS_FULLSCREEN) (`0x13C4D2`); the first that passes is set (`0x13C50B`) and kept at `.data 0xD5314C`. Otherwise ChangeDisplaySettingsA(NULL, 0), and while that fails Sleep(50) and again, with no limit: under Wine on a display that refuses it (Xvfb) the game hangs on a black screen before the movie starts. The movie-display-reset patch replaces that loop (`docs/known-bugs.md`, "Under Wine"). |
| Movie.cinematic_mode | `0xD5314C` (data) | 4 uint32: width, height, bits, frequency. Movie::SetDisplayMode (cinematic) writes the current mode's width, height and bits first, then the table entry it sets over all four. Movie::Play (`0x13D60E`) makes the fullscreen movie window with CreateWindowExA(WS_EX_TOPMOST, WS_POPUP, 0, 0, width, height). Under Proton the 800x600 mode is emulated, so that window is 4:3 on any monitor; movie-display-reset puts the desktop's registry mode back and writes it here instead (a mode Proton scales leaves light lines along the video's window). |
| Movie.gamma_call | `0x13D72C` (data: an instruction) | In Movie::Play, fullscreen only: `call [SetDeviceGammaRamp]` (`ff 15` + import slot, 6 bytes) with ramp[i] = pow(i / 255, 1 / 2.2) * 65535 on all channels (exponent: double at `0xA90310`). The ramp before is saved at `0xD53160` (GetDeviceGammaRamp at `0x13D6B5`) and set back at `0x13CE93`. Proton applies it to the movie's OpenGL output, washing it out; movie-display-reset points the call at a no-op under Wine. |
| Movie.gamma_restore | `0x13CE93` (data: an instruction) | `call [SetDeviceGammaRamp]` with the ramp saved at `0xD53160`, in `0x13CE80`, which the movie window's procedure calls (`0x13D93E`) when Enter, Esc or Space skips the movie. Only a fullscreen movie fills `0xD53160`, so skipping a windowed one sets zeros. movie-display-reset points it at a no-op under Wine too, after Movie.gamma_call. |

OpenGL fullscreen under Wine: minimizing makes Wine drop the driver's context
(`wglMakeCurrent(NULL)` it issues itself) and restoring makes a new, empty one
for the same HGLRC (`+wgl`: `win32u_wgl_context_reset`, `egldrv_context_create`),
so every texture and all GL state are gone. The game also releases its
textures and makes a new context of its own on the way back (the HGLRC
changes, `0x21001` → `0x31001` in one run); textures uploaded before Wine's
reset are lost but keep names the new context hands out again, so
`glIsTexture` on them can't detect it. The gl-context-restore patch (and
fps-unlock's first fix) runs DestroyContext + CreateContext on the first
CanDraw true after a false, if the device was fullscreen meanwhile
(`CGxDevice.fullscreen`, `+0x9C`: nonzero in fullscreen for both renderers;
`0x138990` returns it negated, and the gamma setters `0x13F870` and
`0x13AD60` send a ramp only when it's set). DestroyContext skips what's
already gone (`+0x580`, `+0x57C` are 0), so it's safe after a failed
CreateContext, which calls it itself; the patch retries the pair. The device's textures: count at `+0x540`, array
at `+0x544`, each with its GL name at `+0x30` (`CGxDeviceOpenGl.texture_count`,
`.textures`, `CGxOglTexture.gl_name`).

Direct3D fullscreen under Wine: minimizing loses the device, and the reset on
restore runs into a wined3d bug that leaks the active-light slots of the
lights that were on (`wined3d_stateblock_reset` frees the lights but not its
`light_state.lights[]` table). The game re-enables lights 0–4 after Reset and
wined3d refuses the ones past the free slots ("Too many concurrently active
lights" with `warn+d3d`), so the scene gets darker on each Alt+Tab. The game's
D3D9 calls in a frame are the same before and after (`+d3d9` trace), and the
gamma ramp is sent again by RestoreState. Tome's d3d-reset-lights patch
disables the 8 lights before Reset (only on ReleaseForReset(0), not the
teardown's ReleaseForReset(1)) (`docs/known-bugs.md`, "Under Wine"),
through the `IDirect3DDevice9*` at `+0x584` (`CGxDeviceD3d.d3d_device`, 0
before CreateDevice and after teardown). The device's window is at `+0x574`
(`CGxDeviceD3d.window`: CanDraw's GetClientRect, DestroyWindow in `0x1401E0`).

The renderer is picked once at startup (`0x044ED0`): Direct3D, unless the
registry's `HKCU\Software\Blizzard Entertainment\Warcraft III\Gfx OpenGL`
(DWORD, read with Storm's registry call `0x07C6C6`) isn't 0 or command-line
flag 1 (`-opengl`, checked by `0x064770`) is set; flag 0 then forces
Direct3D again. `0x136720` creates the device (0 OpenGL, 1 Direct3D).

Measured (OpenGL under Wine, 165 Hz monitor): vsync on 165 fps; vsync off
(`__GL_SYNC_TO_VBLANK=0`) and maxfps 200: ~214 in a game; maxfps 1000:
~315. Direct3D (wined3d) stays near 110–120 either way: its main thread and
command-stream thread are both at 100% CPU. Game seconds per real second
stayed 1.00 in every case.

Order seen, all from probes in the game:

| Path | Calls |
|------|-------|
| Single player game from the menus | SetScreen(4), (5), Reset(0) (natives bound), SetScreen(1) (loading), EnterGame; leaving: SetScreen(12) (score screen), Reset(1), SetScreen(5) |
| `-loadfile` | SetScreen(2), Reset(0), SetScreen(1), EnterGame |
| LAN game, hosted | Create Game (lobby): Reset(0); Cancel: Reset, Reset(0) again; Start Game: SetScreen(1), EnterGame; leaving as above |
| Saved game | SetScreen(1), `0x1E3730` (Reset inside), EnterGame; the map info has its path but no name until EnterGame |
| Replay | SetScreen(1), EnterGame; at its end SetScreen(12), Reset(1) |

The loading screen (SetScreen(1)) is the one point every way into a game
passes, after the natives are bound and before the map's script runs.

| What | RVA | Notes |
|------|-----|-------|
| CFrame::SetFocus | `0x0F0A00` | `void __thiscall(int focus, int, CFrame* other)`, `ret 0xC`. `focus = 1` gives the frame the keyboard focus (if its vtable `+0xAC` agrees), `focus = 0` takes it from the focused frame (vtable `+0xB0` on that one). The focused frame is `.data 0xD47830` (`0xD47834` set blocks changes). CChatEditBar::SetActive (`0x3AF7F0`) uses it. |

### Players, units and the selection

Found from the natives that read them (`GetLocalPlayer` `0x200BA0`,
`IsUnitSelected` `0x206690`, `SelectUnit` `0x2119A0`, `GetUnitTypeId`
`0x2040C0`, `GetOwningPlayer` `0x2015F0`, `IsUnitType` `0x206710`,
`GetUnitName` `0x203D90`) and checked in the game with a probe mod.

| What | RVA / offset | Notes |
|------|-----|-------|
| Local player | `CGameWar3+0x28` | `uint16_t` slot. GetLocalPlayer reads `+0x2A` instead when `0x32C110` says so (net client `+0x610 == 1`; not seen yet). |
| CGameWar3::GetPlayer | `0x1E0EA0` | `CPlayerWar3* __thiscall(int slot)`, `ret 4`: `[game+0x58+slot*4]`, count at `+0x54`. |
| Handle → unit | `0x1EEF90` | `CUnit* __fastcall(handle)`: what the unit natives start with. `0x1EDE10` does the same for players. |
| CPlayerWar3 | | slot at `+0x30` (byte, GetPlayerId), name RCString at `+0x24`, CSelectionWar3 at `+0x34`. `CPlayerWar3::GetName` (`0x2684B0`, thiscall, `ret 4`) with 1 writes "Player N" into the name when it's empty. |
| CSelectionWar3 | vtable `0xAA29D4` | Two unit groups: one at `+0x0` (IsUnitSelected also checks it), and the **local** one at `+0x1E8` with its count at `+0x1F8` (`0x290270`), max 12. `+0x1E0` is the local leader (the active unit, `0x28FFE0`); `+0x1AC` the player. The game's debug strings name them: "Add local %s %x:%x for player %d" (`0x28DBE0`, add), "Async set local leader to %s" (`0x293110`); `0x2928C0` removes. |
| Unit group | | List head at `+0xC` (a value <= 0 ends it), count at `+0x10`; node: next at `+4`, `CUnit*` at `+8` (`0x290820`). Units in it hold a reference (`+4`). |
| CUnit | vtable `0xB77EB0` | agent id (two words) at `+0xC`, type id at `+0x30` ('hpea'), owner slot at `+0x58` (vtable `+0xEC`), flags at `+0x5C`: `0x100` dead (vtable `+0x13C`), `0x10000` structure (`0x68C190`). Hero (`0x205540`): the type id starts with A–Z and flag `0x40000000` is clear. `+0x20` bit 2: selectable (SelectUnit does nothing without it). |
| GetUnitTypeName | `0x3442F0` | `const char* __fastcall(type_id, index)`: the localized Name. |
| Handle manager | `CGameWar3+0x1C` | (`0x1E0D70` creates it when missing.) Agent → handle id: `0x282E30(agent, flag)` hashes the agent's id word at `+0x10` (mask `+0x1C8`, 12-byte buckets at `+0x1C0`: link offset, -, first node; node: key `+0`, next `[node+link+4]`, handle `+0x18`), checks the table at `+0x19C` (12 bytes per id from `0x100000`, the object at `+4`) and returns the id; **if there's none it makes one**. Tome reimplements only the lookup: a handle made on one client shifts later ids there. |

**Selection paths.** A click goes through "Add local" (`0x28DBE0`), and so
does `SelectUnit(u, true)` for a selectable unit; `SelectUnit(u, false)`
calls `0x2928C0`. Control groups, drag boxes and Tab end in the same local
group and leader. Tome compares the local group and leader once per frame
instead of hooking each path. A trigger's pause (`CGameUI::SetPaused` with
a4 = 1) resets the UI and clears the selection in the same frame.

**Player-unit events (death, training, orders...)** run through
`0x262CB0` (`CPlayerWar3`, thiscall(id, data)) with internal ids JASS id +
`0x80200` (`TriggerRegisterPlayerUnitEvent` adds it; ids from `common.j`).
Every site first asks `0x072190(id)` whether any trigger is registered and
builds the event data only then, and the dispatch makes a handle for its data
(`0x282E30` with 1), so hooking the dispatch only sees events a map already
listens for. But each event has its own small function on the unit's owner,
`CPlayerWar3::On*`, called unconditionally by one `CUnit` function, that does
the registration check itself; its arguments go to the event data through
`0x26AFE0` (the triggering unit), `0x26AC20` (the other unit) and `0x26AA70`
(the item). Tome hooks those (BEFORE):

| JASS event | Function | Arguments after `this` | Caller |
|------------|----------|-----------|--------|
| UNIT_DEATH (20) | `0x260AB0` | unit, killer (may be null), `ret 8` | `0x67B8A0` |
| UNIT_CONSTRUCT_FINISH (28) | `0x2608B0` | building, `ret 4` | `0x67B630` |
| UNIT_UPGRADE_FINISH (31) | `0x2628A0` | unit (new type), value, `ret 8` | `0x67D960` |
| UNIT_TRAIN_FINISH (34) | `0x262580` | building, trained unit, `ret 8` | `0x67D600` |
| UNIT_RESEARCH_FINISH (37) | `0x2617F0` | unit, research id, `ret 8` | `0x67C690` |
| HERO_LEVEL (41) | `0x25FE60` | hero, `ret 4` | `0x67A230` |
| UNIT_SUMMON (47) | `0x262260` | summoner, summoned, `ret 8` | `0x67B0B0` |
| UNIT_DROP/PICKUP/USE_ITEM (48–50) | `0x260EC0`, `0x2613E0`, `0x262AC0` | unit, item, `ret 8` | `0x679FE0`, `0x67ACF0`, `0x67DBA0` |
| PLAYER_LEAVE (15) | `0x260460` | none | network code (4 sites) |
| UNIT_ATTACKED (18) | `0x2605B0` | attacker, attacked, `ret 8` | `0x67B2E0` |
| UNIT_RESCUED (19) | `0x2615F0` | rescuer, rescued, `ret 8` | `0x67C450` |
| UNIT_CONSTRUCT_START, CANCEL (26, 27) | `0x2609B0`, `0x2607B0` | building, `ret 4` | `0x54A5B0`/`0x54D0F0`/`0x54D9D0`, `0x67B520` |
| UNIT_UPGRADE_START, CANCEL (29, 30) | `0x2629B0`, `0x262790` | unit, type, `ret 8` | `0x67DA80`, `0x67D840` |
| UNIT_TRAIN_START, CANCEL (32, 33) | `0x262680`, `0x262470` | building, unit type, `ret 8` | `0x67D720`, `0x67D4E0` |
| UNIT_RESEARCH_START, CANCEL (35, 36) | `0x2618F0`, `0x2616F0` | unit, research, `ret 8` | `0x67C7B0`, `0x67C570` |
| HERO_SKILL (42) | `0x260360` | hero, ability, level, `ret 0xC` | `0x67A770` |
| HERO_REVIVABLE, REVIVE_START, CANCEL, FINISH (43–46) | `0x25FF60`, `0x260260`, `0x260060`, `0x260160` | hero, `ret 4` | `0x26AD96`, `0x67A660`, `0x67A440`, `0x67A550` |
| UNIT_SELL (269), SELL_ITEM (271), PAWN_ITEM (277) | `0x261AF0`, `0x261C00`, `0x2612D0` | shop, sold, buyer / shop, buyer, item / seller, shop, item, `ret 0xC` | `0x67C8D0`, `0x67CA00`, `0x67C100` |
| UNIT_CHANGE_OWNER (270) | `0x2606B0` | unit, previous owner (low 16 bits), `ret 8` | `0x67B400` |
| UNIT_DECAY (21) | `0x260BC0` | unit, `ret 4` | `0x67B9C0` |
| UNIT_DETECTED, HIDDEN (22, 23) | `0x260DC0`, `0x260FC0` | unit, `ret 4`; `this` is the **detecting** player (the `CUnit` caller gets it as its argument) | `0x67BAD0`, `0x67BBF0` |
| END_CINEMATIC (17) | `0x25FD70` | none, `ret` (Esc, in a cinematic or not) | network handler `0x26F240` |
| ARROW_* (261–268) | `0x25FA10` | the event id (`0x80305`–`0x8030C`: left, right, down, up, each down then up), `ret 4` | network handler `0x26EE00`; the sender (`0x26FF80`, from the key handlers `0x36F870`/`0x36FF10`) sends the action **only if the local player has that event registered** (Esc, by contrast, is always sent) |

The event data's fields (and so the roles above): `0x26AFE0` writes the
triggering unit's agent id to `+0x2C`, `0x26AC20` the other unit to `+0x38`,
`0x26A6A0`/`0x26AE70`/`0x26AA70` a third unit or item to `+0x44`; a type id
goes to `+0x44` directly (`GetTrainedUnitType`, `GetResearched` read it), a
skill's level to `+0x48`. Getters: `GetSellingUnit` `+0x30`, `GetSoldUnit`
`+0x3C`, `GetBuyingUnit`/`GetSoldItem` `+0x48`, `GetLoadedUnit` `+0x30`,
`GetTransportUnit` `+0x3C`.

**Orders and loading** are fired by `CUnit` functions that check both the
per-unit and the player registration first and return when neither is set,
so Tome hooks those instead: `CUnit::OnOrder` (`0x67BD10`), `OnPointOrder`
(`0x67C230`), `OnTargetOrder` (`0x67D2B0`), each `(this = unit, COrder*)`,
`ret 4`, picked by the dispatchers `0x67ABE0` and `0x688790` by the order's
class; and `CUnit::OnLoaded` (`0x67A950`, `this` = the loaded unit, the
transport, `ret 4`). `COrder`: id at `+0x24` (train orders use the unit
type as id), point at `+0x48`/`+0x50` (floats; a target order on a building
under construction had 0, 0), target from `0x686810` (the agent id at
`+0x58`): a unit (`CUnit` vtable `0xB77EB0`), an item (`CItem` `0xB77AA8`) or
a destructable (`0xB7D708`). The player functions are `0x2610C0`,
`0x2614E0`, `0x262360` and `0x2611D0`.

**Spells, damage, acquired targets.** The spell events go through `CUnit`
functions that check the registrations (unit and player) themselves and
make a handle for their data, so Tome hooks those, like the orders:
`CUnit::OnSpellChannel` (`0x67CCB0`), `OnSpellCast` (`0x67CB30`),
`OnSpellEffect` (`0x67CE30`), `OnSpellFinish` (`0x67D130`), `OnSpellEndCast`
(`0x67CFB0`), each `(this = caster, CAbility*)`, `ret 4`; the player
functions are `0x261E20`, `0x261D10`, `0x261F30`, `0x262150`, `0x262040`
(JASS 272–276, internal `0x80310`–`0x80314`). The ability's type id is at
`CAbility+0x34` (`0x684140`). `GetSpellTargetUnit` (`0x202D00`) and
`GetSpellTargetX`/`Y` (`0x202E30`, `0x202F10`) call the ability's virtual
functions `+0x3C8` (the target widget, thiscall), `+0x3CC` and `+0x3D0`
(`float*(float* out)`, `ret 4`); `CAbilitySpell` and its subclasses
(`CAbilityItemHeal` too) have them, `CAbility` itself doesn't, and
`CAbilitySpell`'s own return no widget and 0, 0. Damage and acquired
targets are unit events only (52, 60): `CUnit::OnDamaged` (`0x67B740`,
`(float* damage, CUnit* source)`, `ret 8`, called unconditionally by
`0x69CA80`, which applies the damage afterwards) and `CUnit::OnAcquireTarget`
(`0x67B1D0`, `(CWidget* target)`, `ret 4`, from the attack AI). The
selection events (24, 25: `0x2619F0`, `0x260CC0`) come from `CUnit`
vtable functions (`0x67AF30`, `0x679E70`, `(slot, now)`) that, unless
`now`, send a network action (`0x6BD200`) **only if a trigger listens**,
so other clients never see a selection nobody listens for.

**Widget deaths and dialogs.** `CWidget::OnDeath` (`0x650D80`, thiscall, no
arguments) posts `EVENT_WIDGET_DEATH` (89) for units, items and
destructables alike (`CDestructable` vtable `0xB7D708`, type id at `+0x30`,
`GetDestructableTypeId` `0x1FFA70`). `CScriptDialog::OnClick` (`0x22E540`,
`(button, player slot)`, `ret 8`) posts `EVENT_DIALOG_CLICK` (91) to the
dialog and the button event (90) to the button; the network handler
`0x26EFF0` calls it. Widget positions (`GetWidgetX`, `0x2041B0`) go through
the vtable `+0xB8` and a sprite transform (`0x058900`), not a field. Alliance
changes (12) are posted from somewhere `SetPlayerAlliance` (`0x213CC0`)
reaches indirectly; not found yet.

**Game events.** `HandleManager` (`CGameWar3+0x1C`) fires the game events
through `0x284A70` (`(event id, widget)`, `ret 8`): `0x284B90` and
`0x284BA0` post `EVENT_GAME_SHOW_SKILL` (9) and `EVENT_GAME_BUILD_SUBMENU`
(10), `0x2831B0` `EVENT_GAME_END_LEVEL` (1, from the end of `0x1E7630`, which
`ChangeLevel` `0x1F89E0`, `LoadGame` `0x20E070` and the restart path
`0x1E7370` go through), `0x284810` and `0x2848F0` the tournament events
(257, 258; Battle.net tournaments only). The two menus are network actions:
the command card (`0x392C90`, `0x393AF0`, `0x3BCB90`) sends action `0x66`
(skill menu, `0x2705D0`) or `0x67` (build menu, `0x270640`) on **every**
click, and their handlers `0x26F610` and `0x26F5E0` (fastcall, the action in
`ecx`, its sender's slot at `+0x15` as for every action) call the
`HandleManager` functions. Tome hooks the handlers, for the player.

**Units created and removed, experience.** `0x677870` (`CUnit*
__fastcall(int player, uint32_t type, const float* x, const float* y, const
float* facing, flags, int, int)`, `ret 0x18`) creates and places every
unit: the `CreateUnit` natives (through `0x64E650`, flags `0x502`),
training (`0x600E60`), summons and the map's units, 31 callers in all.
`RemoveUnit` (`0x210C10`) calls the unit's vtable `+0x84`, then
`CUnit::Remove` (`0x694690`, `(1, 1)`, `ret 8`), which also has a few
ability callers; it sends the unit message `0xD0164` to itself (nothing
else sends it) and clears the unit's orders (`0x67DF00`). A corpse that
finishes decaying is freed some other way. Experience: the hero ability
(`CUnit+0x1F0`, its unit at `CAbility+0x30`) keeps it at `+0x8C`
(`GetHeroXP` `0x687440`); `CAbilityHero::AddXP` (`0x528110`, `(const
float* xp, 5 ints)`, `ret 0x18`) is where kills (`0x527B90`), the natives
(through `CUnit` `0x680630`) and `0x52AE10` all arrive. Tome compares the
experience before and after it.

**Player resources and the time of day** have no single event function
(mining, spending, upkeep and bounties each change gold), so Tome reads
them every game tick while a mod listens. `GetPlayerState` (`0x201AD0`):
each state is a synced value at `CPlayerWar3+0x40+state*0x10`, read with
`0x056A40` (thiscall: the agent from the ids at `+8`, then `+0x78`); gold
and lumber (1, 2, and gathered, 15, 16) are stored times 10.
`GetFloatGameState` (`0x1FFF40`, cdecl) gives the time of day for state 2
(hours, as the real's bits in `eax`, like every native).

**Not mapped:** buffs (`CBuff` has some 200 subclasses; adding one goes
through ability code with no single entry found yet) and missiles.

`ReviveHero` fires revive start and finish; an altar's revive order
(`IssueImmediateOrderById(altar, 'Hpal')`) was refused in testing. `CUnit::GetHeroLevel` is `0x685150` (thiscall, no arguments). Items
(`CItem`) keep their type id at `+0x30`, and `GetUnitTypeName` names items
and research ids too (`GetItemName`, `GetObjectName`).

**Victory and defeat:** `RemovePlayer` (`0x210A40`) →
`CPlayerWar3::RemovePlayer(result)` (`0x269B00`, `ret 4`): only while the
player's state (`+0x270`) is 1, it stores the result (0 victory, 1 defeat, 2
tie, 3 neutral), sets the state to 2 and reports it to the net client
(`0x32CF60`); only while `+0x268` is set does it call `OnVictory`
(`0x262BC0`) or `OnDefeat` (`0x25FC80`), which melee victory doesn't. So
Tome hooks `RemovePlayer`.

### Saved games

| What | RVA | Notes |
|------|-----|-------|
| CGameWar3::SaveGame | `0x1E8630` | `int __fastcall(const char* path, int flag)`, writes the CSaveGame. Two callers: the `SaveGame` native (`0x211360`, path from `0x1FFFD0`: "Save\Profile<n>\CustomSaves\<map>\<name>", **no extension added**) and the network save handler (`0x26F4xx`, the Esc menu's Save Game: pauses with SetPaused(1, 0xFF, 1, 0, 0), fires `EVENT_GAME_SAVE` through `0x284760` on the handle manager, path from `0x2BFC90`). |
| Load request | `0x1E7630` | thiscall(path, ?, flag), `ret 0xC`, called by `LoadGame`, `ChangeLevel` and the in-game Load Game: a path ending in `.w3z` (`+0x32C` = 1) is a saved game, anything else a map; the path goes to the RCString at `+0x2F0`, which the next game's `CGameWar3` still has while it loads. `LoadGame("x")` with a name saved without `.w3z` loads nothing and returns to the main menu. |
| HandleManager::FireGameLoaded | `0x284740` | thiscall, no arguments: fires `EVENT_GAME_LOADED`. Called by the saved game loader (`0x1E3730`, at `0x1E3C0F`) once the state is read, before EnterGame. |

Saved games load inside a session (map script `LoadGame`): GAME_END,
SetScreen(1) (the map name is empty then), the loader, EnterGame. `-loadfile`
with a `.w3z` doesn't load it, and the Load Saved Game screen listed no saves
under Wine in testing (neither in `Save\Profile<n>` nor `Save\<name>`).

### Game objects (API version 16)

Read off the natives' own code on both builds (each native: a handle
converter, then a call): `tome_property` uses the callee, so it works for
objects the script has no handle for. The native tables were paired by
name from the `BindNative` calls in `InitNatives` (1,167 natives each).

| What | 1.27b | 1.26a | Notes |
|------|-------|-------|-------|
| CUnit::GetState | `0x687270` | `0x27AE90` | `float* __thiscall(CUnit*, float* out, int state)`, `ret 8`: 0 life, 1 max life, 2 mana, 3 max mana. GetUnitState `0x204050` / `0x3C5400` after checking state < 4. |
| CUnit::SetState | `0x6999E0` | `0x27AF20` | `void __thiscall(CUnit*, int state, const float*)`, `ret 8`. Only 0 and 2 do anything (max life and mana can't be set this way, as in JASS). SetUnitState `0x215710` / `0x3C5EA0`. |
| CUnit::GetMoveSpeed | `0x685310` | `0x26DE20` | `float* __thiscall(CUnit*, float* out)`, `ret 4`, through the move ability at `+0x1EC`. GetUnitMoveSpeed first checks the agent (below). |
| CUnit::SetMoveSpeed | `0x698AF0` | `0x26DE00` | `void __thiscall(CUnit*, const float*)`, `ret 4`; nothing without a move ability. |
| CUnit::GetLevel | `0x684830` | `0x29DCA0` | `int __thiscall(CUnit*)`: GetUnitLevel calls CUnit::GetHeroLevel for heroes (type id A-Z, not `UnitFlag.not_hero`) and this for the rest. |
| CUnit.vtable.GetPosition | `+0xB8` | `+0xB8` | `void* __thiscall(CUnit*)`, no arguments: the position object. |
| CPosition::Get | `0x058900` | `0x4743A0` | `float* __thiscall(pos, float out[3])`, `ret 4`: x, y, z. GetUnitX/Y. |
| CPosition::Set | `0x058810` | `0x4743C0` | `__thiscall(pos, const float xyz[3])`, `ret 4`, no pathing check. SetUnitX (`0x215900` / `0x3C5970`) reads the position, keeps y, sets z = 0 (`.data 0xD3C744` / `0xAAE470`, 0.0) and calls it on a fresh GetPosition. |
| CWidget.vtable.GetLife / SetLife | `+0x12C` / `+0x124` | same | `float*(float* out)` / `void(const float*)`, thiscall: GetWidgetLife and SetWidgetLife, for any widget. |
| CItem.charges | `+0x84` | `+0x84` | int (GetItemCharges). |
| CPlayerWar3::SetState | `0x1E98E0` | `0x3A0EB0` | `void __thiscall(CPlayerWar3*, int state, int value)`, `ret 8`. SetPlayerState passes states 15 and 16 (gathered gold and lumber) times 10 and the rest as they are. |
| ObjectLookup | `0x054530` | `0x03FA30` | (already mapped) The agent check every unit native makes: `fastcall(CAgent.id words)` returns a record whose `+0xC` is `'+agl'` and `+0x20` 0 for a live agent. |
| HandleManager.count | `+0x198` | `+0x198` | ids handed out; the table is the growable array at `+0x194` (count, then the entries pointer at `+0x19C`). A new id is the last free one (`free_count` `+0x188`, array `+0x18C`) or `first + count` (`0x282E30` / `0x430CF7`). Entry: 12 bytes, reference count at `+0`, object at `+4`. The generic lookup `0x285FE0` / `0x428050` checks only `id >= 0x100000`. |
| JassVM::Get | `0x90AB10` | `0x449D10` | `fastcall(int index)`: the VM (`CGameWar3.vm_index`, `+8`, is what CreateString passes). |
| JassVM.strings | `+0x2874` | `+0x2874` | The string table: length at `+4`, entries at `+8`, 16 bytes each (an RCString, then a reference count at `+0xC`; `0x91AF80` / `0x45D210`). A string result is an index; a string argument is a pointer to its entry. |

Second batch (same method, both builds):

| What | 1.27b | 1.26a | Notes |
|------|-------|-------|-------|
| FloatMultiply | `0x06F9C0` | `0x6EEE20` | `float* __fastcall(float* out, const float* a, const float* b)`, `ret 4`: a multiply in integer arithmetic. GetUnitFacing multiplies the position's radians by 180/pi (`.data 0xCD5468` on 1.27b; 1.26a's `0xAAE60C` is filled at startup), SetUnitFacing by pi/180. |
| CPosition.vtable.GetFacing | `+0x1C` | same | `float*(float* out)`: radians. |
| CPosition::SetFacing | `0x05A7A0` | `0x474830` | `__thiscall(pos, const float* radians, int flag)`, `ret 8`. SetUnitFacing passes 0, which turns the unit at its turn speed (seen in the game: 0.03 then 347.85 a moment later); 1 sets it at once (vtable `+0x58` on the agent). |
| CUnit.fly_height | `+0x208` | same | float (GetUnitFlyHeight). |
| CUnit::FindAbility | `0x48CB80` | `0x0787D0` | `__thiscall(unit, id, 0, 1, 1, 1)`, `ret 0x14`: the ability or NULL. An unlearned hero skill has none (SelectHeroSkill makes it). |
| CAbility.level | `+0x50` | same | level - 1. SetUnitAbilityLevel clamps to `[1, CAbility::GetMaxLevel]` (`0x415E00` / `0x02E570`) and calls vtable `+0x2E4` (up) or `+0x2E8` (down) once per level. |
| CUnit::GetAcquireRange / SetAcquireRange | `0x684150` / `0x697EA0` | `0x276A00` / `0x26D620` | `float*(float* out)`, `ret 4` / `void(const float*)`, `ret 4`; through the attack ability at `+0x1E8`. |
| CUnit::GetHeroXP | `0x687440` | `0x26EB30` | `int()`, through the hero ability at `+0x1F0` (0 without). |
| CUnit::GetHeroStr/Agi/Int | `0x686720`, `0x6841A0`, `0x684F20` | `0x2777B0`, `0x2778B0`, `0x277850` | `int(int include_bonuses)`, `ret 4`. |
| CUnit::SetHeroStr/Agi/Int | `0x6997C0`, `0x697EF0`, `0x698860` | `0x277950`, `0x2779B0`, `0x277980` | `void(int value, int permanent)`, `ret 8`; heroes only (they use `+0x1F0` unchecked). |

**Armor and attack** (no natives before 1.29): found by dumping units in the
game and matching `UnitBalance.slk`/`UnitWeapons.slk` (War3Patch.mpq),
the same offsets on both builds. `CUnit+0xE0` armor (float: Town Hall 5,
Peasant 0, a level 1 Mountain King 2.3), `+0xE4` armor type (defType order:
Peasant 1 medium, Town Hall 3 fortified, heroes 5); the info panel's
`CInfoPanelIconArmor` (vtable `0xABEC4C`, slot `+0x6C`, `0x3D4290`) gets
the armor and its bonus as two floats. The attack ability (`CUnit+0x1E8`,
NULL without an attack) holds both weapons side by side: base damage
`+0xA0`/`+0xA4` (dmgplus, plus a hero's primary attribute: 24 for the
Mountain King's 24 strength), dice `+0x88`/`+0x8C`, sides `+0x94`/`+0x98`,
cooldown `+0x158`/`+0x160`, range `+0x258`/`+0x260` (floats 8 bytes
apart), acquisition range `+0x244`. Peasant: 4 + 1d2 every 2.0 s at 90,
and its second weapon (harvesting) 0 + 1d1 every 1.1 s at 66. Tome reads
these but doesn't write them yet: base damage follows the hero's
attribute, so the game rewrites at least that one.

**Every unit, item and destructable.** GroupEnumUnitsOfPlayer (`0x204560`
/ `0x3C42C0`) ends in `0x22F150` / `0x3E7C30`, which calls ForEachAgent
(`0x04C410` / `0x46D1B0`, fastcall, `ret 8`) with the class's type tag
(`'+w3u'` from a two-instruction `mov eax, tag; ret`; `'+w3i'` items,
`'+w3d'` destructables; the same tags on both builds) and a callback. The
walker finds the class in the agent type registry (`.data 0xD3C82C`), runs
through its instance list (`[type+0x6C]+0x58`, next at `+8`, the agent at
`+0x54`), skips dying entries (`+0x20`) and checks each agent's record
(ObjectLookup, `'+agl'`, `+0x20`) before calling `int __fastcall(agent,
context)`; a callback returning 0 stops the walk (`neg; sbb; inc` makes
it the stop flag), non-zero goes on. With its last argument set it also
walks the types listed at `+0x50`, which reach other classes too: from
`'+w3i'` it found every destructable and unit as well. Item instances
are only found that way (`'+w3i'` alone lists none), so Tome walks items
and destructables with it and keeps the agents with the class's vtable;
units are walked as GroupEnumUnitsOfPlayer does (220 units, 2 items, 5,667
destructables in the e2e map). Players aren't agents of a listed class:
`CGameWar3::GetPlayer` gives every slot (all 16, empty ones too). **1.26a's
GetPlayer doesn't check the slot** (`mov eax, [ecx+eax*4+0x58]`; 1.27b's
compares it with `+0x54`): Tome checks `CGameWar3.player_count` itself.

**Handle converters check types.** Each handle type's converter (unit
`0x1EEF90`, player `0x1EDE10`, trigger `0x1EEE50`, gamecache `0x1ED2D0`,
item `0x1ED690`, group `0x1ED550`, timer `0x1EE810`... on 1.27b) calls the
generic lookup `0x285FE0` (no upper bound) and then a typed cast like
`0x1D73B0`: the class's tag (`'+w3u'`), the object's type (vtable `+0x1C`)
and an is-a test (`0x04CDD0`); a mismatch gives null, and the native does
nothing. A scan of every native's first call found the is-a test behind
the converters of 30 types; the natives whose first call doesn't reach it
take a non-handle first argument (a string, a number, or an integer type
like camerafield).

**Handle-typed integers.** `unitstate`, `playerstate`, `race`, `alliancetype`
and the other types a `Convert*` native makes from one integer (`(I)H<type>;`)
aren't in the handle table: `ConvertUnitState(2)` is 2. `jass_call` learns
them from the same BindNative calls and passes them through unchecked.

**Index handles.** `texttag`, `image` and `ubersplat` values are indices
of their own, not handle table ids (`CreateTextTag` returns 0 to 99).
Every native taking a texttag (13 on both builds, e.g. SetTextTagText
`0x214C00` / `0x3BC5D0`) starts with `cmp id, [count]; jae` (`.data
0xBDB0F0` / `0x9550C4`), and SaveTextTagHandle stores the id as an integer
(`0x211720` with type 2; the agent variant `0x211780` looks its value up),
so `jass_call` passes texttags unchecked too. The image and ubersplat
natives only refuse -1: SetImageColor (`0x213640`) ends in `0x759EF0`,
which writes `[[manager+0x2CC] + id*0xA0 + 0x48]` with no upper bound, so
`jass_call` keeps refusing them (they aren't live handles).

### Chat and pause

| What | RVA | Notes |
|------|-----|-------|
| CGameUI::DisplayChatMessage | `0x373470` | `void __thiscall(int player, const char* text, int recipient, float duration)`, `ret 0x10`. Shows a chat line with "[All]"-style prefix (recipient 0 `CHAT_RECIPIENT_ALL`, 1 allies, 2 observers or referees by `0x36E2B0`, else private) and the player's colored name (`0x36CE50`, `0x36CEB0`). Two callers in CGameUI's chat handling (`0x36EDEC` from the network, `0x36EFF6`); in single player the local player's own line goes through it. |
| CGameUI::SetPaused | `0x3719C0` | `void __thiscall(int pause, int player, int a3, int a4, int a5)`, `ret 0x14`. `PauseGame` (`0x20F4F0`): (pause, local player, 0, 1, 1); network pause/resume handlers `0x370900`/`0x370CB0`: (pause, `[event+0x15]`, 1, 0, 0), also for the Esc menu and the victory dialog in single player; `0x26F4BD`, `0x370961`, `0x370ADC` other cases (0xFF: no player). Writes `+0x258` = pause && a3 (a player's pause), and with a5 `+0x260` = pause (a trigger's), and a player's resume returns early while `+0x260` is set. Asks the net client (`0x32AFF0` pause, `0x32B080` resume: per-player state at `client + slot*0x304 + 0x278`, 4 playing, 5/6 paused) and shows `PAUSE_GAME_NOTIFY`/`RESUME_GAME_NOTIFY`. No other code in CGameUI writes `+0x258`/`+0x260`. With a4 = 1 it also resets the in-game UI (selection included). |

### The network client, the lobby and sync actions

Found for the multiplayer handshake and confirmed in LAN games between two
instances (2026-10-26): lobby chat to one player, the sender byte `+0x15` of
lobby events (a net id) and of sync actions (a player slot), NetIsHost,
NetGetPlayerName, the sync actions in games and replays. The net client is `0x06C180(0xD)->[+0x10]->[+0x8]`;
`+0x2288` is the game type (`'LOOP'` single player, `'NONE'`, `'BNET'`, ...;
`0x32BA40` returns it), `+0x610` is 1 during replays. Each local client has a
block at `client + index * 0x304`: `+0x278` its state (3 and up: in a game),
`+0x28C` the host flag, `+0x290` the player lists (`0x3299A0(id)`: ids below
`0x80` in one list, the rest in another), `+0x2DC` the local player's net id.

| What | RVA | Notes |
|------|-----|-------|
| CGameChatroom | vtable `0xAB0BC0` | The lobby (GlueScreen 11). OnEvent (slot 3, `0x2EF2D0`) switches on `[event+8]`: command 0 Start (`StartGameButton`), 1 Cancel (`0x2E6870`), 5 send chat (`0x2F9B50`), code 9 a chat line arrived. `+0x168` the net index every lobby net call takes, `+0x1FC` the chat (`ChatTextArea`), `+0x200` the chat edit box. |
| CGameChatroom::OnChatReceived | `0x2E6EC0` | `void __thiscall(event)`, `ret 4`. Lobby event `0x10` (registered for code 9 by `0x2FF2AC` through `0x32C570`): text at `event+0x18`, sender's net id at `+0x15`; formats "name: text" and adds it with CTextArea::AddText. The sender doesn't receive its own line; the send handler adds it locally. |
| CGameChatroom::StartGame | `0x2F9CE0` | `void __thiscall()`, plain `ret`. Host only (NetIsHost); checks the slot table (`0x29FEA0`: count at `+4`, 9-byte W3GS-style slots at `+8`: pid, download, status (2 occupied), computer, team (12 observers), color, race, AI, handicap) and shows a dialog through `0x2BC510` if the teams don't allow a start; otherwise starts the countdown (`0x32AD30`). |
| CTextArea::AddText | `0x1097A0` | `void __thiscall(const char* text)`, `ret 4`. Appends a line; color codes work. |
| CNetEventLobby::Init | `0x2C5EF0` | `void __thiscall(int type)`, `ret 4`. Lobby events are `0xB0000 | type` (vtable `0xAAA270`), type at `+0x14`, sender `+0x15`, text `+0x18` (0x100 bytes). Chat is type `0x10`. |
| NetSendLobbyEvent | `0x2C2610` | `void __fastcall(int recipient, event, int net_index)`, `ret 4`. One recipient; `0xFD` means every player (what the lobby chat sends). `0x2C2630` takes a list of ids; both end in the net client's `0x32CA60`, which serializes the event and sends it through the host. |
| NetGetPlayerName | `0x32B870` | `const char* __fastcall(int id, int net_index)`, NULL for an id without a player. |
| NetGetLocalPlayer | `0x32B2B0` | `uint8_t __fastcall(int net_index)` (in al; 0xFF without a client). |
| NetIsHost | `0x32C000` | `int __fastcall(int net_index)`: 1 for game type `'NONE'`, 0 once in a game, else block `+0x28C`. |
| NetIsReplay | `0x32C110` | `int()`: net client `+0x610 == 1`. |
| Action registration | `0x32C4D0` | `fastcall(cl = action byte, edx = handler, 0, 0)`: registers net event `0xA0000 | byte` with `0x32C490`. `0x271400` registers the handlers of actions `0x50`–`0x75`; `0x6BDD04` the unit orders `0x10`–`0x1E`; `0x37A794` the cheats `0x20`–`0x32`. `0x32C570` is the lobby (`0xB0000`) variant with an observer and a command code. |
| CNetEventSyncStoredInteger::Init | `0x26DDB0` | `void __thiscall()`, plain `ret`. Action `0x6B` (vtable `0xAA131C`, id `0xA006B`): game cache file name `+0x18`, mission key `+0x118`, key `+0x218` (0x100 bytes each), a value object at `+0x318` whose integer is at `+0x330`; 0x334 bytes. The native SyncStoredInteger (`0x2165D0`) sends nothing in replays, then `0x24AFB0`/`0x270C20` build it from the cache and send it. |
| NetSendAction | `0x26CE10` | `void __fastcall(action, int flags)`, plain `ret`. Writes the action byte and data into a data store and hands it to `0x32C920`, which schedules it for every player and the replay. The builders pass 0. `SyncValue::Destroy` (`0x221080`, thiscall on `action+0x318`) runs after it. |
| OnNetSyncStoredInteger | `0x26F7D0` | `int __fastcall(event)`, plain `ret`. Runs on every client and in replays: `0x24A420` stores the value, **creating** the game cache if no cache has that name (`0x2322B0(name, 1)`), so a client without Tome just keeps an unused cache. Sender at `+0x15`. |
| CPlayerWar3 controller | `+0x268` | mapcontrol (0 user), as GetPlayerController (`0x201710`) reads it. GetPlayerSlotState reads `+0x270` (`CPlayerWar3.game_state`). |
| Edition | `.data 0xCD52F0` | `Game.expansion`: 1 for The Frozen Throne, 0 for Reign of Chaos. `0x2DF4A0` turns it into the product code (`W3XP`/`WAR3`), found from the game name table `0xAA9874` ("Warcraft III: Reign of Chaos", "...: The Frozen Throne"), which `0x2DF470` indexes. `Frozen Throne.exe` is a launcher, so the program's name says nothing about it. Set once at startup by `0x046800` (called from `0x04515C`, before Mss32 loads `tome.mix`): 0 when the product it reads is `'WAR3'`, else 1; `-1` in the file until then. `war3.exe -classic` (what `Warcraft III.exe` passes) gives 0 on 1.27b and 1.26a (checked in game). |
| CPlayerWar3 race | `+0x260` | `GetPlayerRace` (`0x2017C0`, found from the native table's name string) maps the handle with `0x1EDE10` and returns this int, or 1 for no player: 1 human, 2 orc, 3 undead, 4 night elf. The player array's length is `CGameWar3+0x54` (`CGameWar3.player_count`). |

Net ids in a LAN lobby: the host had 128 and the joining player 129 (the
second of `0x3299A0`'s lists), so scan every byte, not 0–63.

**Who hosts** (Nov 2026; `NetGetPlayerNumber`). The game's router hands out
player numbers from 1, and a game is created with its creator in it: the
`CNetEventGameCreate` handler (`0x32DF40`) sets the host flag (`+0x28C`) and
adds the local player with the number the event carries (`+0x18`; the
single player path `0x32AF10` passes 1 itself). `CNetEventGameJoin`
(`0x32DFE0`) adds a joining player the same way without the flag, and
`CNetEventPlayerJoin` (`0x32E230`) the others. In the lobby, player number n
is net id `0x80 | (n - 1)` (`0x3245C0`, `ret 8`: entry `+0x18` the number,
`+0x19` the name (16 bytes), `+0x36` the net id). When the game starts the
entries move to the first list under their game slots (`0x32FF80` from
`0x32CF10`, which also moves the local id), keeping their numbers. So the
host is the player whose number is 1: net id 128 in the lobby, its slot in
the game. `NetGetPlayerNumber` (`0x32B5F0`, `fastcall(id, net_index)`, the
number in al, 0 for none) gives a net id's number, and `0x32B790` the
reverse (inlined on 1.26a; its `NetGetPlayerNumber` is `0x53ED90`). Lobby
slots (`0x29FEA0`'s table, `+0x18`: slots at `+8`, 9 bytes each) hold the
number in their first byte. A replay plays on net client 1 (`+0x610`), where
number 1 is the recorded game's host.

Checked in LAN games between two instances (`tools/e2e-multiplayer.sh`): the
host is 128 in the lobby on both sides, player 0 in the game on both sides,
and player 0 in the replay. The Battle.net provider (`NetProviderBNET`)
differs from the LAN one (`NetProviderLTCP`) only in the transport; player
numbers come from the shared router (`NetRouter.cpp`) and client code, and a
Battle.net game is hosted by its creator's game like a LAN one, so the host
is player 1 there too. Not checked by running a Battle.net game. A game
hosted by a bot (GHost-like hosts) numbers players its own way; Tome warns
when it hosts under another number, and clients answer every sender.

**Senders can't be faked.** A lobby line's sender (`CNetEventLobby.sender`,
`+0x15`) is what the host's router saw: the send path (`0x32CA60`) takes the
sender from the local client (`NetClient.local_id`, `+0x2DC`), and a client
that set it to 128 for one send (`TOME_DEBUG_LOBBY_AS`, as a modified client
could) still reached the host as 129 (checked in the LAN test, on 1.27b and
1.26a: 1.26a's client chain, block size and id offset are the same, through
`NetClient::Registry` `0x53F340`; on 1.27b it's `0x32AEE0`, a thunk that
calls `0x06C180` with `0xD`, the registry entry of the chain above: client
`[[entry + 0x10] + 0x8]`). Lines reach
other players through the host, so they get the same sender. A modified
host can still claim anything; it's the host.

### Esc menu (in games)

The in-game menu (F10, the Menu button) is `CEscMenu` (vtable `0xABDF34`)
with panels of their own classes: `CEscMenuMainPanel` (`0xABDAEC`),
`CEscMenuOptionsPanel` (`0xABDBFC`), `CEscMenuSaveGamePanel`. Their frames
come from `UI\FrameDef\UI\EscMenu*.fdf` and `EscMenuTemplates.fdf`
(`EscMenuButtonTemplate` 0.228 × 0.035, text fonts 0.011–0.015). They're
ordinary CFrames, so GetFrameByName finds them by their FDF names.

| What | RVA | Notes |
|------|-----|-------|
| CEscMenuMainPanel::Setup | `0x3B1890` | slot 55, `int __thiscall(int, int)`, `ret 8`. Runs once per game, while it loads. Stores MainPanel `+0x18C`, EndGamePanel `+0x1C4`, ConfirmQuitPanel `+0x1EC`, HelpPanel `+0x200`, TipsPanel `+0x214` with their sizes, and binds the buttons through `0x3B9100`: Pause 0, Save Game 1, Load Game 2, Options 3, Help 4, Tips 5, End Game 7, Return 8, Previous 9, Restart 10, Quit 11, Exit 12, Confirm Quit 13/14, Help OK 15, Tips OK/Back/Next 16–18. |
| CEscMenuMainPanel::OnEvent | `0x3BD470` | slot 3; jump table over those ids. Options goes to the options panel object at `+0x178` (`0x3CF230`); Return closes the menu (`0x3745F0` on CGameUI). |
| CEscMenuMainPanel::SetPanel | `0x3CF5C0` | `void __thiscall(int panel)`, `ret 4`. Panels: 0 none, 1 main, 2/6 End Game (6: "Resume play" text), 3/7 Confirm Quit, 4 Help, 5 Tips; current at `+0x168`. Returns at once if it's already current; otherwise hides all five, resizes the backdrop (`+0x17C`) with SetWidth/SetHeight to the new panel's stored size and shows it. Help OK calls SetPanel(1). |

Layout of the main panel: Pause at `TOP −0.067` (hidden in single player),
Save Game, Load Game and Options stacked 0.002 apart, then Help and Tips
(0.11 each) sharing a row under Options' left and right edges, End Game 0.039
below that row, Return at the bottom. Tome narrows Options to 0.11, puts its
Mods button in the right half, and re-anchors Tips and End Game.

After Esc closes the menu from the game's Help panel, the first F10 does
nothing and the second opens it (the game's own behavior; Tome handles Esc
in its own panel, so it doesn't happen there).

| What | RVA | Notes |
|------|-----|-------|
| CGameUI::SetMode | `0x374260` | `void __thiscall(void* mode, int push)`, `ret 8`. The current mode is at `+0x1B4`; with push = 1 the old one goes on a stack at `+0x1B8`, which `0x3745F0` pops (Return to Game). |
| CGameUI.esc_menu | `+0x228` | The CEscMenu, made by `0x39D7E0` once per game. Its main panel is at `+0x18`. |
| CEscMenu::Reset | `0x3CDD10` | `void __thiscall()`: options panel to state 0, main panel to SetPanel(1). F10 and the Menu button (`0x36FD99`) call it, then SetMode(esc_menu, 1). |

The menu's mode (`0x3CF230`, `void __thiscall(int mode)`, `ret 4`; the
shown panel object at `+0x14`) picks among its panel objects: 0 the main
panel (`+0x18`), 1 and 2 Save Game and Load Game (`+0x20`), 3 Options
(`CEscMenu.options_panel`, `+0x1C`). Reset's "state 0" is that mode 0.

The Options panel (`CEscMenuOptionsPanel`, `EscMenuOptionsPanel.fdf`) is a
list (Gameplay, Video, Sound, Network, Previous Menu) and four sub-panels
that share an OK/Cancel bar. Its OnEvent (slot 3, `0x3BD5A0`) switches on
the command: 0 Gameplay, 1 Sound, 2 Video and 3 Network load their panel's
controls and call SetPanel with 2, 4, 3 and 5; 4 is Previous Menu; 9 OK;
10 Cancel (Esc too, through the key handler `0x3BF4A0`); 5–8 are its
sliders and menus. Going back (`0x3C2D40`; Previous Menu, and the end of
OK and Cancel) goes from a sub-panel (2–5) to the list and from the list
to the Esc menu's main panel; for any other panel number it does nothing.
The box doesn't change size between them (0.288 × 0.384, `EscMenuBackdrop`).

| What | 1.27b | 1.26a | Notes |
|------|-------|-------|-------|
| CEscMenuOptionsPanel::SetPanel | `0x3CF8A0` | `0x344D60` | `void __thiscall(int panel)`, `ret 4`. 1 the list (`+0x174`), 2 Gameplay (`+0x18C`), 3 Video (`+0x1C0`), 4 Sound (`+0x1F4`), 5 Network (`+0x21C`); a sub-panel also shows the OK/Cancel bar (`+0x22C`). Hides the others (vtable `+0xD0`), shows its own (`+0xD4`), stores the number at `+0x168`; returns at once if it's already shown. Another number is only stored, which leaves room for a mod's panel; OK and Cancel do nothing for it, so the mod goes back itself (SetPanel(1)). |
| CEscMenuOptionsPanel::OnOK | `0x3C2810` | `0x36D600` | `int __thiscall()`, plain `ret`: stores the shown sub-panel's controls (2–5), then goes back (`0x3C2D40`). |
| CEscMenuOptionsPanel::OnCancel | `0x3BC9D0` | `0x36D660` | `int __thiscall()`, plain `ret`: puts the shown sub-panel's controls back (2–5), then goes back. Esc calls it too. |

### Edit boxes

`CEditBox` (vtable `0xA8C62C`; the glue screens' `CGlueEditBoxWar3` and the
chat's `CChatEditBox` derive from it). FDF type `EDITBOX` with an
`EditTextFrame` child (`StandardEditBoxTemplate`, `EscMenuEditBoxTemplate`).

| What | RVA | Notes |
|------|-----|-------|
| CEditBox::GetText | `0x102B50` | `const char* __thiscall()`: the buffer at `+0x1E4` (length `+0x1E8`, capacity check `0x102B60`). 57 callers. |
| CEditBox::SetText | `0x1041B0` | `void __thiscall(const char* text, int notify)`, `ret 8`: copies at most the maximum length (`+0x1F0`), cursor to the end (`+0x21C`), redraws (vtable `+0x108`); notify = 1 sends event `0x400B0065` (`0x102A40`). 72 callers. |

The backdrop's texture can't be set from code yet: `CBackdropFrame` (vtable
`0xA8B5AC`) has new slots `0xE4`–`0xF4` (`0xF7D00` loads a texture by name
and adds it through slot `0xE8`), not followed further.

### Sprite layers and animations

The chained panels on every glue screen are models, not FDF:
`UI\Glues\SpriteLayers\[Expansion\]TopLeftPanel[-Expansion].mdx` and
`TopRightPanel`. The glue manager creates them around `0x2E0BFA` from the skin
keys `GlueSpriteLayerTopLeft` / `GlueSpriteLayerTopRight` /
`GlueSpriteLayerBackground`. Each model has one animation set per screen
(list them with `tools/re/mdx_info.py`).

The model depends on the edition. The sprite layers (`0x2E0930`) format the
skin key with `%s_V%d` and `Game.expansion` (`GlueSpriteLayerTopLeft_V0` in
Reign of Chaos, `_V1` in The Frozen Throne), look it up with
`Skin::GetString` and pass the value to SetModel. `war3skins.txt` in
War3Patch.mpq (read in both editions) and War3xlocal.mpq has `_V0` =
`TopLeftPanel.mdl` and `_V1` = `Expansion\TopLeftPanel-Expansion.mdl`;
war3.mpq's only the plain key, `TopLeftPanel.mdl`. The values name `.mdl`
files; the game loads the `.mdx`. Both models have the same sequences and
timings (Options Morph 1000 ms, Morph Alternate 667 ms), so Tome's chains do
the same lookup (`chains_model()` in `mod_menu.cpp`, `ui::chains_skin_key`,
`ui::skin_model_path`) and fall back to the edition's model
(`ui::default_chains_model`) without a usable entry. The rest of Tome's UI
inherits the game's templates, which each edition reads from its own
archives; the glue popup's `bnet-dialoguebox-*` textures are the game's own
`DialogWar3` backdrop, only in war3.mpq, so the same in both.

| What | RVA | Notes |
|------|-----|-------|
| CSpriteFrame::SetModel | vtable `+0xE8` | `__thiscall(const char* path, -1, 0)` |
| Skin::GetString | `0x342220` (1.26a `0x31F530`) | `const char* __fastcall(const char* key, const char* section)`, plain `ret`: the map's `CustomSkin` section (table at `.data 0xD6A0C4`) first, then `section` (null: the current one, `0xD6A0D8`), then `Default`. Returns the game's own string or null; the hash tables return null while empty (mask -1), so it's safe before they load. |
| CSpriteFrame::GetInstance | `0x0F7060` | `void* __thiscall()`; returns `*[sprite+0x178]` when `[sprite+0x174]` is set |
| SetAnimation | `0x1A42B0` | `void __fastcall(instance, const int* tags, int count, int flags)`, `ret 8`. Picks the sequence matching the tag set. |
| Tag registration | `0x352A60` | Tag ids in order: birth 0, death 1, stand 2, morph 3, alternate 4, found 5, mainmenu 6, singleplayer 7, singleplayerskirmish 8, campaign 9, multiplayer `0xA`, multiplayerpregamechat `0xB`, viewreplay `0xC`, battlenetchatroom `0xD`, battlenetcustom `0xE`, battlenetcustomcreate `0xF`, battlenetwelcome `0x10`, battlenetamm `0x11`, battlenetadvancedoptions `0x12`, battlenetchannel `0x13`, battlenetprofile `0x14`, battlenetteamchat `0x15`, battlenetuserlist `0x16`, options `0x17`, realmselection `0x18`, maincancelpanel `0x19` (names at `.rdata` `0x6FAB7EA0` on). |
| Sprite layers | `0x2E0930` | `__thiscall` on the CGlueMgr: makes the background (`+0x190`), top-left (`+0x194`) and top-right (`+0x198`) CSpriteFrames once. The glue manager is `.data 0xD698EC` (`CGlueMgr.instance`), set after its constructor (`0x2C54E0`) returns; CGlueMgr::Get makes it on first use, and making it makes these sprites, which call SetAnimation: a SetAnimation hook must read the global, not call Get (it recursed into a stack overflow). |

Example: tags `{stand, options, alternate}` select "Options Stand Alternate"
(the lowered left panel). The top-right box is sized per screen: "Options
Stand" holds 3 buttons (from `0.110625`, 0.059 apart), "SinglePlayer Stand"
5 (from `0.15`, under a header plate), "SinglePlayerSkirmish" more,
"MainCancelPanel" only the bottom box, and RealmSelection has no box. The
`enhanced-options` mod swaps `options` for `singleplayer` in the top-right
sprite's SetAnimation calls to fit a fourth tab. "Options Morph" (1000 ms) lowers it, and "Options
Morph Alternate" (667 ms) raises it.

### Layout facts (from the game's FDFs)

- Main menu buttons: `StandardMenuButtonBaseBackdrop` (0.256 × 0.064) with a
  `GLUETEXTBUTTON` from `StandardButtonTemplate` at `TOPRIGHT −0.012, −0.0165`.
- Bottom box with **one** button (main menu Quit, Single Player Cancel): Quit
  backdrop at `BOTTOMRIGHT −0.015, 0.05`, width 0.24, button 0.168.
- Bottom box with **two** buttons (Options OK / Cancel): `BOTTOMRIGHT −0.015, 0.074`
  and `−0.015, 0.0225`.
- Options left-panel content starts at `TOPLEFT 0.025, −0.085` (title). The
  panel interior spans roughly x 0.025–0.41, y −0.085 to −0.50.
- The UI coordinate space is 0.8 × 0.6, stretched to the window.

### Graphics start-up and window mode

Graphics start-up (`0x44ED0`, plain `ret`) reads three DWORDs from
`HKCU\Software\Blizzard Entertainment\Warcraft III` with Storm's registry
helper (`0x7C6C6`): `Gfx Fullscreen` (default 1), `Gfx SwTNL` and `Gfx
OpenGL`. The command line overrides them: `-fullscreen` (id 8) forces
fullscreen and `-window` (id 9) windowed, through `CmdLine::IsSet`
(`0x64770`, `__fastcall(int id)`; the flags are Storm's `SCmdRegisterArgList`
table at `.rdata 0xA80C40`: d3d 0, opengl 1, swtnl 2, classic 3, crash 4,
datadir 5, loadfile 6, gametype 7, fullscreen 8, window 9). It then creates
the device (`0x136720`) and keeps it at `.data 0xD3C648` (`GxDevice.current`);
`CGxDevice.fullscreen` (`+0x9C`) says which mode it got. Neither changes
while the game runs: switching needs a restart (the `enhanced-options` mod
writes `Gfx Fullscreen` and offers one).

The fullscreen display mode: start-up's `0x449C0` reads options 0–4
(reswidth, resheight, colordepth, refreshrate, adapter) and `0x44A40` picks
the adapter's mode (`0x136940`'s list, 0x18 bytes each: adapter, ?, width,
height, bit depth, refresh) with the smallest sum of squared differences;
the refresh term counts only for modes that report one. `refreshrate`
defaults to 75, so a monitor with 60, 120 and 144 Hz modes gets 60. In
fullscreen Direct3D, BuildPresentParams takes the refresh rate from
`IDirect3D9::EnumAdapterModes(adapter +0x94, format, index +0x98)`. The
Options screen's own list (`0x44BB0`, `.data 0xD3C634`: count `+4`, entries
`+8` of 0x1C bytes, the mode index for `0x136AD0` at `+0x18`) is matched
without the refresh rate, and Save writes the matched entry's refresh rate
to option 4.

### Camera projection (for the widescreen patch)

Found from the six calls to `tan` (`_libm_sse2_tan_precise`, import
`0xA7C4AC`, thunk `0x78F7BC`); four are in small float wrappers.

| What | RVA | Notes |
|------|-----|-------|
| C44Matrix::Perspective | `0x126E30` | `void __thiscall(float* m, float fov, float aspect, float near, float far)`, `ret 0x10`. `t = tan(fov · 0.5 / sqrt(aspect² + 1))` (fov is spread over the diagonal), `m[0] = 1/(t·aspect)`, `m[5] = 1/t`, `m[10] = (far+near)/(far−near)`, `m[11] = 1`, `m[14] = −2·far·near/(far−near)`, the rest 0. Only caller: `0x1ADF40`. The off-center variant just before it (`ret 0x18`) has no `tan`. |
| CCamera::SetupProjection | `0x1ADF40` | `__thiscall(camera, rect*, unused)`, `ret 8` (its one caller, the wrapper at `0x1ADE90`, takes the rect in `edx` and one stack argument and pushes both; `ret 4`). fov from camera `+0x114` (1.0466 in a game), near `+0xCC` (331), far `+0xAC` (70000), aspect from the viewport rect, which is in UI units: 1.3333 for the world view in every window shape (measured at 1272 × 686). |
| CSpriteFrame::SetupCamera | `0x0F64E0` | `void __thiscall(frame)`, plain `ret`. Camera at `+0x140`, rect at `+0x12C`, then the wrapper `0x1ADE90`; without a camera it goes on at `0x0F6500`. Used for 3D models in UI frames and for the world frame (`CGameUI+0x3BC`, a CSpriteFrame too): by its own `0x3C1D6F` and before the screen/world conversions at `0x3911AB` and `0x3C09EC` (`CGameUI::Get(1)` then `+0x3BC`), which read the transforms back (`0x137270`, `0x137180`) for unit picking. |
| WorldView::SetupCamera | `0x3640F0` | `void __thiscall(this, float delta, const float* rect)`, `ret 8` (name guessed). If `this+0x38` is 1, first `0x1A2C10` on `+0x5C` with delta; then the wrapper `0x1ADE90` with the camera at `+0x34`. Once per frame, in games only, from the world frame's update (`0x385D55`, `this` = its `+0x19C`). |

Measured in a game at 16:9: `m[0] = 2.3098`, `m[5] = 3.0797` (ratio 0.75,
correct only for 4:3). Scaling `m[0]` by (4/3) / (window width / height)
after Perspective gives correct proportions, and picking the ground follows
(a move order at 90% of the width lands under the cursor). Unit picking
doesn't: it sets up a projection of its own (below), which kept the 4:3
one, so units answered clicks further from the middle of the screen than
they were drawn.

Every camera reaches Perspective through the wrapper `0x1ADE90`, which has
three callers. Counted per 2 s at 16:9 by hooking the wrapper:

| Caller | Where | Calls | What |
|--------|-------|-------|------|
| `0x3640F0` (WorldView::SetupCamera) | games | ~1 per frame | the world view |
| `0x0F64E0` | main menu and games | ~3 per frame in a game | 3D models in UI frames (camera at frame `+0x140`, rect at `+0x12C`): portrait, clock and the like |
| `0x78D0E0` | main menu only | ~1 per frame | the main menu's 3D background |

Only the world view may be widened: the others sit in 2D art that is still
stretched, so the widescreen patch scales only Perspective calls made while
WorldView::SetupCamera runs, or CSpriteFrame::SetupCamera for the world
frame (part of the `0x0F64E0` count above). That one uses the world
frame's rect, not WorldView's, but the same (4/3) / window aspect applies:
the UI space is stretched the same way whatever the rect.

### The 2D UI's coordinates (for the widescreen patch)

UI units ("DDC": 0.8 x 0.6) become the screen's 0..1 ("NDC") through eight
small functions that divide or multiply by the constants at `.rdata
0xAA9328` (0.8) and `0xAA9324` (0.6): `DDCToNDCRect` `0x8B37A0` and
`NDCToDDCRect` `0x8B3850` (fastcall `(in, out)`, rects `{left, bottom,
right, top}`, plain ret), `DDCToNDC` `0x8B37F0` and `NDCToDDC` `0x8B38A0`
(fastcall `(x_out, y_out, x, y)`, either out may be null, `ret 8`),
`DDCToNDCWidth` `0x8B3840` and `NDCToDDCWidth` `0x8B38F0` (stdcall, st0,
`ret 4`), and the heights' twins `0x8B3830`, `0x8B38E0`. Giving the x ones
the window's width in UI units (0.6 x aspect) makes the UI square and the
root the whole window; text and the mouse follow. The x ones read 0.8 through
one `[disp32]` operand each (two in the Rect forms; `DDCWidth` in the
mapping), which the widescreen patch points at its own float. About a dozen places read
0.8 inline (`0x2B5873`, `0x363AB6`, `0x374DA2`, `0x3D9536`, ...); the ones
checked clamp tooltips to the screen or pick a color, nothing that lays out.

- **Roots.** `CScreenFrame::UpdateScreenRect` (`0x11DFF0`) sets a screen
  frame's rect to `NDCToDDCRect({0, 0, 1, 1})` (`.rdata 0xA8DD90`) through
  the layout's vtable `+0xC`. It's slot `+0x4` of the *layout's* vtable
  (`this` = frame + `CFrame.layout`; `CScreenFrame` `0xA8DD24`, also
  `CGlueMgr` and `CGameUI`), which is `+0xEC` of the frame's primary vtable
  read past its end: calling it there passes the wrong `this`.
  `CScreenFrame::GetCurrent` (`0x11DD60`) returns the current screen frame
  (`.data 0xD47BB4`, set by `0x11E060`; the game UI sets itself at
  `0x36FFA9`).
- **Orthographic projections.** `C44Matrix::Ortho` (`0x126D50`, thiscall
  `(m, left, right, bottom, top, near, far)`, `ret 0x18`). The UI's own 2D
  pass builds `(0, 0.8, 0, 0.6, 0, 500)` at `0x193532` once per frame;
  `0x12A3F0` builds pixel-space ones (`0, width, 0, height`). A sprite
  frame's flat camera (`0x1ADDFC`, in CCamera::SetupProjection) is centered
  on its view: `CSpriteFrame.view` (`+0x12C`, `{x0, y0, height, width}`, the
  whole screen for menu models) gives `(-width/2, width/2, -0.3, 0.3)`, and
  the eye sits at the view's middle, so a model made for 0..0.8 lands at
  0..0.8 of a wider screen (the loading screen squeezed to the left).
  `(-width/2, 0.8 - width/2)` stretches it across as the game draws it.
  The cursor is such a frame too: `CCursorFrame` (vtable `0xA8DA14`, 1.26a
  `0x97063C`; `CCursorFrame.vtable`) fills the screen and draws its model
  at the pointer, in the same units as the rest of the UI. Stretched like a
  menu model, it was drawn `width / 0.8` times as far from the left edge as
  the pointer, so it's left as it is. A sprite frame's model is placed from
  the root's left edge, while its flat camera is centered on its own frame:
  a model in a frame that doesn't start at the root's left edge is drawn
  that much further right.
- **Clipping.** Every frame class's render virtual (slot `+0x24`,
  `0xECF70`) asks the frame for a clip (slot `+0xB8`; a sprite frame
  (`0x0F5E50`, 1.26a `0x606BD0`) clips when bit `0x8` of `+0xC` is set: its
  rect and the current clip's intersection). A frame that clips makes it the
  current clip (the render state's `+0x8..+0x14`, UI units) and the
  viewport: `DDCToNDCRect` into `.data 0xD478C0` (1.26a `0xACE644`) at
  `0xED0D7`, then `GxXformSetViewport` `0x1371F0` (1.26a `0x5265E0`;
  stdcall `(x0, x1, y0, y1, near, far)`, fractions of the window, `ret
  0x18`; the device's vtable `+0x4C`, 1.26a `+0x48`, whose base setter
  keeps them at `CGxDevice.viewport`, `+0xE4`; the device is
  `GxDevice.instance`, `.data 0xD52EB8`, 1.26a `0xACBD40`). After its children it puts the
  previous clip back the same way (`0xED2A1`). The roots clip, so a root's
  rect is the viewport its frames are drawn in. A flat sprite camera isn't
  clipped by its frame: its projection places the model in the frame's
  rect over the whole viewport (a loading screen's art: `(-width/2,
  width/2)` over the window). The glue screens' sprite layers
  (`CGlueMgr.sprite_*`) live as long as the glue manager, across screens.
- **Drawing a sprite frame.** Slot `+0x2C` draws: CSpriteFrame `0xF73E0`
  (1.26a `0x60EC00`) runs `CSpriteFrame::SetupCamera` and queues each model
  instance (`+0x174` count, `+0x178` array) into the scene at `+0x180`;
  CModelFrame (a MODEL frame, vtable `0xA8B454`) `0xF8200` does the same
  for its models (`+0x16C`, `+0x170`). Slot `+0x30`
  (`CSpriteFrame.vtable.Render`) renders them with the projection last set
  up: `CSpriteFrame::Render` `0xF72F0` (1.26a `0x60EA80`),
  `CModelFrame::Render` `0xF81B0` (1.26a `0x621700`). Calling SetupCamera
  and Render again draws the frame again with another camera (and, with
  GxXformSetViewport, in another part of the window). Alpha:
  `CFrame.alpha` (`+0xAC`, a byte) goes to what the frame draws through
  `CFrame.vtable.ApplyAlpha` (`+0xA4`: CSpriteFrame `0xF7260`, through
  `0x1ACAB0` with property 6; CModelFrame `0xF8150`, through `0x187E30`).
  A layout's rect: `CLayoutFrame.vtable.GetRect` (`+0x10`, `{bottom, left,
  top, right}`). The loading screen's art is a CSpriteFrame
  (`LoadingMeleeBackground`, `Load-Multiplayer-<race>.mdl`), the score
  screen's three CModelFrames, all filling the screen's frame.
- **Points.** A point is an object with a vtable: one relative to a frame
  (made by `0x11E3C0`) holds the relative `CLayoutFrame*` at `+4`, the
  relative point at `+8`, x and y at `+0xC`, `+0x10`; one relative to the
  screen (`0x11E380`, from `CLayoutFrame::SetPointAbsolute` `0x111590`,
  `thiscall(point, x, y, update)`) holds x at `+4`, so it can't be read as
  the other kind. The in-game callers of `0x111590`: the inventory cover
  (`0x39E94A`) and tooltips clamped to the screen (`0x392707`, `0x395AC2`,
  `0x395B23`, `0x3C948C`). The two kinds' vtables: `0xA8DDD0` relative to
  a frame (`CLayoutFrame.relative_point_vtable`), `0xA8DDBC` relative to
  the screen; 1.26a `0x97076C` (`0x629490`) and `0x970758` (`0x629460`).
  Slot `+0xC` returns which kind (the pool it goes back to, `0x11E300`).
- **The console** (`UI\FrameDef\UI\ConsoleUI.fdf`): nine `CSimpleTexture`
  regions anchored to the console, the race's tiles `ConsoleTexture01..04`
  (512 x 512, the fourth 64 wide, drawn 0.256 and 0.032 wide). Top: tile 1 at
  TOPLEFT 0, tile 2's first 0.087 at 0.256, its last 0.053 at TOPRIGHT
  -0.288, tile 3 at -0.032, tile 4 at 0. Bottom: tile 1 at BOTTOMLEFT 0, tile
  2's lower part (from v 0.4140625, 0.15 high) at 0.256, tile 3 at BOTTOMRIGHT
  -0.032, tile 4 at 0, all others from v 0.3125, 0.176 high. A texture's
  coordinates are at `+0xC4`, eight floats (u, v of each corner). In the
  art: tile 1 is the minimap's box and buttons, a pillar, and the portrait's
  arch from u 0.82; tile 2 the arch's end and the info panel's opening; tile
  3 the inventory's six slots (to u 0.31), wall and the command card's
  frame. A width of 0, or clearing a region's points, doesn't stop it
  drawing where it was; moving it does.
- **Where the console's parts hang** (at 4:3): the command card at the
  console's BOTTOMRIGHT -0.0074; the info panel (`CInfoBar`) at its
  BOTTOMLEFT 0.3094, the inventory's buttons on the info panel's TOPRIGHT
  +0.0188; the inventory's cover (`CInfoPanel.inventory_cover`, `+0x14C`,
  0.128 x 0.175) with its BOTTOMRIGHT at x 0.6 of the screen; the life and
  mana text (made at `0x3D9128` with the first selection, the font string at
  `+0x240` of its owner) at the console's BOTTOMLEFT (0.2145, 0.0283); the
  minimap's buttons at its BOTTOMLEFT 0.1544; the portrait (`CPortraitButton`)
  at its layer's BOTTOMLEFT 0.2075; the clock (`CTimeOfDayIndicator`) at its
  layer's BOTTOMLEFT (0, 0), its model drawn at the top middle from there; the
  clock's cover at its layer's TOP; the chat box from BOTTOMLEFT 0.2 to
  BOTTOMRIGHT -0.2; two strings at the resource bar's TOPRIGHT -0.417 and
  -0.3613 (under the clock).
- **Menus** are full-screen sprite models (`MainMenu.fdf`'s `MainMenuFrame`
  fills the root; the panels, chains and background are models in frames
  that fill it) with frames on them; the logo and the buttons' faces are
  models with their own extent. On the loading screen (`Loading.fdf`) the
  background and the bar (`LoadingBar`, held by its BOTTOMLEFT only) are
  such models.

## Crash handler

game.dll has its own crash handling, found from its import of
`SetUnhandledExceptionFilter` (IAT slot `0xA7C348`, three calls):

- `CrashHandler::Install` (`0x0199F0`, fastcall, `ecx` = the game folder)
  stores the handler `0x019AB0` at .data `0xD78520` and sets
  `CrashHandler::Filter` (`0x03B590`) as the unhandled-exception filter,
  keeping the previous one at .data `0xD78524`. Its one caller is the
  game's main function (`0x045080`, at `0x0450F6`), early, before the
  sound start-up that loads `tome.mix`: when Tome attaches, the game's
  filter is the current one (seen at run time: Tome's `SetUnhandledExceptionFilter`
  returns `game.dll+0x3b590`, and the e2e test checks it).
- `CrashHandler::Filter` calls the handler and maps its result to
  `EXCEPTION_*`; the handler writes the game's error log and minidump and
  starts `BlizzardError.exe` ("The application encountered an unexpected
  error", with a report ID). Nothing else references the filter: no
  `__except` in the main loop uses it, so a fault reaches it only unhandled.
- `CrashHandler::Remove` (`0x03B670`) puts the previous filter back.
- Storm.dll imports `SetUnhandledExceptionFilter` too and can write
  minidumps (`dbghelp.dll`); not looked at further.

Tome's crash reports (`architecture.md`, "Crash reports") put a filter in
front of the game's and call it afterwards, so these aren't hooked; they're
mapped for reference.

## Storm

game.dll imports Storm **by ordinal** only. Those Tome uses, all checked against
call sites in game.dll:

| Ordinal | Function |
|---------|----------|
| 252 | SFileCloseArchive(h). Unlinks the archive from the search order, then drops a reference (`+0x148`); each open file holds one too (released in SFileCloseFile's destructor), so the archive is freed only after its last file closes. |
| 253 | SFileCloseFile(h) |
| 265 | SFileGetFileSize(h, DWORD* high) |
| 266 | SFileOpenArchive(name, priority, flags, HANDLE*). Higher wins. The game's calls (through the import thunk at `0x07C6A2`): 0–3 for the base MPQs, **8 for War3Patch.mpq**, 13 and 14 (`0x2EC8BF`, `0x78C487`, likely campaign archives) and 16 for maps. Tome's generated overlay uses 15, mod assets 12. Order among equal priorities isn't known. |
| 268 | SFileOpenFileEx(mpq=0, name, scope=0, HANDLE*) |
| 269 | SFileReadFile(h, buf, size, DWORD* read, overlapped) |
| 401 / 403 | SMemAlloc / SMemFree |
| 501 / 506 / 507 | SStrCopy / SStrLen / SStrDup |
| 578 | SStrPrintf |

## Map downloads (for the lan-map-guard patch)

Found from the string `Maps\Download\` (Nov 2026). The map transfer is a
small file-transfer service in the network library: `0x32BDB0` registers it
through `0x96A680` → `0x97B820` with the extension list `.w3m;.w3x`
(`+0x2DA`), the search folder `Maps\` (`+0x3DE`) and the download folder
`Maps\Download\` (`+0x4E2`), each 0x104 bytes, guarded by a
`CRITICAL_SECTION` at `+0x2C2` (`0x956D0`/`0x96320` are Enter/Leave). A
transfer record (`0x987730`) has the path at `+0xC`, the local path found or
written at `+0x110`, the size at `+0x214`, the checksum at `+0x21C`, the
buffer at `+0x238` and the bytes received at `+0x23C`.

| What | 1.27b | 1.26a | Notes |
|------|-------|-------|-------|
| MapTransfer::IsValidName | `0x979000` | `0x656D70` | `int __thiscall(this, const char* name)`, `ret 4`. 0 if `0x91F00` says the path is absolute (`/` first, `X:`, or `\\`), if it has one of `*/:><\|&+^?"` (`0x91EC0`), if its base name (`0x91D30`: after the last `\` or `/`) has `\/*/:><\|&+^?"` or is a device (`0x91B60`: the part before the last dot against `nul con prn aux com1-9 lpt1-9`, table at `.data 0xCD6EF8`), or if its extension isn't in the list (`0x9788F0`). `..` passes. |
| Client: start a transfer | `0x989260` | | Parses the host's message (`0x97C750`: path, size up to `0x8000000`, checksums), calls IsValidName on the path, and makes a transfer record with it. |
| Host: offer a file | `0x985280` | | Calls IsValidName on the host's own path, `0x978D80` reads the file. |
| Find the map locally | `0x9789E0` | | Game folder (`0x91C20`) + the path as sent, checked by size and checksum (`0x978F20`, through Storm's `SFileOpenFileEx`); then the base name searched under each folder in `+0x3DE` (`0x978CE0`). |
| Write the download | `0x979090` | | Game folder + `Maps\Download\` + the base name of the path (`0x91D30`), `CreateFile(GENERIC_WRITE, CREATE_ALWAYS)` (`0x93770`). Called from the last part's handler (`0x988276`) once the size and checksum match. |

So a host's path is used as sent only for the first lookup; the written file
always lands in `Maps\Download\` under its base name.

## BLP textures (for the BLP guard)

The game decodes BLP1 textures in one place: `CBLPFile::Open` (RVA
`0x8E9790`, `int __thiscall(this, const char* name, void* context)`, `ret 8`;
`0x9F` bytes to the `int3` padding, its mapping's `size`; 1.26a `0x78A800`,
`0x9E`).
It loads the whole file, stores the buffer pointer at `this+0x4A4` and the
file length at `this+0x4A8`, copies a fixed `0x127`-dword header to `this+8`,
then checks the magic (`"BLP1"` = `0x31504C42`). On a wrong magic it returns 0
with the buffer still loaded; the three callers (`0x1B18AE`, `0x1B35C2`,
`0x70DC0D`) treat 0 as "not a BLP" and don't decode. The mip/JPEG decode
happens later, in the "get mip" helpers (`0x8E9860` / `0x8E98B0`), which read
the per-mip span straight from the buffer.

BLP1 header layout (confirmed against the loader; byte offsets from the file
start, i.e. from `this+8`): `0x00` magic, `0x04` compression (1 = paletted;
the mip decoder takes the JPEG path for every other value), `0x08` flags, `0x0C` width (read at `this+0x14`), `0x10` height
(`this+0x18`), `0x18` hasMipmaps (`this+0x20`: 0 → one mip, else the full
chain from the dimensions), `0x1C` `mipOffsets[16]`, `0x5C` `mipSizes[16]`,
`0x9C` the JPEG-header size (JPEG) or the 256-entry palette (paletted) — the
`0x9C` boundary is confirmed by the loader reading mip data from `this+0xA4`.
The JPEG decoder copies the JPEG header out of the in-object header copy
(`this+0xA8`), not out of the file, so a header longer than 1020 bytes reads
past that copy; the paletted decoder ignores `mipSizes` and reads one index
byte per pixel plus the alpha bits (`0x08`: 1, 4 or 8) from the mip offset.
The loader never checks a mip offset/size or the JPEG-header size against the
file length; `docs/known-bugs.md`, "BLP texture loader", and
`loader/src/patches/blp_guard.cpp`.

## Sound (for the ambient-sound patch)

The game plays through Miles (`Mss32.dll`). Startup (`0x357600`, the sound
setup) reads the Sound options from the registry: the option table at
`.rdata 0xA7F000` lists them in order (16-byte rows; index 0x1A `music`,
0x1B `musicvolume`, 0x1C `sfx`, 0x1D `sfxvolume`, 0x1E `ambient`, 0x1F
`movement`, 0x20 `unit`, 0x21 `subtitles`, 0x22 `nomidi`, 0x23
`softwaremidi`, 0x25 `donotusewaveout`), and the option setter `0x0403E0`
switches on the index. The enabled categories are a flags word at
`.data 0xCE5518`: `0x8` sound effects, `0x10` movement, `0x20` unit sounds,
`0x200` music, `0x400` ambient (`0x3571A0` sets bits, `0x357110` clears them;
clearing `0x8` also clears `0x430`). Map loading clears `0x8` while it runs
(`0x1D8F50`/`0x1DB6D0` around `0x1E5A10`) and sounds started meanwhile are
resumed when it's set again.

The MIDI driver is opened by `0x07FA60` (from `0x07F8B0`, unless the `nomidi`
flag 4 is set): `AIL_open_XMIDI_driver`, with the null driver
(`AIL_OPEN_XMIDI_NULL_DRIVER`) when flag 8 is set, then a DLS synthesizer
(S3BASE.DLL, VORT_DLS.DLL, else Miles' software one on the digital driver).
1.27b always sets flag 8 (`or eax, 8` at `0x357712`; `softwaremidi` is read
and ignored), so MIDI always goes through the software synthesizer. Handles:
digital driver `.data 0xD3CEE8`, MIDI driver `0xD3CEF0`, DLS `0xD3CEF4`;
all three are open under Wine.

**Ambient sound (1.27a "disabled while a MIDI issue is being resolved"):**
the day and night ambience is MIDI: `UI\SoundInfo\MIDISounds.slk` labels
(`LordaeronSummerDay` …) name a `.mid` and a `.dls` bank under
`Sound\Ambient\`. Blizzard.j's `SetAmbientDaySound`/`SetAmbientNightSound`
(unchanged in War3Patch.mpq) call `CreateMIDISound(label, 20, 20)`. The
native (`0x1FBE40`) makes a `CSound` and calls `CSound::InitMIDI`
(`0x2385B0`), which in 1.27b only forwards to `0x238450` (label, looping 1,
0, 0, fade in, fade out, 0): a plain label sound, made by `0x356DB0`, whose
lookup (`0x343A30`) searches only the WAV label tables (`.data 0xD6A54C`,
`0xD6A514`). The MIDI labels live in their own table (`0xD6A64C`, filled by
`0x34CDE0`, looked up by `0x341D60`), so the result is NULL, `CSound.sound`
(`+0x24`) stays empty and StartSound does nothing. Measured: the call
returned NULL for every ambient label, and the game's audio stream was
silent with music off.

The MIDI path is otherwise intact: `CreateMIDISnd` (`0x356C90`, fastcall
`(label, open_arg, fade_in, fade_out, category)`, `ret 0xC`; `open_arg`
in edx goes on to the sequence open `0x081B70`, the saved-game loader passes
0) makes the MIDI object;
`CSound.midi` (`+0x28`) holds it; StartSound (`0x240640`) plays `+0x24` with
`0x359130` or else `+0x28` with `0x359070` (which loads the DLS bank,
`0x34CB80` → `0x0821C0`, and refuses with "Midi sound disabled." while the
flags lack `0x8`, or `0x400` for a sound of category `0x8`); the release
(`0x23B000`) frees `+0x28` with `0x357A00`; saved games restore it
(`0x23C95B` → `0x357E20`, which calls `0x356C90`). The ambient-sound patch
(`loader/src/patches/ambient_sound.cpp`) makes `CSound::InitMIDI` call
`CreateMIDISnd` with category `0x8` (`SoundCategory.ambient`) instead;
measured, the ambience plays (stream RMS ~170 with music off, 0 without the
patch) and the Ambient sounds option still silences it.

### Running the compiler outside a game (not working yet)

For a deterministic test of the memhack guard on the real compiler, from a
`game.dll` loaded in a test process but never run (2026-10-29):

| What | RVA | Notes |
|------|-----|-------|
| Script syntax check | `0x7199D0` | `__fastcall(script, name)` plus one stack argument (`ret 4`; 0 picks `Blizzard.j`, otherwise `common.ai`). Calls `0x713450`, loads `common.j` and the second file by name through `0x713390` (Storm), then `Jass::compile` with the list below and the callback `0x727760`. Looks like the World Editor's check. |
| Compile file list | (stack) | `Jass::compile`'s `context`: (name, text) pairs ending in two nulls, e.g. `{"common.j", text, "blizzard.j", text, name, script, 0, 0}`; `a0` 0, `a1` 1 here. |
| Error callback | `0x727760` | `__fastcall(message, line)` plus three stack arguments (`ret 0xC`): an int stored at `+4` and a text copied to `+0x10C` of a `0x210`-byte record appended to the array at `.data 0xD720D0` (count `0xD720D4`, zeroed before each check). |
| Checker natives | `0x713450` | Once (flag `.data 0xD71A00`): binds every common.j native through `BindNative` to a stub returning 0 (`0x727800`), so declarations resolve. This is the "second native table" from the roadmap's open questions. |
| Per-thread instances | get `0x06C180`, set `0x06C1E0` | `__fastcall(index[, value])` on the calling thread's table (`.data 0xD3CB98`, made by `0x06C1B0`, set for a thread by `0x06C1D0`). Index 5 is the JASS engine, 0xD the net client (see the network section), 0xE another. |

Calling `0x713450` in a fresh process crashes in `BindNative`'s insert
(`0x90C730` → `0x90F176` with `ecx` null): `0x06C180(5)` returns 0, since
no thread table or JASS engine exists before the game starts. The only
store into slot 5 found is the clear at `0x90C850`; whatever creates the
engine sets it some other way (not a direct `mov [eax+0x14]` after the
table lookup). Next step: find the engine's constructor (its vtable's
deleting destructor is called from `0x90C850`) and make a thread table
with it in slot 5 before compiling.

## blizzard.ax (for the movie-frame-length patch)

The movies (`Movies\*.mpq`, AVIs of `BLZ0` video and MP3 sound) are decoded
by the install's `blizzard.ax`, a DirectShow transform filter (a DivX
decoder built on the base classes' `CTransformFilter`). `war3.exe` loads it
and calls its `DllRegisterServer` at startup; DirectShow then loads it by
CLSID from the game folder. Its addresses are in `mappings/blizzard.ax.json`
(`"module": "blizzard.ax"`, `BlzFilter.*`), checked when the patch loads it
and by a unit test against the local file.

- Build: PE timestamp `0x3CDC61D0` (May 2002), SizeOfImage `0x96000`, fixed
  base `0x1C400000` (with relocations), 612,328 bytes. Unchanged in 1.27b.
- Filter vtable (`.data1`, RVA `0x623C0` on): Transform `0x4020`,
  CheckInputType `0x4540`, CheckTransform `0x4680`, **DecideBufferSize
  `0x5410`** (slot at RVA `0x623F0`), GetMediaType `0x47C0`, SetMediaType
  `0x34D0` (slot `0x62404`).
- Fields (filter `this`): input pin `+0x8C`, movie width `+0xEC`, height
  `+0xF0` (from the input's VIDEOINFOHEADER), output format code `+0xF4`
  (SetMediaType, and Transform on a sample that changes the type: RGB32 4, RGB24 6, RGB555 8, RGB565 `0xA`, YUY2 1, UYVY 2,
  YV12 and I420 `0xD`), a flag at `+0x16C` (2 buffers instead of 1), the
  output type's VIDEOINFOHEADER at `+0x1B8` (its biWidth at `+0x1EC`: the
  row width in pixels Transform decodes with, `0x404498`; the movie's width
  rounded up to 32 if it's 0).
- DecideBufferSize (`HRESULT __thiscall(this, IMemAllocator*,
  ALLOCATOR_PROPERTIES*)`, `ret 8`): `cbBuffer = width * 4 * height`
  whatever `+0xF4` says, then `SetProperties`. Transform decodes into the
  output sample and calls only `SetPreroll(FALSE)` on it, never
  `SetActualDataLength`, so a frame's length is the buffer's. A renderer that copies the sample's
  length (Wine's VMR7) overruns a YUY2 surface by half.
- Under GE-Proton11-11 (winedmo splitter) the graph is splitter → this
  filter → VMR7, connected as YUY2 800x368. Under GE-Proton10-34 Wine's AVI
  splitter decodes `BLZ0` itself (YV12 to the renderer) and the filter
  isn't used for the picture. Seen with `WINEDEBUG=+quartz,+strmbase`.
- Movie::Play (game.dll `0x13D520`, `ret 8`) calls Movie::SetDisplayMode
  first, before any DirectShow call: the patch's hook point.

## Methods and tools

- **Disassembly:** `objdump -d -M intel --start-address=0x6f… --stop-address=0x6f… input/1.27b/game.dll`.
  For repeated searches, dump `.text` once to a scratch file and grep it.
- **String → code:** `python3 -I tools/re/xrefs.py input/1.27b/game.dll CreditsButton`.
  Shows `push`/`mov ecx` uses and data tables. Strings used as FDF frame
  names lead straight to the code that looks those frames up.
- **Class → vtable:** `python3 -I tools/re/rtti.py input/1.27b/game.dll CTextFrame --base CFrame`.
  Prints the overridden slots, which narrows down where a class's own code is.
- **Byte signatures:** `python3 -I tools/re/signatures.py input/1.27b/game.dll mappings/1.27b.json [--write]`.
  Prints each function's signature (relocated bytes masked as `??`), compares
  it with the file and, with `--write`, fills in `bytes`. Exits non-zero if a
  stored signature differs.
- **Model animations:** extract with `tome-mpq`, then `python3 -I tools/re/mdx_info.py file.mdx`.
- **MPQ contents:** `wine build/tools/mpq/tome-mpq.exe list|extract|put …` (build with `-DTOME_BUILD_DEV_TOOLS=ON`).
- **Runtime checks:** log values from inside the game (e.g. frame flag words
  next to a known-visible sibling, which is how Show and Hide were found to be
  reversed), and take screenshots with `tools/game.sh shot`. Use
  `TOME_DEBUG_CLICKS` to click through UI without a human.
- **Probing call order:** add candidate functions to the mapping file, run
  `signatures.py --write`, and hook them from a throwaway mod that logs
  arguments and a per-second call count (`tome_hook` BEFORE/AFTER). One game
  launch with `TOME_DEBUG_CLICKS` (including `type:` lines for the chat)
  then shows which functions run, how often and in what order. That's how
  the event hooks were chosen.
- **game.dll in a test process:** `LoadLibrary` of the install's `game.dll`
  (with `SetDllDirectory` on its folder) works under Wine in about 0.1 s;
  its imports load and nothing runs, so Tome's patches can be applied to
  the real code and pure functions called (`tests/game/`). `Mss32.dll`
  only loads `.mix` files once the game opens its sound.
- **Crash reports:** the game writes them under the Wine prefix in
  `AppData/Local/Temp/BlizzardError/<date>/Crash.txt`.
- Useful tricks that worked:
  - Count callers of candidate functions (SetPoint has 347).
  - Find setters by the struct field they write (SetText writes `+0x1E8`).
  - Read the RTTI pointer just before a vtable to name its class.
  - Follow a skin key string (`GlueSpriteLayerTopLeft`) to the code that uses the model.

## Pitfalls already hit

- **Reloading all of FrameDef.toc after startup** (LoadFrameDefToc) clears
  tables that already-created frames point into, and the next map load
  crashed. Load additional files with LoadTocFiles instead.
- **Redefining an existing frame name doesn't replace it.** Define new names
  and create them with CreateFrame.
- **Show and Hide are easy to swap.** Verify flags at runtime.
- **Opening UI from outside a click** works, but only after the menu has
  finished appearing (about 3 s after Setup). Setup-time visibility gets
  overridden.
- **JASS string literals need doubled backslashes** (`"UI\\FrameDef\\x.toc"`).
  `\F` is an invalid escape: the map script then fails to compile and the
  game runs the map without it, without any error.
- **A wrong signature in the mapping corrupts the stack when hooked.** The
  hook pops what the layout says, so a `thiscall` with two stack arguments
  mapped with one (`CGameWar3::Reset`, `ret 8`) crashed the game. Check
  every function's `ret N` against the signature before hooking it.
- **Two live frames with the same name and context crash the game** when
  their screen is destroyed (inside GetFrameByName, during the teardown).
  Recreating a hidden frame after a mod's disable and enable did it;
  `frame_create` and `BlzCreateFrame` now refuse a name and context that
  exists.
- **A clicked button keeps the keyboard focus in games:** Enter then presses
  it again instead of opening the chat box. Tome takes the focus back after
  a mod's in-game click (`CFrame::SetFocus(0)`).
- **The same number can mean different things:** `0x17` is also an event id elsewhere; `0x1B9` appears in vector maths.
