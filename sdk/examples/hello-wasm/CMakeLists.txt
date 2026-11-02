# SPDX-License-Identifier: MIT
# The example as a WebAssembly mod: needs clang and wasm-ld (or a Rust
# toolchain's rust-lld); see sdk/CMakeLists.txt.
tome_add_wasm_mod(hello_wasm
    SOURCES hello_wasm.c
    METADATA tome.mod.json
    FOLDER hello-wasm
    FILES config.schema.json
)
tome_package(hello_wasm)
