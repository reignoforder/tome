# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(tome_tests
    main.cpp
    test_abi.cpp
    test_assets.cpp
    test_background_mute.cpp
    test_blp.cpp
    test_chat.cpp
    test_command_line.cpp
    test_context_restore.cpp
    test_crash_report.cpp
    test_debug_script.cpp
    test_frames.cpp
    test_game_lock.cpp
    test_handshake.cpp
    test_hash.cpp
    test_hash_cache.cpp
    test_hooks.cpp
    test_intermod.cpp
    test_jass.cpp
    test_json.cpp
    test_keys.cpp
    test_log_view.cpp
    test_main_menu_fdf.cpp
    test_mappings.cpp
    test_metadata.cpp
    test_mod_menu_model.cpp
    test_mod_state.cpp
    test_movie_decoder.cpp
    test_mpq.cpp
    test_package.cpp
    test_path_guard.cpp
    test_popup.cpp
    test_resolver.cpp
    test_rtti.cpp
    test_settings.cpp
    test_signature.cpp
    test_text.cpp
    test_tome_hpp.cpp
    test_objects.cpp
    test_units.cpp
    test_utf8.cpp
    test_verify.cpp
    test_wasm_abi.cpp
    test_wasm_alloc.cpp
    test_wasm_layout.cpp
    test_version.cpp
    test_widescreen.cpp
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
    ../loader/src/patch.cpp
    ../loader/src/paths.cpp
    ../loader/src/unwind.cpp
)
target_link_libraries(tome_hook_runtime_tests PRIVATE tome_core)
target_include_directories(tome_hook_runtime_tests PRIVATE ../loader/src)
target_compile_definitions(tome_hook_runtime_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(tome_hook_runtime_tests PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
target_link_options(tome_hook_runtime_tests PRIVATE -static)
add_test(NAME tome_hook_runtime_tests COMMAND tome_hook_runtime_tests)

# The loader itself (tome_runtime: everything in tome.mix but DllMain) driven
# in this process against a fake game image, with the fixture mods in a
# temporary TOME_HOME: deterministic, no game files (tests/host/harness.h).
# Each test runs in a process of its own.
add_executable(tome_host_tests
    host/host_main.cpp
    host/harness.cpp
    host/test_api.cpp
    host/test_chat.cpp
    host/test_crash.cpp
    host/test_lifecycle.cpp
    host/test_manager.cpp
    host/test_mods.cpp
    host/test_wasm.cpp
)
target_link_libraries(tome_host_tests PRIVATE tome_testing tome_runtime monocypher)  # monocypher: signs mods in tests
target_include_directories(tome_host_tests PRIVATE .)
target_compile_definitions(tome_host_tests PRIVATE
    TOME_TEST_BUILD_DIR="${CMAKE_BINARY_DIR}"
    TOME_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_compile_options(tome_host_tests PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
target_link_options(tome_host_tests PRIVATE -static)
add_dependencies(tome_host_tests hook_a hook_b crash hello hello_cpp)
if(TARGET hello_wasm)
    add_dependencies(tome_host_tests hello_wasm)
    target_compile_definitions(tome_host_tests PRIVATE TOME_TEST_WASM_EXAMPLE=1)
endif()
if(TARGET hello_wasm_rs)
    add_dependencies(tome_host_tests hello_wasm_rs)
    target_compile_definitions(tome_host_tests PRIVATE TOME_TEST_WASM_RUST_EXAMPLE=1)
endif()
# Signed by sdk/support/tome_sign.py with a public test-only key
# (host/test-signing.key; host/test-signing.key.md),
# so host tests check Tome and the signer agree on what's signed:
# crash-signed, a folder with files in subfolders and non-ASCII names, and
# packages/crash.tome from tome_package(SIGN_KEY).
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    set(signed "${CMAKE_BINARY_DIR}/mods/crash-signed")
    set(test_key "${CMAKE_CURRENT_SOURCE_DIR}/host/test-signing.key")
    add_custom_command(OUTPUT "${signed}/tome.sig"
        COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_SOURCE_DIR}/host/make_signed_fixture.py"
            "${CMAKE_BINARY_DIR}/mods/crash" "${signed}" "${test_key}"
        DEPENDS crash "${CMAKE_CURRENT_SOURCE_DIR}/host/make_signed_fixture.py" "${test_key}"
            "${CMAKE_SOURCE_DIR}/sdk/support/tome_sign.py" "${CMAKE_CURRENT_SOURCE_DIR}/e2e/mods/crash/tome.mod.json"
        COMMENT "Signing crash-signed"
        VERBATIM
    )
    add_custom_target(crash_signed ALL DEPENDS "${signed}/tome.sig")
    tome_package(crash SIGN_KEY host/test-signing.key)
    add_dependencies(tome_host_tests crash_signed crash_package)
    target_compile_definitions(tome_host_tests PRIVATE TOME_TEST_SIGNED=1)
    add_test(NAME tome_sign_selftest
        COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_SOURCE_DIR}/sdk/support/tome_sign.py" selftest)
    # Every source file says its license (tools/spdx.py lists them with git).
    find_package(Git QUIET)
    if(Git_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
        add_test(NAME tome_spdx_headers
            COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_SOURCE_DIR}/tools/spdx.py" --check)
        # Skipped (77) when git can't read the checkout, e.g. a worktree in the container.
        set_tests_properties(tome_spdx_headers PROPERTIES
            ENVIRONMENT "GIT=${GIT_EXECUTABLE}" SKIP_RETURN_CODE 77)
    endif()
endif()
add_test(NAME tome_host_tests COMMAND tome_host_tests)

# The same, against the real game.dll from TOME_GAME_DIR, loaded into the test
# process but never run: Tome's startup patching, the hook engine on every
# mapped function, and mods' hooks around real game code. Each test is
# skipped without the install (tests/game/test_game.cpp).
add_executable(tome_game_tests
    host/host_main.cpp
    host/harness.cpp
    game/test_game.cpp
)
target_link_libraries(tome_game_tests PRIVATE tome_testing tome_runtime)
target_include_directories(tome_game_tests PRIVATE . host)
target_compile_definitions(tome_game_tests PRIVATE
    TOME_TEST_BUILD_DIR="${CMAKE_BINARY_DIR}"
    TOME_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    TOME_TEST_GAME_DIR="${TOME_GAME_DIR}"
)
target_compile_options(tome_game_tests PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
target_link_options(tome_game_tests PRIVATE -static)
add_dependencies(tome_game_tests hook_a hook_b crash)
add_test(NAME tome_game_tests COMMAND tome_game_tests)

# Tome's tools for mod makers (loader/src/testing): tome_test.dll on mods of
# every kind, and tome-check on broken fixtures (it checks every mod this
# build makes as it builds them, sdk/TomeMod.cmake).
set(mods_under_test crash)
set(mods_test_defines TOME_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(TARGET hello_rs)
    list(APPEND mods_under_test hello_rs)
    list(APPEND mods_test_defines TOME_TEST_RUST_EXAMPLE=1)
endif()
if(TARGET hello_wasm)
    list(APPEND mods_under_test hello_wasm)
    list(APPEND mods_test_defines TOME_TEST_WASM_EXAMPLE=1)
endif()
tome_add_mod_test(tome_mod_tools_tests SOURCES tools/test_tome_test.c MODS ${mods_under_test})
target_compile_definitions(tome_mod_tools_tests PRIVATE ${mods_test_defines})
# Its failure reports: cases that fail on purpose with TOME_TEST_SHOW_FAILURES
# (each message checked), and a case name that doesn't exist.
add_test(NAME tome_test_failures COMMAND ${CMAKE_COMMAND}
    "-DCOMMAND=${CMAKE_CROSSCOMPILING_EMULATOR};$<TARGET_FILE:tome_mod_tools_tests>;shows_"
    "-DEXPECT=no log line with \"a line nobody logs\";needs-ghost is \"error\", not \"active\": ;is \"got\", not \"wanted\";is NULL, not \"wanted\";Tome folder kept: ;unhandled exception 0xc0000005;shows_a_crash crashed;2 tests, 2 failed"
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/expect_output.cmake)
set_tests_properties(tome_test_failures PROPERTIES ENVIRONMENT "TOME_TEST_SHOW_FAILURES=1;WINEDEBUG=-all")
add_test(NAME tome_test_unknown_case COMMAND tome_mod_tools_tests --run no-such-case)
set_tests_properties(tome_test_unknown_case PROPERTIES ENVIRONMENT "WINEDEBUG=-all"
    PASS_REGULAR_EXPRESSION "no test case named no-such-case")
# tome-check on a broken fixture: its error, then the line it prints when it
# exits with 1.
function(tome_check_fails fixture expected)
    # The error as it's printed: every regex special character escaped.
    string(REGEX REPLACE "([][+.*()^$?|\\\\])" "\\\\\\1" expected "${expected}")
    add_test(NAME tome_check_${fixture} COMMAND tome_check "${CMAKE_CURRENT_SOURCE_DIR}/e2e/mods/${fixture}")
    set_tests_properties(tome_check_${fixture} PROPERTIES ENVIRONMENT "WINEDEBUG=-all"
        PASS_REGULAR_EXPRESSION "error: ${expected}(.|\n)*: check failed")
endfunction()
tome_check_fails(broken-json "tome.mod.json: line 2, column 1: expected a string key")
tome_check_fails(needs-ghost "doesn't start: error: needs \"ghost\"")
tome_check_fails(needs-unmapped "tome.mod.json: needs \"CUnit.wings\", which isn't mapped for 1.27b")
tome_check_fails(wrong-arch "tome.mod.json: entry DLL \"wrong-arch.dll\" not found")
# ...and where it passes or only warns: the C example and an assets-only
# mod pass (--strict: warnings would fail them); a mod for one edition; a mod whose icon is
# missing; a folder that isn't there; no arguments.
function(tome_check_prints name expected)
    add_test(NAME tome_check_${name} COMMAND tome_check ${ARGN})
    set_tests_properties(tome_check_${name} PROPERTIES ENVIRONMENT "WINEDEBUG=-all" PASS_REGULAR_EXPRESSION "${expected}")
endfunction()
tome_check_prints(hello "hello 1\\.0\\.0: ok \\(native, 7 natives\\)" --strict "${CMAKE_BINARY_DIR}/mods/hello")
tome_check_prints(assets "asset-demo 1\\.0\\.0: ok \\(assets, 0 natives\\)" --strict "${CMAKE_CURRENT_SOURCE_DIR}/e2e/mods/asset-demo")
tome_check_prints(icon "warning: icon icon\\.blp isn't in the mod folder" "${CMAKE_CURRENT_SOURCE_DIR}/e2e/mods/zz-bad-icon")
# A mod for Reign of Chaos only: checked in that game unless told otherwise.
tome_check_prints(roc_only "roc-only 1\\.0\\.0: ok \\(no code, 0 natives\\)" --strict "${CMAKE_CURRENT_SOURCE_DIR}/e2e/mods/roc-only")
tome_check_prints(roc_only_in_tft "error: doesn't start: .*is for Reign of Chaos, this is The Frozen Throne"
    --edition tft "${CMAKE_CURRENT_SOURCE_DIR}/e2e/mods/roc-only")
tome_check_prints(missing "tome-check: can't read" "${CMAKE_CURRENT_BINARY_DIR}/no-such-mod")
tome_check_prints(usage "usage: tome-check")

# tome-mpq (TOME_BUILD_DEV_TOOLS) on a copy of one of the game's maps;
# skipped without the game.
if(TOME_BUILD_DEV_TOOLS)
    add_test(NAME tome_mpq COMMAND ${CMAKE_COMMAND} -DMPQ=$<TARGET_FILE:tome-mpq>
        "-DWINE=${CMAKE_CROSSCOMPILING_EMULATOR}" "-DGAME=${TOME_GAME_DIR}" -DWORK=${CMAKE_CURRENT_BINARY_DIR}/mpq-test
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_mpq.cmake)
    set_tests_properties(tome_mpq PROPERTIES ENVIRONMENT "WINEDEBUG=-all" SKIP_REGULAR_EXPRESSION "skipped: no melee map")
endif()
