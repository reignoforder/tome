# SPDX-License-Identifier: GPL-3.0-or-later
# WAMR 2.4.5 (Bytecode Alliance, Apache-2.0 WITH LLVM-exception; TOME.md):
# downloaded when the build is configured, checked against its SHA-256 and
# patched (patch.cmake), into <build>/_deps/wamr-src. Sets WAMR to its core/
# folder. Included by loader/CMakeLists.txt and fuzz/CMakeLists.txt.
#
# Offline: download the archive once and pass it, still checked:
#   -DTOME_WAMR_ARCHIVE=/path/to/WAMR-2.4.5.tar.gz
# A URL is downloaded to <build>/_deps/WAMR-2.4.5.tar.gz (once), where CI
# picks it up for a release's source archive.
include_guard(GLOBAL)
include(FetchContent)

set(TOME_WAMR_SHA256 1ab09d51099f276ca4a1d6629f6b589aab2bd0caa01445e05031a4bed22c199b)
set(TOME_WAMR_ARCHIVE
    "https://github.com/bytecodealliance/wasm-micro-runtime/archive/refs/tags/WAMR-2.4.5.tar.gz"
    CACHE STRING "WAMR 2.4.5's source archive (a URL or a file); checked against its SHA-256")
set(wamr_archive "${TOME_WAMR_ARCHIVE}")
if(wamr_archive MATCHES "^[a-z]+://" AND NOT FETCHCONTENT_SOURCE_DIR_WAMR)
    set(wamr_archive "${CMAKE_BINARY_DIR}/_deps/WAMR-2.4.5.tar.gz")
    set(wamr_have "")
    if(EXISTS "${wamr_archive}")
        file(SHA256 "${wamr_archive}" wamr_have)
    endif()
    if(NOT wamr_have STREQUAL TOME_WAMR_SHA256)
        file(DOWNLOAD "${TOME_WAMR_ARCHIVE}" "${wamr_archive}.part"
            EXPECTED_HASH SHA256=${TOME_WAMR_SHA256} STATUS wamr_status)
        list(GET wamr_status 0 wamr_code)
        if(NOT wamr_code EQUAL 0)
            file(REMOVE "${wamr_archive}.part")
            message(FATAL_ERROR "WAMR: couldn't download ${TOME_WAMR_ARCHIVE}: ${wamr_status} (offline: -DTOME_WAMR_ARCHIVE=<file>)")
        endif()
        file(RENAME "${wamr_archive}.part" "${wamr_archive}")
    endif()
endif()
FetchContent_Declare(wamr
    URL "${wamr_archive}"
    URL_HASH SHA256=${TOME_WAMR_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    # Only the sources: Tome builds them with its own list, not WAMR's CMake.
    SOURCE_SUBDIR tome-no-cmake
)
FetchContent_MakeAvailable(wamr)
# Patched in place (once: patch.cmake skips what's done), also a source
# folder given with FETCHCONTENT_SOURCE_DIR_WAMR.
execute_process(COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_LIST_DIR}/patch.cmake"
    WORKING_DIRECTORY "${wamr_SOURCE_DIR}" RESULT_VARIABLE patch_result)
if(NOT patch_result EQUAL 0)
    message(FATAL_ERROR "WAMR: Tome's patches don't apply to ${wamr_SOURCE_DIR} (third_party/wamr/patch.cmake)")
endif()
set(WAMR "${wamr_SOURCE_DIR}/core")
