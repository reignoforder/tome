# WebAssembly mods

Status (2026-10-30): **built, experimental** (milestones 1–4 below). A mod
can be a WebAssembly module (`"wasm"` in `tome.mod.json`) instead of a DLL.
It runs in WAMR's interpreter inside Tome and can do almost everything a DLL
mod does through the API: lifecycle, settings, chat commands, game events,
screens and frames, clicks, keybinds, popups, messages between mods, JASS
natives and code values, the game's files and its own files. It can't hook
game functions, resolve addresses or touch memory outside its own: what
makes it sandboxed. Mod makers: in C, `sdk/include/tome/tome_wasm.h`, the
example `sdk/examples/hello-wasm` and the template `sdk/templates/wasm`; in
Rust, the `tome-wasm` crate (`sdk/rust/wasm`), the example `hello-wasm-rs`
and the template `sdk/templates/rust-wasm`; `sdk/README.md`, "WebAssembly
mods".

## Why

A DLL mod is native code in the game's process: Tome can't sandbox it, so
every DLL mod is full trust, and approval is the only protection. Most mods
don't need that. A Wasm mod has a boundary Tome enforces:

- the module reads and writes only its own linear memory (the interpreter
  bounds-checks every access, in software);
- it calls only Tome's imports, each of which checks its arguments
  (`core/wasm_abi.*`) before doing anything;
- what Tome hands it is copied into its memory (`core/wasm_layout.*`); it
  gets handles (small integers in a table of its own), never game pointers;
- a trap (out of bounds, unreachable, a call stack too deep, the instruction
  budget running out) is handled like a DLL mod's crash: the call is
  abandoned and the mod disabled; the game keeps running.

DLL mods stay, for what needs native code (hooks on arbitrary functions,
raw memory, patches). Wasm mods are for the rest, so fewer mods need full
trust.

## Pieces

