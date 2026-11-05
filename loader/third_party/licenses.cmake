# SPDX-License-Identifier: GPL-3.0-or-later
# tome_copy_licenses(<folder> <list>): copies the license texts in this
# folder (NOTICES.md says what each one covers) into <folder>, keeping
# subfolders, and appends their paths (relative to <folder>) to <list>.
# Everything here but code and Tome's notes (wamr/TOME.md), so a new row's
# text in NOTICES.md comes along; not WAMR's old vendored core/. Included by
# the zips' scripts (sdk/cmake/dist.cmake, cmake/dist.cmake).
set(_tome_third_party "${CMAKE_CURRENT_LIST_DIR}")

function(tome_copy_licenses dest out)
    # The caller's list before any local is set, and locals with names of
    # their own: a local called like it (both callers' is texts) would be
    # read instead, and every path appended twice.
    set(_copied ${${out}})
    set(_from "${_tome_third_party}")
    if(NOT EXISTS "${_from}/NOTICES.md")
        message(FATAL_ERROR "tome_copy_licenses: no NOTICES.md in ${_from}")
    endif()
    file(GLOB_RECURSE _found RELATIVE "${_from}" "${_from}/*")
    list(FILTER _found EXCLUDE REGEX "\\.(c|h|cmake)$|(^|/)TOME\\.md$|^wamr/core/")
    foreach(_text IN LISTS _found)
        get_filename_component(_dir "${dest}/${_text}" DIRECTORY)
        file(COPY "${_from}/${_text}" DESTINATION "${_dir}")
        list(APPEND _copied "${_text}")
    endforeach()
    set(${out} ${_copied} PARENT_SCOPE)
endfunction()
