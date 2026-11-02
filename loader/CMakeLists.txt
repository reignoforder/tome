# SPDX-License-Identifier: GPL-3.0-or-later
include(${CMAKE_SOURCE_DIR}/cmake/embed_mappings.cmake)
tome_embed_mappings("${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_mappings.cpp" "${CMAKE_SOURCE_DIR}/mappings")

# Monocypher (third_party/monocypher, BSD-2 or CC0): Ed25519 signature
# checks for tome.sig (core/signature.*). Its own warnings, not Tome's.
add_library(monocypher STATIC
    third_party/monocypher/monocypher.c
    third_party/monocypher/monocypher-ed25519.c
)
target_include_directories(monocypher PUBLIC third_party/monocypher)

# WAMR (Apache-2.0 WITH LLVM-exception; third_party/wamr/TOME.md): the
# WebAssembly interpreter for Wasm mods (docs/wasm-mods.md), downloaded and
# patched at configure time (third_party/wamr/wamr.cmake). The files
# and definitions WAMR's own runtime_lib.cmake picks for its fast
# interpreter on X86_32/windows with nothing else (no AOT, JIT, WASI,
# threads), instruction metering on; built with Tome's list instead of its
# scripts. With its own warnings (-Wall) but the ones it always has: MSVC
# pragmas, and lint in win_clock.c and bh_log.c. Keep the rest visible: an ignored
# __declspec(thread) was a real bug (third_party/wamr/TOME.md).
include(third_party/wamr/wamr.cmake)
add_library(wamr STATIC
    ${WAMR}/iwasm/common/arch/invokeNative_general.c
    ${WAMR}/iwasm/common/wasm_application.c
    ${WAMR}/iwasm/common/wasm_blocking_op.c
    ${WAMR}/iwasm/common/wasm_c_api.c
    ${WAMR}/iwasm/common/wasm_exec_env.c
    ${WAMR}/iwasm/common/wasm_loader_common.c
    ${WAMR}/iwasm/common/wasm_memory.c
    ${WAMR}/iwasm/common/wasm_native.c
    ${WAMR}/iwasm/common/wasm_runtime_common.c
    ${WAMR}/iwasm/common/wasm_shared_memory.c
    ${WAMR}/iwasm/interpreter/wasm_interp_fast.c
    ${WAMR}/iwasm/interpreter/wasm_loader.c
    ${WAMR}/iwasm/interpreter/wasm_runtime.c
    ${WAMR}/shared/mem-alloc/ems/ems_alloc.c
    ${WAMR}/shared/mem-alloc/ems/ems_gc.c
    ${WAMR}/shared/mem-alloc/ems/ems_hmu.c
    ${WAMR}/shared/mem-alloc/ems/ems_kfc.c
    ${WAMR}/shared/mem-alloc/mem_alloc.c
    ${WAMR}/shared/platform/common/memory/mremap.c
    ${WAMR}/shared/platform/windows/platform_init.c
    ${WAMR}/shared/platform/windows/win_atomic.cpp
    ${WAMR}/shared/platform/windows/win_clock.c
    ${WAMR}/shared/platform/windows/win_malloc.c
    ${WAMR}/shared/platform/windows/win_memmap.c
    ${WAMR}/shared/platform/windows/win_thread.c
    ${WAMR}/shared/platform/windows/win_time.c
    ${WAMR}/shared/platform/windows/win_util.c
    ${WAMR}/shared/utils/bh_assert.c
    ${WAMR}/shared/utils/bh_bitmap.c
    ${WAMR}/shared/utils/bh_common.c
    ${WAMR}/shared/utils/bh_hashmap.c
    ${WAMR}/shared/utils/bh_leb128.c
    ${WAMR}/shared/utils/bh_list.c
    ${WAMR}/shared/utils/bh_log.c
    ${WAMR}/shared/utils/bh_queue.c
    ${WAMR}/shared/utils/bh_vector.c
    ${WAMR}/shared/utils/runtime_timer.c
)
target_include_directories(wamr PRIVATE
    ${WAMR}/iwasm/interpreter
    ${WAMR}/iwasm/common
    ${WAMR}/shared/platform/windows
    ${WAMR}/shared/platform/include
    ${WAMR}/shared/mem-alloc
    ${WAMR}/shared/utils
)
target_include_directories(wamr PUBLIC ${WAMR}/iwasm/include)
target_compile_definitions(wamr PRIVATE
    BH_FREE=wasm_runtime_free
    BH_MALLOC=wasm_runtime_malloc
    BH_PLATFORM_WINDOWS
    BUILD_TARGET_X86_32
    HAVE_STRUCT_TIMESPEC
    WASM_DISABLE_HW_BOUND_CHECK=1
    WASM_DISABLE_STACK_HW_BOUND_CHECK=1
    WASM_DISABLE_WAKEUP_BLOCKING_OP=0
    WASM_ENABLE_AOT_INTRINSICS=0
    WASM_ENABLE_BULK_MEMORY=1
    WASM_ENABLE_EXTENDED_CONST_EXPR=0
    WASM_ENABLE_FAST_INTERP=1
    WASM_ENABLE_INSTRUCTION_METERING=1
    WASM_ENABLE_INTERP=1
    WASM_ENABLE_MINI_LOADER=0
    WASM_ENABLE_MULTI_MODULE=0
    WASM_ENABLE_QUICK_AOT_ENTRY=0
    # clang and rustc emit call_indirect in its reference-types encoding by
    # default; nothing of Tome's uses externref.
    WASM_ENABLE_REF_TYPES=1
    WASM_ENABLE_SHARED_MEMORY=0
    WASM_ENABLE_SHRUNK_MEMORY=1
    WASM_GLOBAL_HEAP_SIZE=10485760
    _WINSOCK_DEPRECATED_NO_WARNINGS
    # Tome's (TOME.md): no Winsock; WAMR's log lines go to tome.log.
    TOME_WAMR_NO_SOCKETS
    BH_VPRINTF=tome_wamr_vprintf
)
set_target_properties(wamr PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)
target_compile_options(wamr PRIVATE -Wall -Wno-unused-parameter -Wno-unknown-pragmas -ffunction-sections -fdata-sections)
set_source_files_properties(${WAMR}/shared/platform/windows/win_clock.c PROPERTIES
    COMPILE_OPTIONS "-Wno-maybe-uninitialized;-Wno-unused-variable")