| | |
|---|---|
| `loader/third_party/wamr` | WAMR 2.4.5 (Bytecode Alliance; Apache-2.0 WITH LLVM-exception), downloaded at configure time (hash-checked) and patched (`wamr.cmake`, `patch.cmake`); `TOME.md` there: the configuration, three MinGW patches, offline builds, how to update |
| `loader/src/wasm_mods.*` | loading a module, its imports and Tome's callbacks into it, natives (`natives::Handler`), traps |
| `loader/src/core/wasm_abi.*` | bounds, string and out-parameter checks, file names, the handle table |
| `loader/src/core/wasm_layout.*` | the wasm32 layouts Tome writes (events, units, game info, native arguments), JASS signatures, native results |
| `loader/src/core/wasm_imports.h` | the import list with WAMR's signatures, and the limits (shared with `fuzz/fuzz_wamr.cpp`) |
| `loader/src/core/wasm_alloc.*` | the heap WAMR allocates from, with a limit (below) |
| `sdk/include/tome/tome_wasm.h`, `tome_wasm_consts.h` | the C SDK: structs with layout asserts, imports, wrappers; the constants generated from `tome.h` (`tools/gen-wasm-consts.py`) |
| `sdk/support/tome_wasm.c` | the runtime compiled into every Wasm mod: `tome_buffer`, `tome_callback`, `tome_native`, the callback table, `memcpy` & co. |
| `sdk/TomeMod.cmake` | `tome_add_wasm_mod()`, `tome_find_wasm_toolchain()` (clang, and wasm-ld or a Rust toolchain's rust-lld); `tome_add_rust_mod(... WASM)` |
| `sdk/rust/wasm` | the Rust SDK: the `tome-wasm` crate (imports, the same runtime in Rust, closures, `export_mod!`; its constants generated too) and `hello-wasm-rs`, a workspace of its own for `wasm32-unknown-unknown` |

## The module's contract

Exports Tome calls, checked when it loads (a wrong type refuses the
module):

| Export | Type | |
|---|---|---|
| `tome_mod_load` | `() -> i32` | required; 0 is success |
| `tome_mod_enable`, `tome_mod_disable`, `tome_mod_unload` | `() -> ()` | optional |
| `tome_buffer` | `(size) -> ptr` | a buffer of at least `size` bytes Tome writes the next callback's data into; 0 if there's none (the call is then dropped) |
| `tome_callback` | `(id, ptr, len) -> ()` | a callback: `id` is what the module registered it with |
| `tome_native` | `(id, args, count) -> i32` | a native: `args` points at one word per argument |
| `tome_callbacks_revoked` | `() -> ()` | optional: the mod is being enabled again, and the disable before revoked every registration, so no id from before is called back (the C runtime frees its table; the Rust one already does in `tome_mod_disable`). Called before `tome_mod_enable` |
| `memory` | | the default memory |

`tome_callback` and `tome_native` need `tome_buffer`; registering a callback
or native without them is refused. The SDK's runtime provides all three
(and the C runtime `tome_callbacks_revoked`). Tome frees its own record of
the revoked callbacks and natives at the same moment.

Nothing may run while the module is instantiated: a start function, or an
exported `__wasm_call_ctors` or `__post_instantiate` (which WAMR would call
then), refuses the module. They'd run before Tome has the instance, without
the instruction budget and outside the crash guard; do that work in
`tome_mod_load`.

Imports: module `tome` only, functions only; anything else (another module,
an unknown name, the wrong type, a global or memory) refuses the module at
load. Strings are `(ptr, len)`: UTF-8 without NUL bytes, at most 4096 bytes
(64 for file names); a bad one is logged and the import does nothing
(`TOME_ERR_INVALID_ARGUMENT`, or the fallback). Out-parameters are
`(ptr, capacity)` and return the full length, `UINT32_MAX` for a buffer
outside memory; nothing is terminated.

| Import | `tome.h` | As a Wasm import |
|---|---|---|
| `log`, `mod_id`, `mod_version` | same | |
| `game_info` | same | `tome_wasm_game_info`, without `game_base` (an address) |
| `register_command`, `display_text` | same | |
| `setting_*`, `set_setting_*`, `on_setting_changed` | same | |
| `on_event` | same | the callback gets a `tome_wasm_event` (below) |
| `on_ui`, `ui_root` | same | the callback gets the screen as an i32 |
| `frame_get`, `frame_create`, `frame_set_point`, `frame_set_text`, `frame_set_visible`, `frame_set_animation`, `frame_on_click` | same | frames are handles (below); a click gets the button's handle |
| `load_fdf`, `load_toc` | same | `load_fdf`: a path in the mod's folder, as for DLLs; `load_toc`: checked like `read_game_file` |
| `on_keybind`, `keybind_name` | same | |
| `popup`, `popup_close` | same | the popup as a struct of `(ptr, len)` strings; the callback gets the id and choice |
| `restart_game` | same | it only asks the player |
| `selection` | same | an array of `tome_wasm_unit` |
| `subscribe`, `publish` | same | messages up to 64 KiB, copied |
| `map_script_shadows`, `read_game_file` | same | copied; names checked like the file natives' (`core/path_guard`: no `..`, drive, root or device, at most 259 bytes), since with the game's "Allow Local Files" on Storm reads loose files first; `read_game_file` also refuses Tome's own folder (`tome\...`: state.json, other mods' config files) and files over 64 MiB |
| `config_read`, `config_write` | instead of `config_dir` | the mod's own files in `tome/mods/config/<id>/`, plain names, up to 1 MiB |
| `register_native` | same | below |
| `jass_code_call`, `jass_code_execute` | same | Tome checks code values, as for DLLs |
| `object_type`, `object_of`, `handle_of`, `player_object`, `unit_info`, `get_property`, `set_property`, `get_keyed_property`, `set_keyed_property`, `list_objects` | same | objects are handles (below); `tome_object_type_of` in C (`tome_object_type` is the enum); `unit_info` writes a `tome_wasm_unit` |
| `host_player` | same | `tome_host_player` in C, `host_player()` (an `Option`) in Rust |
| `net_send`, `on_net_message` | same | `tome_net_send` and `tome_on_net_message(fn(player, data, size, replay, user), user)` in C; `net_send(&[u8])` and `on_net_message(\|player, data, replay\| ...)` in Rust |
| `timer_start`, `timer_stop` | same | `tome_timer_start(clock, delay, interval, fn(timer, user), user, &id)` in C, `timer_start(Clock::Game, ...)` in Rust; the callback block is `[timer][state]` (0: fires again, 1: its last time, 2: ended with its game, not called), so the module frees its side of a timer that's done |
| `on_checksum` | same | `tome_on_checksum(fn(user) -> uint64_t, user)` in C, `on_checksum(\|\| hash)` and `Hash` in Rust; the module writes the hash over its 8-byte callback block, which Tome reads back |
| `jass_call` | same | arguments one word each (`tome_jass_value`), strings as pointers to NUL-terminated text in the module's memory, copied in; a string result copied into a buffer of the module's (`tome_jass_call` in C, `jass_call` in Rust); the same checks as for DLLs (below) |
| `resolve`, `hook`, `unhook`, `hook_call_next` | **no** | addresses and raw arguments |
| `service_*`, `on_service` | **no** | function tables; `publish`/`subscribe` instead |
| `jass_string`, `jass_string_create` | not needed | strings come and go as text (below) |
| `mod_dir`, `config_dir` | **no** | paths |

