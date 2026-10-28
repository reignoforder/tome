# A Tome mod in C. Copy this folder, rename "my-c-mod" (here and in
# tome.mod.json), and build:
#
#   cmake -S . -B build -DTOME_SDK_DIR=<tome>/sdk \
#         -DCMAKE_TOOLCHAIN_FILE=<tome>/sdk/cmake/toolchain-i686-mingw.cmake
#   cmake --build build
#
# build/mods/my-c-mod/ goes into the game's tome/mods/ as it is;
# build/packages/my-c-mod.tome is the same mod as one file.
cmake_minimum_required(VERSION 3.20)
project(my_c_mod C)

set(TOME_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "Tome's sdk folder")
include("${TOME_SDK_DIR}/TomeMod.cmake")

tome_add_mod(my_c_mod SOURCES mod.c METADATA tome.mod.json FOLDER my-c-mod)
set_target_properties(my_c_mod PROPERTIES OUTPUT_NAME my-c-mod)
tome_package(my_c_mod)
