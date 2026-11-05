# SPDX-License-Identifier: GPL-3.0-or-later
# ccache, when it's installed, keeps compiled files across build folders and
# rebuilds (the objects are the compiler's own, unchanged). A launcher given
# with -DCMAKE_<LANG>_COMPILER_LAUNCHER wins. Included by the top-level
# CMakeLists.txt and fuzz/CMakeLists.txt, after project().
option(TOME_CCACHE "Compile through ccache when it's installed" ON)
if(TOME_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER AND NOT CMAKE_C_COMPILER_LAUNCHER)
    find_program(TOME_CCACHE_PROGRAM ccache)
    if(TOME_CCACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER "${TOME_CCACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${TOME_CCACHE_PROGRAM}")
    endif()
endif()
