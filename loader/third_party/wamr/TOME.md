# WAMR in Tome

WebAssembly Micro Runtime (Bytecode Alliance), **WAMR-2.4.5**, commit
`25bd7eb63e828e4bd242cc9b38d260b4b31c6605`
(https://github.com/bytecodealliance/wasm-micro-runtime). Apache-2.0 WITH
LLVM-exception (`LICENSE`, and `ATTRIBUTIONS.md` for the parts it took from
others: here, they're only in `core/iwasm/include/wasm_c_api.h`). Both are
kept here because `tome.mix` contains WAMR's code.

Runs sandboxed WebAssembly mods (`docs/wasm-mods.md`). Its sources aren't
in the repository: `wamr.cmake` downloads the release archive when the build
is configured, checks its SHA-256, and applies Tome's patches
(`patch.cmake`), into `<build>/_deps/wamr-src`. Tome builds the files its
configuration needs with its own source list (`loader/CMakeLists.txt`,
target `wamr`), not WAMR's CMake scripts. The fuzz build
(`fuzz/CMakeLists.txt`, `fuzz_wamr`) builds the same configuration for the
host from the same download, with WAMR's Linux platform files
(`core/shared/platform/linux`, `core/shared/platform/common/posix`: the ones
its `platform_api_posix.cmake` picks without WASI) and its x86-64 native
call (`core/iwasm/common/arch/invokeNative_em64.s`).

Offline, download the archive once and configure with
`-DTOME_WAMR_ARCHIVE=/path/to/WAMR-2.4.5.tar.gz` (still checked against the
hash). A different archive is refused. Downloaded from its URL, the
archive is kept as `<build>/_deps/WAMR-2.4.5.tar.gz` (`wamr.cmake`; CI
attaches it to a release as the source of the WAMR in it).

## Configuration

Fast interpreter only: no AOT, JIT, WASI, libc, threads, shared memory,
SIMD or multi-module; reference types on (`WASM_ENABLE_REF_TYPES`: clang
and rustc use their encoding of `call_indirect` by default); hardware bound checks off (Tome's
target is x86-32, where WAMR has none: every access is checked in
software); instruction metering on (the per-call budget). The `-D` list in
`loader/CMakeLists.txt` is what WAMR's `runtime_lib.cmake` produced for
that, plus Tome's own (and its warnings, but for MSVC pragmas and lint
in `win_clock.c` and `bh_log.c`, stay on: check new ones when updating):

- `TOME_WAMR_NO_SOCKETS`: no Winsock (see the patches);
- `BH_VPRINTF=tome_wamr_vprintf`: WAMR's log lines go to `tome.log`.

WAMR allocates through Tome's capped heap (`core/wasm_alloc.h`), set in
`RuntimeInitArgs`.

## Patches (`patch.cmake`)

Each replaces one exact piece of text, and stops the configure step if the
text isn't there (another WAMR version); running it again changes nothing.

1. `platform_internal.h`: no `typedef SSIZE_T ssize_t` under MinGW, which
   has its own (`int` on 32-bit, where `SSIZE_T` is `long`: a conflict).
2. `platform_init.c`: `bh_platform_init` doesn't start Winsock under
   `TOME_WAMR_NO_SOCKETS`. Only WASI uses sockets; `win_socket.c` isn't
   built.
3. `platform_internal.h`: `os_thread_local_attribute` is `__thread` under
   MinGW GCC, which ignores `__declspec(thread)` with only a warning: every
   thread-local was one global. The cached stack boundary of the first
   thread that called a module (Tome's bootstrap thread) then applied to
   every thread, so calls on the main thread failed with "native stack
   overflow" (host test `host_wasm_first_load_on_another_thread`).

## Updating

Check WAMR's security advisories and release notes first. Then: put the new
tag's archive URL and SHA-256 in `wamr.cmake` (and `LICENSE`,
`ATTRIBUTIONS.md` from it here), make `patch.cmake` apply to it, configure
WAMR's `runtime_lib.cmake` with the configuration above for
`X86_32`/`windows` with the MinGW toolchain, and compare the compiled
sources (`compile_commands.json`) and the `-D` list with
`loader/CMakeLists.txt` (and the Linux ones with `fuzz/CMakeLists.txt`).
Run `make test`, `make e2e` and `tools/fuzz.sh 600 wamr`.
