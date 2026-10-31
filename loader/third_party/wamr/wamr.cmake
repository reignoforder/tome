# WAMR 2.4.5 (Bytecode Alliance, Apache-2.0 WITH LLVM-exception; TOME.md):
# downloaded when the build is configured, checked against its SHA-256 and
# patched (patch.cmake), into <build>/_deps/wamr-src. Sets WAMR to its core/
# folder. Included by loader/CMakeLists.txt and fuzz/CMakeLists.txt.
#
# Offline: download the archive once and pass it, still checked:
#   -DTOME_WAMR_ARCHIVE=/path/to/WAMR-2.4.5.tar.gz
include_guard(GLOBAL)
include(FetchContent)

set(TOME_WAMR_ARCHIVE
    "https://github.com/bytecodealliance/wasm-micro-runtime/archive/refs/tags/WAMR-2.4.5.tar.gz"
    CACHE STRING "WAMR 2.4.5's source archive (a URL or a file); checked against its SHA-256")
FetchContent_Declare(wamr
    URL "${TOME_WAMR_ARCHIVE}"
    URL_HASH SHA256=1ab09d51099f276ca4a1d6629f6b589aab2bd0caa01445e05031a4bed22c199b
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
