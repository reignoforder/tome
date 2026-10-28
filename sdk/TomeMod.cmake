# Build helpers for mods. Included by Tome's own sdk/CMakeLists.txt, and by mods
# built outside this repo:
#
#   set(TOME_SDK_DIR "../tome/sdk" CACHE PATH "Tome's sdk folder")
#   include(${TOME_SDK_DIR}/TomeMod.cmake)

if(NOT TARGET tome_sdk)
    add_library(tome_sdk INTERFACE)
    target_include_directories(tome_sdk INTERFACE "${CMAKE_CURRENT_LIST_DIR}/include")
endif()

# tome_add_mod(<target> SOURCES ... METADATA <tome.mod.json> [FILES ...])
#
# Builds a mod DLL and assembles a ready-to-install folder at
# <build>/mods/<mod folder>/ containing the DLL, its tome.mod.json and the
# FILES (paths relative to the source folder, kept as they are, e.g. ui/x.fdf).
# The folder goes into tome/mods/ as it is; tome_package() makes a .tome of it.
function(tome_add_mod target)
    cmake_parse_arguments(MOD "" "METADATA;FOLDER" "SOURCES;FILES" ${ARGN})
    if(NOT MOD_FOLDER)
        set(MOD_FOLDER ${target})
    endif()

    add_library(${target} SHARED ${MOD_SOURCES})
    target_link_libraries(${target} PRIVATE tome_sdk)
    if(MINGW)
        # Keeps new game threads at the x87 precision Windows gives them
        # (support/fpreset.c), compiled as whichever language the mod uses.
        set(fpreset "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/support/fpreset.c")
        get_property(languages GLOBAL PROPERTY ENABLED_LANGUAGES)
        if(NOT "C" IN_LIST languages)
            set_source_files_properties("${fpreset}" TARGET_DIRECTORY ${target} PROPERTIES LANGUAGE CXX)
        endif()
        target_sources(${target} PRIVATE "${fpreset}")
    endif()
    set_target_properties(${target} PROPERTIES
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/mods/${MOD_FOLDER}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/mods/${MOD_FOLDER}"
    )
    # Mods load into a process without MinGW runtime DLLs either.
    target_link_options(${target} PRIVATE -static -static-libgcc $<$<CONFIG:Release>:-s>)

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/${MOD_METADATA}"
            "${CMAKE_BINARY_DIR}/mods/${MOD_FOLDER}/tome.mod.json"
    )
    foreach(file IN LISTS MOD_FILES)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/${file}"
                "${CMAKE_BINARY_DIR}/mods/${MOD_FOLDER}/${file}"
        )
    endforeach()
    # Rebuild (and so recopy) when the metadata or one of the files changes.
    list(TRANSFORM MOD_FILES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/" OUTPUT_VARIABLE file_paths)
    list(APPEND file_paths "${CMAKE_CURRENT_SOURCE_DIR}/${MOD_METADATA}")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS ${file_paths})
    # For tome_package.
    set_target_properties(${target} PROPERTIES TOME_MOD_FOLDER "${MOD_FOLDER}" TOME_MOD_FILES "${MOD_FILES}")
endfunction()

# tome_package(<target> [FILES ...])
#
# Packs the folder tome_add_mod assembled for <target> into
# <build>/packages/<mod folder>.tome, a single file to install in
# tome/mods/ (docs/architecture.md, "Packages"): the DLL, tome.mod.json, the
# mod's FILES, and these FILES (paths in the mod folder, for files a build
# step makes there, such as a generated icon). Built with the mod.
function(tome_package target)
    cmake_parse_arguments(PKG "" "" "FILES" ${ARGN})
    get_target_property(folder ${target} TOME_MOD_FOLDER)
    get_target_property(files ${target} TOME_MOD_FILES)
    if(NOT folder)
        message(FATAL_ERROR "tome_package(${target}): not a tome_add_mod target")
    endif()
    if(NOT files)
        set(files "")
    endif()
    set(dir "${CMAKE_BINARY_DIR}/mods/${folder}")
    set(out "${CMAKE_BINARY_DIR}/packages/${folder}.tome")
    list(TRANSFORM files PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/" OUTPUT_VARIABLE sources)
    # Relative names in the archive, from inside the mod folder: an entry
    # "tome.mod.json" at the top, never "./" or the folder's own name.
    add_custom_command(OUTPUT "${out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/packages"
        COMMAND ${CMAKE_COMMAND} -E rm -f "${out}"
        COMMAND ${CMAKE_COMMAND} -E tar cf "${out}" --format=zip
            tome.mod.json $<TARGET_FILE_NAME:${target}> ${files} ${PKG_FILES}
        WORKING_DIRECTORY "${dir}"
        DEPENDS ${target} ${sources}
        COMMENT "Packing ${folder}.tome"
        VERBATIM
    )
    add_custom_target(${target}_package ALL DEPENDS "${out}")
endfunction()
