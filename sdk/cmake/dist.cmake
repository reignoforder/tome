# Packs the SDK into one zip for mod makers (the sdk-dist target in
# sdk/CMakeLists.txt): cmake -DSDK=<sdk> -DOUT=<zip> -DNAME=<top folder>
# [-DCHECK_EXE=<tome-check.exe> -DTEST_DLL=<tome_test.dll> -DTEST_IMPLIB=<its import library>] -P dist.cmake
#
# Everything in sdk/ but build output (Rust target/ folders, build/ and
# build-*/ folders such as a template built in place, a template's
# Cargo.lock), under one top folder, and Tome's tools for mods in bin/ and
# lib/ (TomeMod.cmake, "Checks at build time and tests").
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
file(REMOVE "${OUT}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${OUT}" --format=zip "${NAME}"
    WORKING_DIRECTORY "${stage}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "couldn't write ${OUT}")
endif()
list(LENGTH files count)
message(STATUS "SDK: ${OUT} (${count} files)")
