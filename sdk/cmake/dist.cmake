# Packs the SDK into one zip for mod makers (the sdk-dist target in
# sdk/CMakeLists.txt): cmake -DSDK=<sdk> -DOUT=<zip> -DNAME=<top folder> -P dist.cmake
#
# Everything in sdk/ but build output (Rust target/ folders, build/ and
# build-*/ folders such as a template built in place, a template's
# Cargo.lock), under one top folder.
file(GLOB_RECURSE files RELATIVE "${SDK}" "${SDK}/*")
list(FILTER files EXCLUDE REGEX "(^|/)(target|build|build-[^/]*)/|^templates/rust/Cargo\\.lock$|(^|/)__pycache__/")
get_filename_component(stage "${OUT}" DIRECTORY)
set(stage "${stage}/sdk-dist")
file(REMOVE_RECURSE "${stage}")
foreach(file IN LISTS files)
    get_filename_component(dir "${stage}/${NAME}/${file}" DIRECTORY)
    file(COPY "${SDK}/${file}" DESTINATION "${dir}")
endforeach()
file(REMOVE "${OUT}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${OUT}" --format=zip "${NAME}"
    WORKING_DIRECTORY "${stage}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "couldn't write ${OUT}")
endif()
list(LENGTH files count)
message(STATUS "SDK: ${OUT} (${count} files)")
