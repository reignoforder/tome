# SPDX-License-Identifier: GPL-3.0-or-later
# tome-mpq: a small MPQ utility for tests and development (list, extract,
# replace files in maps). Built on StormLib (MIT), fetched at configure time.
include(FetchContent)

FetchContent_Declare(stormlib
    GIT_REPOSITORY https://github.com/ladislav-zezula/StormLib.git
    GIT_TAG 86f9b99ffe4d3417dad16d00541cf6f2e3d7bf79  # v9.30
)
set(STORM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(stormlib)
# Patched in place (once: the script skips what's done).
execute_process(COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_LIST_DIR}/patch-stormlib.cmake"
    WORKING_DIRECTORY "${stormlib_SOURCE_DIR}" RESULT_VARIABLE patch_result)
if(NOT patch_result EQUAL 0)
    message(FATAL_ERROR "StormLib: Tome's patches don't apply to ${stormlib_SOURCE_DIR} (tools/mpq/patch-stormlib.cmake)")
endif()
# Its C (zlib 1.2.5, LZMA, LibTomCrypt...) is written before C23, GCC 16's
# default, which warns about every K&R function definition. NO_VIZ is
# zlib's switch for its visibility attribute, which PE targets don't have.
set_target_properties(storm PROPERTIES C_STANDARD 17 C_EXTENSIONS ON)
target_compile_definitions(storm PRIVATE NO_VIZ)

add_executable(tome-mpq tome_mpq.cpp)
target_link_libraries(tome-mpq PRIVATE storm)
target_link_options(tome-mpq PRIVATE -static)
