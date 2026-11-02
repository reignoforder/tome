# SPDX-License-Identifier: MIT
# The Rust crates and examples (sdk/rust/README.md), when cargo and its
# i686-pc-windows-gnu target are installed: rustup target add i686-pc-windows-gnu.
tome_add_rust_mod(hello_rs
    CRATE examples/hello-rs
    METADATA tome.mod.json
    FOLDER hello-rs
    FILES ui/hello_rs.fdf config.schema.json
)
tome_package(hello_rs)

# The Rust template (sdk/templates/rust), built so it keeps working.
tome_add_rust_mod(my_rust_mod
    CRATE ../templates/rust
    METADATA tome.mod.json
    FOLDER my-rust-mod
)

# The tome crate's tests, under Wine (the runner in .cargo/config.toml).
add_test(NAME tome_rust_tests
    COMMAND ${TOME_CARGO} test -p tome --target i686-pc-windows-gnu --target-dir "${CMAKE_BINARY_DIR}/cargo"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# The committed bindings match tome.h.
find_program(TOME_BINDGEN bindgen HINTS "$ENV{HOME}/.cargo/bin")
if(TOME_BINDGEN AND EXISTS "${CMAKE_SOURCE_DIR}/tools/gen-rust-bindings.sh")
    add_test(NAME tome_rust_bindings_current
        COMMAND "${CMAKE_SOURCE_DIR}/tools/gen-rust-bindings.sh" --check)
    set_tests_properties(tome_rust_bindings_current PROPERTIES ENVIRONMENT "BINDGEN=${TOME_BINDGEN}")
endif()
tome_package(my_rust_mod)
