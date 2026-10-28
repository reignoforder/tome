# The SDK: headers, build helpers, examples and templates (README.md).
# Part of Tome's build, or a project of its own, e.g. from the sdk-dist zip:
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-i686-mingw.cmake
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.20)
    project(tome_sdk LANGUAGES C CXX)
    set(TOME_SDK_STANDALONE ON)
    enable_testing()
endif()

include(TomeMod.cmake)

add_subdirectory(examples/hello)
add_subdirectory(examples/hello-cpp)
# The templates build on their own too (sdk/templates/*/CMakeLists.txt); here
# they're built so they keep working.
add_subdirectory(templates/c)
add_subdirectory(templates/cpp)

# Rust mods (sdk/rust): built when cargo and the i686-pc-windows-gnu target
# are installed, unless TOME_BUILD_RUST is OFF; ON makes them required.
set(TOME_BUILD_RUST AUTO CACHE STRING "Build the Rust crates and examples (AUTO, ON, OFF)")
set_property(CACHE TOME_BUILD_RUST PROPERTY STRINGS AUTO ON OFF)
if(NOT TOME_BUILD_RUST STREQUAL "OFF")
    find_program(TOME_CARGO cargo HINTS "$ENV{HOME}/.cargo/bin")
    find_program(TOME_RUSTC rustc HINTS "$ENV{HOME}/.cargo/bin")
    set(rust_ok FALSE)
    # tome_add_rust_mod links with the C compiler: MinGW's, not MSVC's.
    if(TOME_CARGO AND TOME_RUSTC AND NOT MSVC)
        execute_process(COMMAND ${TOME_RUSTC} --print target-libdir --target i686-pc-windows-gnu
            OUTPUT_VARIABLE rust_libdir OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE rust_result ERROR_QUIET)
        if(rust_result EQUAL 0 AND IS_DIRECTORY "${rust_libdir}")
            set(rust_ok TRUE)
        endif()
    endif()
    if(rust_ok)
        add_subdirectory(rust)
    elseif(TOME_BUILD_RUST STREQUAL "ON")
        message(FATAL_ERROR "TOME_BUILD_RUST=ON needs a MinGW build, cargo and: rustup target add i686-pc-windows-gnu")
    else()
        message(STATUS "Rust mods: skipped (needs a MinGW build, cargo and: rustup target add i686-pc-windows-gnu)")
    endif()
endif()

if(TOME_SDK_STANDALONE)
    return()
endif()

# The SDK as one zip for mod makers, without Tome's sources:
# cmake --build build --target sdk-dist (sdk/README.md, "Without Tome's repository").
add_custom_target(sdk-dist
    COMMAND ${CMAKE_COMMAND} -DSDK=${CMAKE_CURRENT_SOURCE_DIR}
        -DOUT=${CMAKE_BINARY_DIR}/tome-sdk-${PROJECT_VERSION}.zip -DNAME=tome-sdk-${PROJECT_VERSION}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dist.cmake
    COMMENT "Packing the SDK"
    VERBATIM
)
