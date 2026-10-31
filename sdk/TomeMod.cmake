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

# DLL mods must be 32-bit. A project with no C or C++ compiler (only
# WebAssembly mods, tome_add_wasm_mod) needs neither.
function(tome_require_32bit)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(FATAL_ERROR "Tome mods must be 32-bit (Warcraft III is a 32-bit program). Configure with "
            "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_FUNCTION_LIST_DIR}/cmake/toolchain-i686-mingw.cmake, "
            "or for Visual Studio with -A Win32.")
    endif()
endfunction()
if(DEFINED CMAKE_SIZEOF_VOID_P)
    tome_require_32bit()
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
    tome_require_32bit()
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

# tome_add_rust_mod(<target> CRATE <dir> METADATA <tome.mod.json> [FOLDER <name>] [FILES ...] [WASM])
#
# The same for a mod written in Rust (sdk/rust): builds the cdylib crate in
# CRATE with cargo for i686-pc-windows-gnu, using this build's MinGW
# compiler as the linker, and assembles <build>/mods/<mod folder>/ with its
# DLL (named after the crate: hello-rs makes hello_rs.dll, which goes in
# tome.mod.json "entry"), tome.mod.json and the FILES. METADATA and FILES
# are relative to CRATE. cargo runs on every build and decides itself what
# is out of date.
#
# WASM: a WebAssembly mod (the tome-wasm crate, sdk/rust/wasm) instead,
# built for wasm32-unknown-unknown (rustup target add
# wasm32-unknown-unknown): hello-wasm-rs makes hello_wasm_rs.wasm, which
# goes in tome.mod.json "wasm". Needs no C compiler.
function(tome_add_rust_mod target)
    cmake_parse_arguments(MOD "WASM" "CRATE;METADATA;FOLDER" "FILES" ${ARGN})
    if(NOT MOD_WASM)
        tome_require_32bit()
    endif()
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
    set(cargo_dir "${CMAKE_BINARY_DIR}/cargo")
    set(dir "${CMAKE_BINARY_DIR}/mods/${MOD_FOLDER}")
    if(MOD_WASM)
        string(REPLACE "-" "_" dll "${crate_name}.wasm")
        set(rust_target wasm32-unknown-unknown)
        set(linker_env "")
    else()
        string(REPLACE "-" "_" dll "${crate_name}.dll")
        set(rust_target i686-pc-windows-gnu)
        string(TOUPPER "${rust_target}" env_target)
        string(REPLACE "-" "_" env_target "${env_target}")
        set(linker_env
            "CARGO_TARGET_${env_target}_LINKER=${CMAKE_C_COMPILER}"
            "CARGO_TARGET_${env_target}_RUSTFLAGS=-C link-arg=-static -C link-arg=-static-libgcc")
    endif()

    set(copies "")
    foreach(file IN LISTS MOD_FILES)
        list(APPEND copies COMMAND ${CMAKE_COMMAND} -E copy_if_different "${crate}/${file}" "${dir}/${file}")
    endforeach()
    # Run from the crate's folder so cargo finds its .cargo/config.toml; the
    # linker and the static runtime are passed in case there is none.
    add_custom_target(${target} ALL
        COMMAND ${CMAKE_COMMAND} -E env ${linker_env}
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

# tome_find_wasm_toolchain(): sets TOME_WASM_FOUND, and TOME_WASM_CLANG and
# TOME_WASM_LINK (a command) when clang and a WebAssembly linker are there:
# wasm-ld (LLVM's lld), or the rust-lld that every Rust toolchain ships
# (rustup), run as wasm-ld. Nothing else is needed: Wasm mods have no libc.
function(tome_find_wasm_toolchain)
    find_program(TOME_WASM_CLANG NAMES clang)
    find_program(TOME_WASM_LD NAMES wasm-ld)
    set(link "")
    if(TOME_WASM_LD)
        set(link "${TOME_WASM_LD}")
    else()
        file(GLOB lld "$ENV{HOME}/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld")
        if(lld)
            list(GET lld 0 lld)
            set(link "${lld};-flavor;wasm")
        endif()
    endif()
    set(found FALSE)
    if(TOME_WASM_CLANG AND link)
        # Exits 0 whatever it lists: look for the WebAssembly backend itself.
        execute_process(COMMAND ${TOME_WASM_CLANG} -print-targets
            RESULT_VARIABLE rc OUTPUT_VARIABLE targets ERROR_QUIET)
        if(rc EQUAL 0 AND targets MATCHES "wasm32")
            set(found TRUE)
        endif()
    endif()
    set(TOME_WASM_FOUND ${found} PARENT_SCOPE)
    set(TOME_WASM_LINK "${link}" PARENT_SCOPE)
endfunction()

# tome_add_wasm_mod(<target> SOURCES <.c/.cpp>... METADATA <tome.mod.json>
#                   [FOLDER <name>] [FILES ...])
#
# A WebAssembly mod (include/tome/tome_wasm.h): compiles the SOURCES and
# support/tome_wasm.c (its runtime) with clang for wasm32, no libc, links
# <build>/mods/<mod folder>/<target>.wasm (tome.mod.json "wasm") and copies
# tome.mod.json and the FILES there. Needs tome_find_wasm_toolchain() to have
# found one. C++ works without exceptions, RTTI or a standard library.
function(tome_add_wasm_mod target)
    cmake_parse_arguments(MOD "" "METADATA;FOLDER" "SOURCES;FILES" ${ARGN})
    if(NOT MOD_FOLDER)
        set(MOD_FOLDER ${target})
    endif()
    tome_find_wasm_toolchain()
    if(NOT TOME_WASM_FOUND)
        message(FATAL_ERROR "tome_add_wasm_mod(${target}): needs clang and wasm-ld (or a Rust toolchain's rust-lld)")
    endif()
    set(sdk "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    set(dir "${CMAKE_BINARY_DIR}/mods/${MOD_FOLDER}")
    set(obj_dir "${CMAKE_CURRENT_BINARY_DIR}/${target}.wasm-objects")
    file(GLOB headers "${sdk}/include/tome/tome_wasm*.h")
    set(objects "")
    foreach(src ${MOD_SOURCES} "${sdk}/support/tome_wasm.c")
        get_filename_component(abs "${src}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        get_filename_component(name "${abs}" NAME)
        set(obj "${obj_dir}/${name}.o")
        if(abs MATCHES "\\.(cpp|cc|cxx)$")
            set(lang -std=c++17 -fno-exceptions -fno-rtti)
        else()
            set(lang -std=c11)
        endif()
        add_custom_command(OUTPUT "${obj}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${obj_dir}"
            COMMAND ${TOME_WASM_CLANG} --target=wasm32 ${lang} -O2 -ffreestanding -nostdlib
                -Wall -Wextra -I "${sdk}/include" -c "${abs}" -o "${obj}"
            DEPENDS "${abs}" ${headers}
            COMMENT "Compiling ${name} (wasm32)"
            VERBATIM
        )
        list(APPEND objects "${obj}")
    endforeach()
    set(wasm "${dir}/${target}.wasm")
    set(copies "")
    foreach(file ${MOD_FILES})
        list(APPEND copies COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/${file}" "${dir}/${file}")
    endforeach()
    list(TRANSFORM MOD_FILES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/" OUTPUT_VARIABLE file_sources)
    add_custom_command(OUTPUT "${wasm}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${dir}"
        # --stack-first (the default only in newer wasm-ld): the stack at the
        # bottom of memory, so overflowing it traps instead of overwriting
        # the runtime's data, as in Rust mods.
        COMMAND ${TOME_WASM_LINK} --no-entry --strip-debug --stack-first -z stack-size=65536 ${objects} -o "${wasm}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/${MOD_METADATA}" "${dir}/tome.mod.json"
        ${copies}
        DEPENDS ${objects} "${CMAKE_CURRENT_SOURCE_DIR}/${MOD_METADATA}" ${file_sources}
        COMMENT "Linking ${MOD_FOLDER}/${target}.wasm"
        VERBATIM
    )
    add_custom_target(${target} ALL DEPENDS "${wasm}")
    set_target_properties(${target} PROPERTIES TOME_MOD_FOLDER "${MOD_FOLDER}" TOME_MOD_FILES "${MOD_FILES}"
        TOME_MOD_ENTRY "${target}.wasm" TOME_MOD_METADATA "${CMAKE_CURRENT_SOURCE_DIR}/${MOD_METADATA}")
endfunction()

# tome_package(<target> [SIGN_KEY <key file>] [FILES ...])
#
# Packs the folder tome_add_mod assembled for <target> into
# <build>/packages/<mod folder>.tome, a single file to install in
# tome/mods/ (docs/architecture.md, "Packages"): the DLL, tome.mod.json, the
# mod's FILES, and these FILES (paths in the mod folder, for files a build
# step makes there, such as a generated icon). Built with the mod.
#
# SIGN_KEY (a key from support/tome_sign.py keygen, outside the source
# tree and never committed) signs the package: a tome.sig over exactly the
# packed files, so players who allowed one version get later ones signed
# with the same key without being asked again. Needs Python 3.
function(tome_package target)
    cmake_parse_arguments(PKG "" "SIGN_KEY" "FILES" ${ARGN})
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
    set(names tome.mod.json ${entry} ${files} ${PKG_FILES})
    if(PKG_SIGN_KEY)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        get_filename_component(key "${PKG_SIGN_KEY}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        # Only the packed files, in a folder of their own, are signed.
        set(stage "${CMAKE_BINARY_DIR}/packages/${folder}.stage")
        set(unsigned "${CMAKE_BINARY_DIR}/packages/${folder}.unsigned.zip")
        add_custom_command(OUTPUT "${out}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/packages"
            COMMAND ${CMAKE_COMMAND} -E rm -rf "${out}" "${stage}" "${unsigned}"
            COMMAND ${CMAKE_COMMAND} -E tar cf "${unsigned}" --format=zip ${names}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${stage}"
            COMMAND ${CMAKE_COMMAND} -E chdir "${stage}" ${CMAKE_COMMAND} -E tar xf "${unsigned}"
            COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/support/tome_sign.py" sign "${stage}" "${key}"
            COMMAND ${CMAKE_COMMAND} -E chdir "${stage}" ${CMAKE_COMMAND} -E tar cf "${out}" --format=zip ${names} tome.sig
            COMMAND ${CMAKE_COMMAND} -E rm -rf "${stage}" "${unsigned}"
            WORKING_DIRECTORY "${dir}"
            DEPENDS ${target} "${dir}/${entry}" "${metadata}" ${sources} "${key}"
            COMMENT "Packing and signing ${folder}.tome"
            VERBATIM
        )
    else()
        # Relative names in the archive, from inside the mod folder: an entry
        # "tome.mod.json" at the top, never "./" or the folder's own name.
        add_custom_command(OUTPUT "${out}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/packages"
            COMMAND ${CMAKE_COMMAND} -E rm -f "${out}"
            COMMAND ${CMAKE_COMMAND} -E tar cf "${out}" --format=zip ${names}
            WORKING_DIRECTORY "${dir}"
            # The metadata too: a Rust mod's DLL doesn't change with it.
            DEPENDS ${target} "${dir}/${entry}" "${metadata}" ${sources}
            COMMENT "Packing ${folder}.tome"
            VERBATIM
        )
    endif()
    add_custom_target(${target}_package ALL DEPENDS "${out}")
endfunction()
