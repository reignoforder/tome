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
  `0x115`): writes the saved descriptors back, newest first.
- `Jass::compile` `0x9146F0` (BEFORE and AFTER): drops the list. A script
  that stops inside a function never reaches its `endfunction`, and the next
  compile's first one would write into the old compile's freed symbols. This
  is a safety net. In game, the parser recovers from a syntax error or a
  missing `endfunction` and still reaches a later `endfunction`, and a script
  that ends inside a function crashes 1.27b itself, with or without Tome.

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

There is no common glue menu class in RTTI: every screen derives from
CFrame directly.

### Control clicks and observers

`CObserver` (vtable `0xA84314`, 12 bytes: vtable, reference count at `+4`,
registration list at `+8`; constructor `0x071750`) receives events through
slot 3, `int __thiscall OnEvent(CEvent*)`. Slot 0 drops the last reference
(calls slot 1, the deleting destructor, which frees with Storm).

| What | RVA | Notes |
|------|-----|-------|
| CControl::SetListener | `0x112D80` | `void __thiscall(int command, CObserver* listener, int enable)`, `ret 0xC`. Calls the control's vtable `+0x08`(event `0x40090064`, command, listener): a registration node holding a reference to the listener. |
| CControl::Click | `0x112100` | `void __thiscall(int button)`, `ret 4`. Mapped. Returns at once unless the control is enabled (`0x072190` with `0x40090064`); fills the event at `0xCE3BA8` (id `0x40090064` at `+8`, the button at `+0x10`, the control at `+0x14`) and dispatches it through the control's vtable `+0x10`. The listener's OnEvent sees its registration's command at `event+8`. Mouse paths pass the mouse event's button, a shortcut path (`0x0FEA8F`) passes 1. Tome clicks the Esc menu's `ConfirmQuitQuitButton` with it to restart from a game (closing the window there asks Confirm Exit first). |

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
Wine (the separate `fps-unlock` mod is built on it):

