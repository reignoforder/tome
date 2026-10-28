# libFuzzer targets for the core parsers that read untrusted bytes (lobby
# chat lines, packages, tome.mod.json, map scripts and paths). Host build
# with clang, separate from the mingw build:
#   cmake -S fuzz -B build-fuzz -G Ninja -DCMAKE_CXX_COMPILER=clang++
#   cmake --build build-fuzz && build-fuzz/fuzz_handshake fuzz/corpus/handshake
# tools/fuzz.sh builds and runs them all.
cmake_minimum_required(VERSION 3.20)
project(tome_fuzz CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "libFuzzer needs clang: -DCMAKE_CXX_COMPILER=clang++")
endif()

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(SANITIZERS -fsanitize=address,undefined -fno-sanitize-recover=undefined)
set(CORE ${ROOT}/loader/src/core)

add_library(fuzz_core STATIC
    ${CORE}/chat.cpp
    ${CORE}/frames.cpp
    ${CORE}/handshake.cpp
    ${CORE}/jass.cpp
    ${CORE}/json.cpp
    ${CORE}/keys.cpp
    ${CORE}/metadata.cpp
    ${CORE}/package.cpp
    ${CORE}/path_guard.cpp
    ${CORE}/settings.cpp
    ${CORE}/version.cpp
    ${CORE}/zip.cpp
)
target_include_directories(fuzz_core PUBLIC ${ROOT}/loader/src ${ROOT}/sdk/include)
target_compile_options(fuzz_core PUBLIC -g -O1 -fno-exceptions -fno-rtti ${SANITIZERS} -fsanitize=fuzzer-no-link)
target_link_options(fuzz_core PUBLIC ${SANITIZERS})

foreach(name handshake chat zip inflate json jass paths)
    add_executable(fuzz_${name} fuzz_${name}.cpp)
    target_link_libraries(fuzz_${name} PRIVATE fuzz_core)
    target_compile_options(fuzz_${name} PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz_${name} PRIVATE -fsanitize=fuzzer)
endforeach()