New API functions get a Wasm import when they can be one without handing out
a pointer; `tools/gen-wasm-consts.py` keeps the constants in step (a test
checks).

### What Tome writes into the module's memory

`core/wasm_layout.*` lays out its structs as clang does for wasm32 (4-byte
pointers, 8-aligned doubles), for the address the block is copied to:
`tome_wasm_event` (184 bytes, the fields of `tome_event_info`),
`tome_wasm_unit` (28), `tome_wasm_game_info` (16),
`tome_wasm_script_shadow` (20). `tome_wasm.h` declares the same structs and
`_Static_assert`s every offset Tome writes, so a mismatch fails the mod's
build. Strings are NUL-terminated copies after the struct.

### Handles

- **Frames** are a name and context (or "the root of screen N"), looked up
  again on every use (`frame_get`/`ui_root`): a frame that's gone, or a
  screen that changed, just fails. A module never holds a frame pointer, so
  it can't make Tome touch freed memory.
- **Units, items, destructables and players** (in events, the selection,
  `object_of` and `player_object`) are ids in the module's table; the
  table drops them at each map load and game end (and past 100 000). The
  object behind one is only used through the game objects imports. Tome
  keeps each unit's, item's and destructable's agent id (`CAgent.id`) with
  its handle, taken whenever it hands the handle out (the object is live
  then), and looks the id up in the game's agent table before every use:
  a removed object fails the call without its memory being read. Players
  are only compared with the game's players. After that, the same checks
  as for DLL mods (`architecture.md`, "Game objects").
- **`jass_call`:** a native turns a handle argument into its object with
  the game's own converter, which looks the id up without an upper bound
  and then checks the object's type (an is-a test on its type tag; found
  in the converters of 30 handle types). Tome checks every handle argument
  first, for DLL and Wasm mods alike: 0 or a live id in the handle table
  (bounds and reference count, so a made-up id never reaches the
  unbounded lookup), of the parameter's kind where Tome can tell (unit,
  item, destructable, player, widget; other types only get other objects,
  and the converter tells those apart). Code values must be ones the map
  passed to a Tome native. Handle types that are integers (`unitstate`,
  `race`..., made by `Convert*`) pass as they are, as they do for map
  scripts, and so do texttags (indices the texttag natives bound-check
  themselves; images and ubersplats aren't, so they're refused). File natives are the path sanitizer's wrappers.
- Handles are per module: one module's handles mean nothing to another.

### Natives

Declared in `tome.mod.json` `"natives"` like a DLL mod's, registered with
`register_native(name, id)`. Tome converts the arguments before the module
sees them: integers, booleans, handle ids and code values as they are, reals
read from the pointer JASS passes (as float bits), strings copied
(NUL-terminated, a pointer to the copy; 0 for null). The result comes back
as a word and is checked:

- a string: a pointer to a NUL-terminated UTF-8 string in the module's
  memory, which Tome turns into a JASS string itself (there's no
  `jass_string_create` to misuse);
- a handle: only 0 or one of that call's own arguments of the same type;
  anything else becomes null (logged). JASS can't tell a made-up id typed
  as a unit from a real one, which is what typecast exploits rely on;
- natives returning `code` can't be registered;
- a registration converts by the signature it was made with: while the
  engine has another one bound (`tome.mod.json` changed it and a map loaded
  since), the native returns 0 instead of calling the module.

A trap in a native makes it return 0 and disables the mod, as a crash does.

## Limits

- 200M instructions per call into the module (WAMR's instruction metering,
  every opcode; about half a second under Wine): an endless loop is a trap.
  WAMR counts each entry into the module on its own (a callback that the
  module's own import call causes starts afresh) and doesn't count time in
  imports, so a call also traps at its next import or nested callback once
  5 seconds have passed since it began (`kCallTimeLimitMs`): a loop over
  imports can't hang the game either.
- 64 MiB of memory, a 64 KiB operand stack, modules up to 16 MiB. The
  memory limit covers the initial memory too: WAMR's own cap
  (`max_memory_pages`) only limits growth, so a module declaring more is
  refused after instantiation.
- WAMR's own allocations (its loader's and instances' structures, not
  linear memories, which it maps) come from a heap with a limit: 128 MiB
  outstanding, for every Wasm mod together (`core/wasm_alloc.h`). WAMR
  sizes some from counts a module declares before checking them against its
  bytes: 15 bytes could ask for gigabytes (`fuzz_wamr` found it). Past the
  limit the module fails to load, instead of taking the game's address
  space.
