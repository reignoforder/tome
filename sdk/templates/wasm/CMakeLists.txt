# A Tome mod in C as WebAssembly: sandboxed, so players aren't asked to
# trust it with their computer. Copy this folder, rename "my-wasm-mod" (here
# and in tome.mod.json), and build; it needs clang and wasm-ld (LLVM's lld,
# or the rust-lld of a Rust toolchain), no MinGW and no toolchain file:
#
#   cmake -S . -B build -DTOME_SDK_DIR=<tome>/sdk
#   cmake --build build
#
# build/mods/my-wasm-mod/ goes into the game's tome/mods/ as it is;
# build/packages/my-wasm-mod.tome is the same mod as one file.
cmake_minimum_required(VERSION 3.20)
project(my_wasm_mod NONE)

set(TOME_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "Tome's sdk folder")
include("${TOME_SDK_DIR}/TomeMod.cmake")

tome_add_wasm_mod(my_wasm_mod SOURCES mod.c METADATA tome.mod.json FOLDER my-wasm-mod)
tome_package(my_wasm_mod)
