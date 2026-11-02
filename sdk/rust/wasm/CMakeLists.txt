# SPDX-License-Identifier: MIT
# The Rust crate for WebAssembly mods (tome-wasm) and its example, when cargo
# and its wasm32-unknown-unknown target are installed: rustup target add
# wasm32-unknown-unknown (sdk/CMakeLists.txt).
tome_add_rust_mod(hello_wasm_rs WASM
    CRATE examples/hello-wasm-rs
    METADATA tome.mod.json
    FOLDER hello-wasm-rs
    FILES config.schema.json
)
tome_package(hello_wasm_rs)

# The Rust template (sdk/templates/rust-wasm), built so it keeps working.
tome_add_rust_mod(my_rust_wasm_mod WASM
    CRATE ../../templates/rust-wasm
    METADATA tome.mod.json
    FOLDER my-rust-wasm-mod
)
tome_package(my_rust_wasm_mod)

# The crate's tests, for the host (they don't call Tome).
execute_process(COMMAND ${TOME_RUSTC} -vV OUTPUT_VARIABLE rustc_info ERROR_QUIET)
if(rustc_info MATCHES "host: ([^\n]+)")
    add_test(NAME tome_wasm_rust_tests
        COMMAND ${TOME_CARGO} test -p tome-wasm --target ${CMAKE_MATCH_1} --target-dir "${CMAKE_BINARY_DIR}/cargo"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