- No threads: every call is on the main thread, except the first load and
  enable (the bootstrap thread). WAMR's thread-locals (patched for MinGW) and
  a fresh exec env per thread handle that switch
  (`host_wasm_first_load_on_another_thread`, and the e2e test).
- WAMR starts on the first Wasm mod's load: nothing at startup otherwise.
  Its code is at most 268 KB of `tome.mix`.
- WAMR's configuration: fast interpreter; no AOT, JIT, WASI, threads, SIMD,
  multi-module; reference types on (clang and rustc emit them by default).

## Approval

A Wasm mod still goes through approval (`CodeKind::Wasm`, "runs sandboxed
code"): its natives are reachable from map scripts, and its assets and FDF
go to the game's parsers. Signatures work as for any mod. The text never
says "safe": the sandbox limits what the code can do, not what the game does
with its files.

## Multiplayer

Wasm is deterministic except for NaN bit patterns. `"sync": "required"` Wasm
mods work like DLL ones: the handshake hashes the module like any file.

## Tests

- Unit tests: `test_wasm_abi.cpp` (bounds, strings, out-parameters, file
  names, handles and their generations), `test_wasm_layout.cpp` (every
  struct's offsets, no game pointer in a block, older event structs,
  signatures, native results); fuzzed by `fuzz_wasm_abi` and
  `fuzz_wasm_layout`.
- Host tests (`tests/host/test_wasm.cpp`): modules built by
  `tests/wasm_builder.h` (approval and lazy start, commands, settings, config
  files, traps in load, enable, callbacks and natives, the instruction
  budget, bad arguments, refused imports and exports, made-up handle
  results, natives returning code, the thread switch), and the built
  `hello-wasm` example (natives with integer, real and handle arguments).
- e2e (`tools/e2e-test.sh`): `wasm-e2e` (`tests/e2e/make_wasm_mod.py`, no
  toolchain needed: a command and a trap in a game) and `hello-wasm` when
  it's built (game info, main menu frames, events, natives called by the map
  script with strings both ways and a handle result, a native running map
  code that calls the same mod's native again, a command with settings, a
  message from one Wasm mod to the other).
- The Rust example `hello-wasm-rs` in host tests (closures for commands
  and settings, enabled again after a disable, natives) and e2e (game info,
  frames, events, a command, a message from `hello-wasm`); the
  `tome-wasm` crate's own tests (`tome_wasm_rust_tests`, on the host: the
  runtime's buffers and callbacks, layouts, conversions).
- `tome_wasm_consts_current`: both SDKs' constants match `tome.h`.
- `fuzz_wamr`: WAMR itself, built with Tome's configuration for the host
  (x86-64 Linux, with WAMR's Linux platform files from the same
  download), from Tome's allocator and import list: a module through load,
  Tome's import check, instantiation with Tome's limits, and calls to every
  export under an instruction budget. Seeds: modules for what the
  configuration turns on (`fuzz/make_seeds.py`), `wasm-e2e` and the built
  examples. UBSan's alignment check is off for WAMR: its interpreter reads
  its compiled code unaligned on purpose.

## Milestones

1. ✅ WAMR built into Tome; lifecycle, log, commands, settings; traps, the budget.
2. ✅ Events, screens, frames and clicks, keybinds, popups, selection,
   messages, the game's files and the mod's own.
3. ✅ Natives (converted arguments, checked results) and code values.
4. ✅ The C SDK (`tome_wasm.h`, runtime, `tome_add_wasm_mod`, example,
   template) and the Rust one (`tome-wasm`, `hello-wasm-rs`, template).
5. Typed hooks (hook points with typed arguments implemented by Tome, for
   both kinds of mod), one at a time, as mods ask for them.

## Not done / open

- `fuzz_wamr` runs WAMR for x86-64 Linux; Tome's build is x86-32 Windows
  (no 32-bit sanitizer runtimes here). The interpreter and loader are the
  same code; the platform layer and native-call glue differ.
- Whether a Wasm mod could ever skip approval: probably not, while it can
  declare natives and ship assets.