set_source_files_properties(${WAMR}/shared/utils/bh_log.c PROPERTIES COMPILE_OPTIONS "-Wno-unused-but-set-variable")

# Platform-independent logic (JSON, versions, metadata, dependency
# resolution). Kept free of Windows APIs so it can be unit-tested.
add_library(tome_core STATIC
    src/core/assets.cpp
    src/core/background_mute.cpp
    src/core/blp.cpp
    src/core/chat.cpp
    src/core/command_line.cpp
    src/core/context_restore.cpp
    src/core/crash_report.cpp
    src/core/debug_script.cpp
    src/core/frames.cpp
    src/core/game_lock.cpp
    src/core/handshake.cpp
    src/core/hash_cache.cpp
    src/core/hooks.cpp
    src/core/intermod.cpp
    src/core/jass.cpp
    src/core/json.cpp
    src/core/keys.cpp
    src/core/main_menu_fdf.cpp
    src/core/mappings.cpp
    src/core/metadata.cpp
    src/core/mod_menu_model.cpp
    src/core/mod_state.cpp
    src/core/movie_decoder.cpp
    src/core/mpq.cpp
    src/core/package.cpp
    src/core/path_guard.cpp
    src/core/pe.cpp
    src/core/popup.cpp
    src/core/resolver.cpp
    src/core/rtti.cpp
    src/core/settings.cpp
    src/core/signature.cpp
    src/core/tome_settings.cpp
    src/core/units.cpp
    src/core/verify.cpp
    src/core/version.cpp
    src/core/wasm_abi.cpp
    src/core/wasm_alloc.cpp
    src/core/wasm_layout.cpp
    src/core/widescreen.cpp
    src/core/x86.cpp
    src/core/zip.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_mappings.cpp
)
target_include_directories(tome_core PUBLIC src)
target_link_libraries(tome_core PUBLIC tome_sdk)  # core/hooks uses the hook types from tome.h
target_link_libraries(tome_core PRIVATE monocypher)
# The loader's own code (not the tests: they set their own).
set(TOME_CXX_OPTIONS -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)
target_compile_options(tome_core PRIVATE ${TOME_CXX_OPTIONS})

# Everything in tome.mix but DllMain, compiled once: tome.mix links these
# objects, and so do the tests that drive the loader in their own process
# (tests/host, tests/game), so they test the same code that ships.
add_library(tome_runtime OBJECT
    src/api.cpp
    src/bootstrap.cpp
    src/crash_handler.cpp
    src/crash_loop.cpp
    src/debug_input.cpp
    src/events.cpp
    src/frames.cpp
    src/game_build.cpp
    src/guard.cpp
    src/handshake.cpp
    src/hooks.cpp
    src/intermod.cpp
    src/keybinds.cpp
    src/log.cpp
    src/main_thread.cpp
    src/mappings.cpp
    src/mod_manager.cpp
    src/mod_menu.cpp
    src/mods_ready.cpp
    src/natives.cpp
    src/overlay.cpp
    src/packages.cpp
    src/patch.cpp
    src/patches.cpp
    src/patches/ambient_sound.cpp
    src/patches/background_mute.cpp
    src/patches/blp_guard.cpp
    src/patches/d3d_reset_lights.cpp
    src/patches/gl_context_restore.cpp
    src/patches/memhack_guard.cpp
    src/patches/movie_frame_length.cpp
    src/patches/movie_display_reset.cpp
    src/patches/path_sanitizer.cpp
    src/patches/widescreen.cpp
    src/paths.cpp
    src/popups.cpp
    src/restart.cpp
    src/settings_api.cpp
    src/sha256.cpp
    src/storm.cpp
    src/tome_settings.cpp
    src/units.cpp
    src/unwind.cpp
    src/wasm_mods.cpp
    src/watcher.cpp
)
target_link_libraries(tome_runtime PUBLIC tome_core tome_sdk)
target_include_directories(tome_runtime PUBLIC src)

