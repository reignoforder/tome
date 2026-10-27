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

# Where `cmake --build build --target deploy` copies tome.mix.
set(TOME_GAME_DIR "${CMAKE_SOURCE_DIR}/WarCraft III" CACHE PATH "Warcraft III install directory")

# Development tools (tools/mpq). Downloads StormLib at configure time.
option(TOME_BUILD_DEV_TOOLS "Build development tools such as tome-mpq" OFF)

enable_testing()

add_subdirectory(sdk)
add_subdirectory(loader)
add_subdirectory(tests)
if(TOME_BUILD_DEV_TOOLS)
    add_subdirectory(tools/mpq)
endif()
