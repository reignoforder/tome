include(${CMAKE_SOURCE_DIR}/cmake/embed_mappings.cmake)
tome_embed_mappings("${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_mappings.cpp" "${CMAKE_SOURCE_DIR}/mappings")

# Platform-independent logic (JSON, versions, metadata, dependency
# resolution). Kept free of Windows APIs so it can be unit-tested.
add_library(tome_core STATIC
    src/core/assets.cpp
    src/core/background_mute.cpp
    src/core/blp.cpp
    src/core/chat.cpp
    src/core/command_line.cpp
    src/core/context_restore.cpp
    src/core/debug_script.cpp
    src/core/frames.cpp
    src/core/game_lock.cpp
    src/core/handshake.cpp
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
    src/core/mpq.cpp
    src/core/package.cpp
    src/core/path_guard.cpp
    src/core/pe.cpp
    src/core/popup.cpp
    src/core/resolver.cpp
    src/core/rtti.cpp
    src/core/settings.cpp
    src/core/tome_settings.cpp
    src/core/units.cpp
    src/core/verify.cpp
    src/core/version.cpp
    src/core/widescreen.cpp
    src/core/x86.cpp
    src/core/zip.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_mappings.cpp
)
target_include_directories(tome_core PUBLIC src)
target_link_libraries(tome_core PUBLIC tome_sdk)  # core/hooks uses the hook types from tome.h
# The loader's own code (not the tests: they set their own).
set(TOME_CXX_OPTIONS -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)
target_compile_options(tome_core PRIVATE ${TOME_CXX_OPTIONS})

# Everything in tome.mix but DllMain, compiled once: tome.mix links these
# objects, and so do the tests that drive the loader in their own process
# (tests/host, tests/game), so they test the same code that ships.
add_library(tome_runtime OBJECT
    src/api.cpp
    src/bootstrap.cpp
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

target_link_libraries(tome_runtime PUBLIC bcrypt version ole32)
# Linking an object library adds its objects (and its definitions) to tome.mix.
target_link_libraries(tome_loader PRIVATE tome_runtime)

add_custom_target(deploy
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:tome_loader> "${TOME_GAME_DIR}/tome.mix"
    DEPENDS tome_loader
    COMMENT "Copying tome.mix to ${TOME_GAME_DIR}"
)
