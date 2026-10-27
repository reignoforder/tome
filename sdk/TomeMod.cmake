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
endfunction()