| What | RVA | Notes |
|------|-----|-------|
| Main loop | `0x07B1C0` | `int __stdcall(int)`, `ret 4`: a scheduler of timed tasks per thread; on the main thread `0x07A1C0` calls it with 1 (seen in a backtrace). Drawing a frame is one of the main thread's tasks. It waits for the next task's time through its own wait (`0x096820`), not `Sleep`, and measures every deadline with GetTickCount (the thunk `0x092A10`). |
| Scheduler::SetPeriod | `0x073630` | `void __fastcall(int period_ms, void* context)`, plain `ret`. Stores the loop's period at `+0x48` of the record for `context` (0: the calling thread's, through `0x06C180`). The main thread's period spaces its frames. |
| Options::Get | `0x040ED0` | `void* __cdecl()`: the options object (`.data 0xD3C5F0`), made on first use. Options are numbered; their table is 16 bytes per id from `.rdata 0xA7EFE8`: registry name, ?, type, default text. |
| Options::Apply | `0x040310` | `int __thiscall(int option)`, `ret 4`: applies an option's stored value through the switch at `0x0403E0` (`ret 8`, id − 5 indexes `0x04098C`). Startup applies a list of them (`0x043ED0`). |
| maxfps | option `0x16` | Video\maxfps in the registry, default 200, also registered by name by `0x37AA70` with the callback `0x379E20` (store, then apply). Its case (`0x040607`) calls Scheduler::SetPeriod((int)(1000.0 / maxfps), 0). maxfps 60 gave 64.5 fps, 200 gave ~218, 1000 gave ~430 (vsync off, menu). |
| GetTickCount | import slot `0xA7C148` | 118 calls through the thunk `0x092A10`, a few `call [slot]`. Windows advances it every 15.6 ms; frames can't come faster than that (Wine: 1 ms). Rounding it down to 16 ms steps under Wine dropped maxfps 200 to 62.5 fps, the classic ~64 cap. The net clock `0x092A20` uses QueryPerformanceCounter instead when `.data 0xD3D5C4` is set, else GetTickCount; the net client subtracts readings unsigned and caps its bank at 4000 ms (`0x32D812`), so a clock that steps back would run the game up to 4 s ahead. |
| CGxDeviceD3d::BuildPresentParams | `0x13FED0` | `void __thiscall(D3DPRESENT_PARAMETERS*)`, `ret 4`. Callers: CreateDevice (`0x13FC20`, IDirect3D9 vtable `+0x40`) and the reset after a lost device (`0x1404DD`, IDirect3DDevice9 `+0x40`). PresentationInterval (`+0x34`) is always 0 (DEFAULT): vsync on. OpenGL never calls `wglSwapIntervalEXT`, so the driver decides. |
| CGxDeviceD3d::CanDraw | `0x140410` | `bool __thiscall(device)`, plain `ret`. Device vtable `+0x3C`, before every frame. TestCooperativeLevel; on `D3DERR_DEVICENOTRESET` (or a resize pending at `+0x730`) ReleaseForReset(0), BuildPresentParams, `IDirect3DDevice9::Reset` (`+0x40`), RestoreState. False while the device is lost. |
| CGxDeviceD3d::ReleaseForReset | `0x13FDC0` | `void __thiscall(device, int release_textures)`, `ret 4`. Clears the state caches (render states `+0x748`, 256 hashed `{kind, state, value}` slots of 12 bytes, which `0x142C60` checks before every SetRenderState; texture stages `+0x1348`/`+0x1358`; lights `+0x1B58`, 8 × `0x74`) and releases the surfaces at `+0x6C0..+0x6D8`; with 1 also the textures (`0x143B00`). Called with 0 before Reset (`0x1404D2`), with 1 by the teardown (`0x13FD2B`). The d3d-reset-lights patch hooks it. |
| CGxDeviceD3d::RestoreState | `0x142E50` | `void __thiscall(device)`, plain `ret`. After CreateDevice (`0x13FC9A`) and Reset (`0x1404FA`): z, lighting and fog states, the 8 lights from `+0x3C0` (enabled 0–4 on the main menu), the cull mode, then SetGamma(gamma, force 1). |
| CGxDeviceD3d::SetGamma | `0x13F870` | `void __thiscall(device, float exponent, int force)`, `ret 8`. Vtable `+0x1C`. Returns early if the exponent equals the cached one (`CGxDevice.gamma`, `+0xD8`) and force is 0. In fullscreen (`+0x9C` nonzero) with `D3DCAPS2_FULLSCREENGAMMA` (`0x20000` in `+0x594`) sends `SetGammaRamp` with `(i/255)^(exponent + rand()/32767/1000) × 65535`. The Gamma option g gives exponent `(100 − g)/100 + 0.2` (Options::Apply, `0x04057F`, through `0x136B80`). |
| CGxDeviceD3d::WndProc | `0x140210` | `LRESULT __stdcall(hwnd, msg, wparam, lparam)`. Tracks focus in `.data 0xD54400`; on WM_SYSKEYUP Tab while focused (leaving with Alt+Tab) it sets a neutral ramp (SetGamma(1.0, 1)) for the desktop and puts the user's exponent back in the cache unapplied; RestoreState applies it after the Reset on return. |
| CGxDeviceOpenGl::CanDraw | `0x1442F0` | `bool __thiscall(device)`, plain `ret`. Device vtable `+0x3C`, called before every frame (`0x1365C0`). True if the viewport (`+0xBE4..+0xBF0`) has an area: false while minimized. Nothing recovers on restore (Direct3D's `0x140410` resets the device). |
| CGxDeviceOpenGl::ReleaseTextures | `0x13ED30` | `void __thiscall(device)`, plain `ret`. glDeleteTextures for the `+0x540` count / `+0x544` array of textures, clears each name (`+0x30`) and marks it dirty (`0x13A1E0`); the upload (vtable `+0x00`, `0x13EDB0`) makes new ones. |
| CGxDeviceOpenGl::DestroyContext | `0x13AFA0` | `void __thiscall(device)`, plain `ret`. ReleaseTextures, wglMakeCurrent(0), wglDeleteContext (`+0x580`), release the DC (`+0x57C`). |
| CGxDeviceOpenGl::CreateContext | `0x13AF40` | `bool __thiscall(device)`, plain `ret`. GetDC of the window `+0x578`, wglCreateContext (`0x13F5C0`), make current, driver queries (`0x13F2E0`), GL state setup (`0x13DCC0`). |

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
| Edition | `.data 0xCD52F0` | `Game.expansion`: 1 for The Frozen Throne, 0 for Reign of Chaos. `0x2DF4A0` turns it into the product code (`W3XP`/`WAR3`), found from the game name table `0xAA9874` ("Warcraft III: Reign of Chaos", "...: The Frozen Throne"), which `0x2DF470` indexes. `Frozen Throne.exe` is a launcher, so the program's name says nothing about it. |
| CPlayerWar3 race | `+0x260` | `GetPlayerRace` (`0x2017C0`, found from the native table's name string) maps the handle with `0x1EDE10` and returns this int, or 1 for no player: 1 human, 2 orc, 3 undead, 4 night elf. The player array's length is `CGameWar3+0x54` (`CGameWar3.player_count`). |

Net ids in a LAN lobby: the host had 128 and the joining player 129 (the
second of `0x3299A0`'s lists), so scan every byte, not 0–63. Not found: which
net id the host has, as seen by a client, in general. Candidates: the
per-player flag at `+0x3C` of a player entry (`0x32B800` tests it for the
local player before enabling a button), or the lowest id from 128.

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

| What | RVA | Notes |
|------|-----|-------|
| CSpriteFrame::SetModel | vtable `+0xE8` | `__thiscall(const char* path, -1, 0)` |
| CSpriteFrame::GetInstance | `0x0F7060` | `void* __thiscall()`; returns `*[sprite+0x178]` when `[sprite+0x174]` is set |
| SetAnimation | `0x1A42B0` | `void __fastcall(instance, const int* tags, int count, int flags)`, `ret 8`. Picks the sequence matching the tag set. |
| Tag registration | `0x352A60` | Tag ids in order: birth 0, death 1, stand 2, morph 3, alternate 4, found 5, mainmenu 6, … options `0x17`, realmselection `0x18`, maincancelpanel `0x19` (word list at `.rdata` `0x6FAB7EA0`). |

Example: tags `{stand, options, alternate}` select "Options Stand Alternate"
(the lowered left panel). "Options Morph" (1000 ms) lowers it, and "Options
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

## BLP textures (for the BLP guard)

The game decodes BLP1 textures in one place: `CBLPFile::Open` (RVA
`0x8E9790`, `int __thiscall(this, const char* name, void* context)`, `ret 8`).
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

## Methods and tools

- **Disassembly:** `objdump -d -M intel --start-address=0x6f… --stop-address=0x6f… "WarCraft III/game.dll"`.
  For repeated searches, dump `.text` once to a scratch file and grep it.
- **String → code:** `python3 -I tools/re/xrefs.py "WarCraft III/game.dll" CreditsButton`.
  Shows `push`/`mov ecx` uses and data tables. Strings used as FDF frame
  names lead straight to the code that looks those frames up.
- **Class → vtable:** `python3 -I tools/re/rtti.py "WarCraft III/game.dll" CTextFrame --base CFrame`.
  Prints the overridden slots, which narrows down where a class's own code is.
- **Byte signatures:** `python3 -I tools/re/signatures.py "WarCraft III/game.dll" mappings/1.27b.json [--write]`.
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
