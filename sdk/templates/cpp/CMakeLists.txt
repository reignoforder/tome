# A Tome mod in C++ (tome/tome.hpp). Copy this folder, rename "my-cpp-mod"
# (here and in tome.mod.json), and build:
#
#   cmake -S . -B build -DTOME_SDK_DIR=<tome>/sdk \
#         -DCMAKE_TOOLCHAIN_FILE=<tome>/sdk/cmake/toolchain-i686-mingw.cmake
#   cmake --build build
#
# build/mods/my-cpp-mod/ goes into the game's tome/mods/ as it is;
# build/packages/my-cpp-mod.tome is the same mod as one file.
cmake_minimum_required(VERSION 3.20)
project(my_cpp_mod CXX)

set(TOME_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "Tome's sdk folder")
include("${TOME_SDK_DIR}/TomeMod.cmake")

tome_add_mod(my_cpp_mod SOURCES mod.cpp METADATA tome.mod.json FOLDER my-cpp-mod)
set_target_properties(my_cpp_mod PROPERTIES OUTPUT_NAME my-cpp-mod CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
tome_package(my_cpp_mod)