add_library(tome_loader SHARED src/dllmain.cpp)

# Miles Sound System (Mss32.dll) LoadLibrary's every *.mix in the game folder,
# which is how Tome gets into the process without patching anything.
set_target_properties(tome_loader PROPERTIES
    PREFIX ""
    OUTPUT_NAME "tome"
    SUFFIX ".mix"
)

target_compile_definitions(tome_runtime PUBLIC
    TOME_VERSION="${PROJECT_VERSION}"
    TOME_VERSION_W=L"${PROJECT_VERSION}"
    TOME_DEV=$<BOOL:${TOME_DEV}>
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0601
)

target_compile_options(tome_runtime PRIVATE ${TOME_CXX_OPTIONS})
target_compile_options(tome_loader PRIVATE ${TOME_CXX_OPTIONS})

# The game ships no MinGW runtime DLLs, so link libgcc/libstdc++/winpthreads
# statically. tome.mix must only depend on DLLs that Windows itself provides.
target_link_options(tome_loader PRIVATE
    -static -static-libgcc -static-libstdc++
    -Wl,--enable-stdcall-fixup
    -Wl,--kill-at  # TomeRestartW, not TomeRestartW@16, for rundll32
    $<$<CONFIG:Release>:-s>
)

target_link_libraries(tome_runtime PUBLIC bcrypt version ole32 wamr)
# Linking an object library adds its objects (and its definitions) to tome.mix.
target_link_libraries(tome_loader PRIVATE tome_runtime)

get_filename_component(_tome_install "${TOME_INSTALL_DIR}" REALPATH)
get_filename_component(_tome_input "${TOME_GAME_DIR}" REALPATH)
if(TOME_INSTALL_DIR AND NOT _tome_install STREQUAL _tome_input)
    add_custom_target(deploy
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:tome_loader> "${TOME_INSTALL_DIR}/tome.mix"
        DEPENDS tome_loader
        COMMENT "Copying tome.mix to ${TOME_INSTALL_DIR}"
        VERBATIM  # paths like "Program Files (x86)": unquoted, ( breaks the shell
    )
else()
    # input/ is only read (by the tests), so it's no place to deploy to.
    add_custom_target(deploy
        COMMAND ${CMAKE_COMMAND} -E echo "deploy: set TOME_INSTALL_DIR to a game folder other than input/ (make deploy INSTALL_DIR=...)"
        COMMAND ${CMAKE_COMMAND} -E false
        VERBATIM
    )
endif()

# The loader without the game (src/testing/fake_game.h): a fake 1.27b image
# and a temporary Tome folder, for Tome's host tests and for mod makers'
# tools, which ship in the SDK zip (sdk/README.md, "Testing a mod"):
#   tome_test.dll  sdk/include/tome/tome_test.h, for mods' own tests
#   tome-check     checks a built mod as Tome would load it (tome_add_mod runs it)
# Not part of tome.mix.
add_library(tome_testing OBJECT src/testing/fake_game.cpp)
# Linked together with tome_runtime (an object library's objects only go to
# the targets that link it directly).
target_link_libraries(tome_testing PUBLIC tome_runtime)
target_compile_options(tome_testing PRIVATE ${TOME_CXX_OPTIONS})

add_library(tome_test SHARED src/testing/tome_test.cpp)
target_link_libraries(tome_test PRIVATE tome_testing tome_runtime)
target_compile_options(tome_test PRIVATE ${TOME_CXX_OPTIONS})
set_target_properties(tome_test PROPERTIES PREFIX "" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
# Only DLLs Windows provides; only the tome_test_* functions exported.
target_link_options(tome_test PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--exclude-all-symbols
    $<$<CONFIG:Release>:-s>)

add_executable(tome_check src/testing/tome_check.cpp)
target_link_libraries(tome_check PRIVATE tome_testing tome_runtime shell32)  # shell32: CommandLineToArgvW
target_compile_options(tome_check PRIVATE ${TOME_CXX_OPTIONS})
set_target_properties(tome_check PROPERTIES OUTPUT_NAME "tome-check" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_link_options(tome_check PRIVATE -static $<$<CONFIG:Release>:-s>)
