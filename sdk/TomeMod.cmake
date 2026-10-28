# Build helpers for mods (sdk/README.md). Included by Tome's own
# sdk/CMakeLists.txt, and by mods built outside this repo:
#
#   set(TOME_SDK_DIR "../tome/sdk" CACHE PATH "Tome's sdk folder")
#   include(${TOME_SDK_DIR}/TomeMod.cmake)
#
# Mods are 32-bit Windows DLLs: configure with the MinGW toolchain in
# cmake/toolchain-i686-mingw.cmake (from Linux), or a 32-bit MSVC or
# clang-cl (Visual Studio's "-A Win32").

include_guard(GLOBAL)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Tome mods must be 32-bit (Warcraft III is a 32-bit program). Configure with "
        "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_LIST_DIR}/cmake/toolchain-i686-mingw.cmake, "
        "or for Visual Studio with -A Win32.")
endif()

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
    # The game has no compiler runtime DLLs: link the runtime in.
    if(MSVC)
        set_target_properties(${target} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        target_link_options(${target} PRIVATE -static -static-libgcc $<$<CONFIG:Release>:-s>)
    endif()

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
    set_target_properties(${target} PROPERTIES TOME_MOD_FOLDER "${MOD_FOLDER}" TOME_MOD_FILES "${MOD_FILES}"
        TOME_MOD_ENTRY "$<TARGET_FILE_NAME:${target}>" TOME_MOD_METADATA "${CMAKE_CURRENT_SOURCE_DIR}/${MOD_METADATA}")
endfunction()

# tome_add_rust_mod(<target> CRATE <dir> METADATA <tome.mod.json> [FOLDER <name>] [FILES ...])
#
# The same for a mod written in Rust (sdk/rust): builds the cdylib crate in
# CRATE with cargo for i686-pc-windows-gnu, using this build's MinGW
# compiler as the linker, and assembles <build>/mods/<mod folder>/ with its
# DLL (named after the crate: hello-rs makes hello_rs.dll, which goes in
# tome.mod.json "entry"), tome.mod.json and the FILES. METADATA and FILES
# are relative to CRATE. cargo runs on every build and decides itself what
# is out of date.
function(tome_add_rust_mod target)
    cmake_parse_arguments(MOD "" "CRATE;METADATA;FOLDER" "FILES" ${ARGN})
    if(NOT MOD_FOLDER)
        set(MOD_FOLDER ${target})
    endif()
    if(NOT TOME_CARGO)
        find_program(TOME_CARGO cargo HINTS "$ENV{HOME}/.cargo/bin" REQUIRED)
    endif()
    get_filename_component(crate "${MOD_CRATE}" ABSOLUTE)
    file(READ "${crate}/Cargo.toml" manifest)
    # The DLL is named after [lib] name if there is one, else the package.
    # (Lines of the section up to its `name`, never past the next [header].)
    set(section_name "\\][^\n]*(\n([^[\n][^\n]*)?)*\nname *= *\"([^\"]+)\"")
    if(manifest MATCHES "\\[lib${section_name}")
        set(crate_name "${CMAKE_MATCH_3}")
    elseif(manifest MATCHES "\\[package${section_name}")
        set(crate_name "${CMAKE_MATCH_3}")
    else()
        message(FATAL_ERROR "tome_add_rust_mod(${target}): no package name in ${crate}/Cargo.toml")
    endif()
    string(REPLACE "-" "_" dll "${crate_name}.dll")
    set(rust_target i686-pc-windows-gnu)
    set(cargo_dir "${CMAKE_BINARY_DIR}/cargo")
    set(dir "${CMAKE_BINARY_DIR}/mods/${MOD_FOLDER}")
    string(TOUPPER "${rust_target}" env_target)
    string(REPLACE "-" "_" env_target "${env_target}")

    set(copies "")
    foreach(file IN LISTS MOD_FILES)
        list(APPEND copies COMMAND ${CMAKE_COMMAND} -E copy_if_different "${crate}/${file}" "${dir}/${file}")
    endforeach()
    # Run from the crate's folder so cargo finds its .cargo/config.toml; the
    # linker and the static runtime are passed in case there is none.
    add_custom_target(${target} ALL
        COMMAND ${CMAKE_COMMAND} -E env
            "CARGO_TARGET_${env_target}_LINKER=${CMAKE_C_COMPILER}"
            "CARGO_TARGET_${env_target}_RUSTFLAGS=-C link-arg=-static -C link-arg=-static-libgcc"
            ${TOME_CARGO} build --release --manifest-path "${crate}/Cargo.toml"
                --target ${rust_target} --target-dir "${cargo_dir}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${cargo_dir}/${rust_target}/release/${dll}" "${dir}/${dll}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${crate}/${MOD_METADATA}" "${dir}/tome.mod.json"
        ${copies}
        WORKING_DIRECTORY "${crate}"
        BYPRODUCTS "${dir}/${dll}"
        COMMENT "Building ${target} with cargo"
        USES_TERMINAL
        VERBATIM
    )
    set_target_properties(${target} PROPERTIES TOME_MOD_FOLDER "${MOD_FOLDER}" TOME_MOD_FILES "${MOD_FILES}"
        TOME_MOD_ENTRY "${dll}" TOME_MOD_SOURCE_DIR "${crate}" TOME_MOD_METADATA "${crate}/${MOD_METADATA}")
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
    get_target_property(entry ${target} TOME_MOD_ENTRY)
    get_target_property(source_dir ${target} TOME_MOD_SOURCE_DIR)
    get_target_property(metadata ${target} TOME_MOD_METADATA)
    if(NOT folder)
        message(FATAL_ERROR "tome_package(${target}): not a tome_add_mod or tome_add_rust_mod target")
    endif()
    if(NOT files)
        set(files "")
    endif()
    if(NOT source_dir)
        set(source_dir "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    set(dir "${CMAKE_BINARY_DIR}/mods/${folder}")
    set(out "${CMAKE_BINARY_DIR}/packages/${folder}.tome")
    list(TRANSFORM files PREPEND "${source_dir}/" OUTPUT_VARIABLE sources)
    # Relative names in the archive, from inside the mod folder: an entry
    # "tome.mod.json" at the top, never "./" or the folder's own name.
    add_custom_command(OUTPUT "${out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/packages"
        COMMAND ${CMAKE_COMMAND} -E rm -f "${out}"
        COMMAND ${CMAKE_COMMAND} -E tar cf "${out}" --format=zip
            tome.mod.json ${entry} ${files} ${PKG_FILES}
        WORKING_DIRECTORY "${dir}"
        # The metadata too: a Rust mod's DLL doesn't change with it.
        DEPENDS ${target} "${dir}/${entry}" "${metadata}" ${sources}
        COMMENT "Packing ${folder}.tome"
        VERBATIM
    )
    add_custom_target(${target}_package ALL DEPENDS "${out}")
endfunction()
