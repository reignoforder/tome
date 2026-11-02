# SPDX-License-Identifier: MIT
# Packs the SDK into one zip for mod makers (the sdk-dist target in
# sdk/CMakeLists.txt): cmake -DSDK=<sdk> -DOUT=<zip> -DNAME=<top folder>
# [-DCHECK_EXE=<tome-check.exe> -DTEST_DLL=<tome_test.dll> -DTEST_IMPLIB=<its import library>]
# [-DGPL=<Tome's LICENSE> -DTHIRD_PARTY=<loader/third_party>] -P dist.cmake
#
# Everything in sdk/ but build output (Rust target/ folders, build/ and
# build-*/ folders such as a template built in place, a template's
# Cargo.lock), under one top folder, and Tome's tools for mods in bin/ and
# lib/ (TomeMod.cmake, "Checks at build time and tests"). The tools are
# Tome's code (GPL) with third-party code in them: their licenses go into
# licenses/ (loader/third_party/NOTICES.md).
if((DEFINED CHECK_EXE OR DEFINED TEST_DLL OR DEFINED TEST_IMPLIB) AND NOT (DEFINED GPL AND DEFINED THIRD_PARTY))
    message(FATAL_ERROR "dist.cmake: Tome's tools ship with their licenses: pass -DGPL and -DTHIRD_PARTY too")
endif()
file(GLOB_RECURSE files RELATIVE "${SDK}" "${SDK}/*")
list(FILTER files EXCLUDE REGEX "(^|/)(target|build|build-[^/]*)/|^templates/rust(-wasm)?/Cargo\\.lock$|(^|/)__pycache__/")
get_filename_component(stage "${OUT}" DIRECTORY)
set(stage "${stage}/sdk-dist")
file(REMOVE_RECURSE "${stage}")
foreach(file IN LISTS files)
    get_filename_component(dir "${stage}/${NAME}/${file}" DIRECTORY)
    file(COPY "${SDK}/${file}" DESTINATION "${dir}")
endforeach()
foreach(tool CHECK_EXE TEST_DLL TEST_IMPLIB)
    if(DEFINED ${tool})
        if(tool STREQUAL "TEST_IMPLIB")
            set(dir lib)
        else()
            set(dir bin)
        endif()
        file(COPY "${${tool}}" DESTINATION "${stage}/${NAME}/${dir}")
        get_filename_component(name "${${tool}}" NAME)
        list(APPEND files "${dir}/${name}")
    endif()
endforeach()
if(DEFINED GPL)
    # Not LICENSE: that's the SDK's own (MIT), at the top.
    configure_file("${GPL}" "${stage}/${NAME}/licenses/GPL-3.0.txt" COPYONLY)
    list(APPEND files "licenses/GPL-3.0.txt")
endif()
if(DEFINED THIRD_PARTY)
    # Everything in it but code and Tome's notes (wamr/TOME.md), so a new
    # row's text in NOTICES.md comes along; not WAMR's old vendored core/.
    file(GLOB_RECURSE texts RELATIVE "${THIRD_PARTY}" "${THIRD_PARTY}/*")
    list(FILTER texts EXCLUDE REGEX "\\.(c|h|cmake)$|(^|/)TOME\\.md$|^wamr/core/")
    if(NOT EXISTS "${THIRD_PARTY}/NOTICES.md")
        message(FATAL_ERROR "dist.cmake: no NOTICES.md in ${THIRD_PARTY}")
    endif()
    foreach(text IN LISTS texts)
        get_filename_component(dir "${stage}/${NAME}/licenses/${text}" DIRECTORY)
        file(COPY "${THIRD_PARTY}/${text}" DESTINATION "${dir}")
        list(APPEND files "licenses/${text}")
    endforeach()
endif()
file(REMOVE "${OUT}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${OUT}" --format=zip "${NAME}"
    WORKING_DIRECTORY "${stage}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "couldn't write ${OUT}")
endif()
list(LENGTH files count)
message(STATUS "SDK: ${OUT} (${count} files)")
