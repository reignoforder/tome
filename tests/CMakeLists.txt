add_executable(tome_tests
    main.cpp
    test_assets.cpp
    test_chat.cpp
    test_command_line.cpp
    test_debug_script.cpp
    test_frames.cpp
    test_game_lock.cpp
    test_handshake.cpp
    test_hash.cpp
    test_hooks.cpp
    test_intermod.cpp
    test_jass.cpp
    test_json.cpp
    test_keys.cpp
    test_main_menu_fdf.cpp
    test_mappings.cpp
    test_metadata.cpp
    test_mod_menu_model.cpp
    test_mod_state.cpp
    test_mpq.cpp
    test_package.cpp
    test_path_guard.cpp
    test_popup.cpp
    test_resolver.cpp
    test_rtti.cpp
    test_settings.cpp
    test_text.cpp
    test_units.cpp
    test_utf8.cpp
    test_verify.cpp
    test_version.cpp
    test_x86.cpp
    test_zip.cpp
)
target_link_libraries(tome_tests PRIVATE tome_core)
# For checking the mapping files against the local install, if present.
target_compile_definitions(tome_tests PRIVATE TOME_TEST_GAME_DIR="${TOME_GAME_DIR}")
target_compile_options(tome_tests PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
target_link_options(tome_tests PRIVATE -static)

# Runs through CMAKE_CROSSCOMPILING_EMULATOR (wine) when cross-compiling.
add_test(NAME tome_tests COMMAND tome_tests)

# e2e fixture mods with code (tools/e2e-test.sh copies them from <build>/mods).
tome_add_mod(hook_a SOURCES e2e/mods/hook-a/hook_a.c METADATA e2e/mods/hook-a/tome.mod.json FOLDER hook-a)
tome_add_mod(hook_b SOURCES e2e/mods/hook-b/hook_b.c METADATA e2e/mods/hook-b/tome.mod.json FOLDER hook-b)
tome_add_mod(crash SOURCES e2e/mods/crash/crash.c METADATA e2e/mods/crash/tome.mod.json)
set_target_properties(hook_a PROPERTIES OUTPUT_NAME hook-a)
set_target_properties(hook_b PROPERTIES OUTPUT_NAME hook-b)

# The hook engine and the crash guard on real code in this process: the
# Windows side of loader/src/hooks.cpp and guard.cpp, compiled in directly
# (they only need the log).
add_executable(tome_hook_runtime_tests
    main.cpp
    test_guard.cpp
    test_hook_runtime.cpp
    ../loader/src/guard.cpp
    ../loader/src/hooks.cpp
    ../loader/src/log.cpp
    ../loader/src/paths.cpp
    ../loader/src/unwind.cpp
)
target_link_libraries(tome_hook_runtime_tests PRIVATE tome_core)
target_include_directories(tome_hook_runtime_tests PRIVATE ../loader/src)
target_compile_definitions(tome_hook_runtime_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(tome_hook_runtime_tests PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
target_link_options(tome_hook_runtime_tests PRIVATE -static)
add_test(NAME tome_hook_runtime_tests COMMAND tome_hook_runtime_tests)
