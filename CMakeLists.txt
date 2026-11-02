# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)

project(tome VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Tome must be built as 32-bit. Configure with "
        "-DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-i686-mingw.cmake")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# MinGW's default printf family is msvcrt's, which lacks C99 formats like %zu.
add_compile_definitions(__USE_MINGW_ANSI_STDIO=1)

# The Warcraft III 1.27b files the tests read: input/ (input/README.md).
# Nothing writes there. (Build folders made before input/ cached another one.)
unset(TOME_GAME_DIR CACHE)
set(TOME_GAME_DIR "${CMAKE_SOURCE_DIR}/input")

# Where `cmake --build build --target deploy` copies tome.mix: a game folder
# of your choice, given explicitly (empty: deploy only says so).
set(TOME_INSTALL_DIR "" CACHE PATH "Warcraft III folder that the deploy target copies tome.mix into")
if(TOME_INSTALL_DIR)
    get_filename_component(TOME_INSTALL_DIR "${TOME_INSTALL_DIR}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
endif()

# Developer builds read the TOME_DEBUG_* environment variables (README,
# "Environment variables"), which the tests need. TOME_DEBUG_APPROVE allows
# mods' code without the menu, so release builds leave this off.
option(TOME_DEV "Developer build: read the TOME_DEBUG_* environment variables" OFF)

# Development tools (tools/mpq). Downloads StormLib at configure time.
option(TOME_BUILD_DEV_TOOLS "Build development tools such as tome-mpq" OFF)

enable_testing()

# tome-check and tome_test.dll for the SDK's mods come from this build's
# targets (loader/CMakeLists.txt), not from an SDK zip's bin/ (sdk/TomeMod.cmake).
set(TOME_TOOLS_FROM_TARGETS ON)

add_subdirectory(sdk)
add_subdirectory(loader)
add_subdirectory(tests)
if(TOME_BUILD_DEV_TOOLS)
    add_subdirectory(tools/mpq)
endif()
