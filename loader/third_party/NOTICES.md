# Third-party notices

Tome's binaries (`tome.mix`, and `tome-check.exe` and `tome_test.dll` in the
SDK zip's `bin/`) are under the GNU GPL v3 or later (`LICENSE`; in the zips,
`licenses/GPL-3.0.txt`). They contain code from the projects below, each
under its own license. The texts are in `loader/third_party/` in Tome's
source, and in `licenses/` in the zips.

| Code | License | Text |
|------|---------|------|
| [WAMR](https://github.com/bytecodealliance/wasm-micro-runtime) 2.4.5, the WebAssembly interpreter, with Tome's patches (`wamr/patch.cmake`) | Apache-2.0 WITH LLVM-exception | `wamr/LICENSE`, `wamr/ATTRIBUTIONS.md` |
| [Monocypher](https://monocypher.org) 4.0.3, Ed25519 signatures | BSD-2-Clause or CC0-1.0 | `monocypher/LICENCE.md` |
| [mingw-w64](https://www.mingw-w64.org) runtime (`libmingw32`, `libmingwex`), linked statically | ZPL-2.1; parts public domain, BSD, MIT or LGPL, as marked (gdtoa's printf and strtod among them) | `mingw-w64/COPYING`, `mingw-w64/COPYING.MinGW-w64-runtime.txt` (v13.0.0) |
| mingw-w64 winpthreads (`libwinpthread`), linked statically | MIT, parts BSD-3-Clause | `mingw-w64/COPYING.winpthreads` |
| GCC's runtime (`libgcc`, `libstdc++`), linked statically | GPL-3.0-or-later WITH GCC-exception-3.1 | `gcc/COPYING.RUNTIME`, with the GPL (`LICENSE`; `licenses/GPL-3.0.txt`) |
| Mark Adler's [puff.c](https://github.com/madler/zlib/tree/develop/contrib/puff), the model for the inflate code in `loader/src/core/zip.cpp` (altered: table-driven decoding, no `longjmp`) | Zlib | `puff/LICENSE` |
| Unicode's simple uppercase mappings (`loader/src/core/upcase_table.inc`, made by `tools/gen-upcase.py`) | Unicode-3.0 | `unicode/license.txt` |

## Source

Tome's source for a release is its tag in
https://github.com/reignoforder/tome (`v<version>`). WAMR isn't in it: the
build downloads the WAMR-2.4.5 release archive (SHA-256 in
`wamr/wamr.cmake`) and applies `wamr/patch.cmake`
(`wamr/TOME.md`). The mingw-w64 and GCC libraries are the toolchain's
(`tools/container/Containerfile` names the packages).
