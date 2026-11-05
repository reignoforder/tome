# SPDX-License-Identifier: GPL-3.0-or-later
# reproducible_zip() for the zips' scripts: cmake/dist.cmake (the player
# download) and sdk/cmake/dist.cmake (the SDK zip, given this file as
# -DREPRO_ZIP; the SDK zip is only made in Tome's repository).
#
# A zip that's the same bytes for the same files, wherever and whenever it's
# made (reproducible releases): cmake/repro_zip.py, given as -DZIP_TOOL with
# -DPYTHON. Without them, CMake's own zip writer, which stores each file's
# times. Each name after `dir` is a file or folder in it, packed with
# everything under it.
function(reproducible_zip out dir)
    file(REMOVE "${out}")
    if(DEFINED ZIP_TOOL AND DEFINED PYTHON AND PYTHON)
        execute_process(COMMAND "${PYTHON}" -I "${ZIP_TOOL}" "${out}" "${dir}" ${ARGN} RESULT_VARIABLE result)
    else()
        message(WARNING "No Python: ${out} isn't reproducible (it has the files' times)")
        execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${out}" --format=zip ${ARGN}
            WORKING_DIRECTORY "${dir}" RESULT_VARIABLE result)
    endif()
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "couldn't write ${out}")
    endif()
endfunction()
