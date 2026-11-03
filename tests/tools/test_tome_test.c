/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tome_test.dll on mods of every kind (sdk/include/tome/tome_test.h): the
 * Rust and WebAssembly examples (when this build makes them:
 * TOME_TEST_RUST_EXAMPLE, TOME_TEST_WASM_EXAMPLE) and the crash fixture. The
 * hello example has tests of its own (sdk/examples/hello/test_hello.c). */
#include <tome/tome_test.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef TOME_TEST_RUST_EXAMPLE
static void rust_mod(void) {
    uint32_t result = 0;
    uint32_t args[1];
    TOME_EXPECT(tome_test_install(TOME_TEST_MODS_DIR "/hello-rs"));
    TOME_EXPECT(tome_test_start());
    if (!TOME_EXPECT_STATE("hello-rs", "active")) return;
    tome_test_load_map();
    args[0] = tome_test_real(1.5f);
    TOME_EXPECT(tome_test_call("HelloRsDouble", args, 1, &result));
    TOME_EXPECT(tome_test_real_result(result) == 3.0f);
    args[0] = tome_test_string("Ζεύς");
    TOME_EXPECT(tome_test_call("HelloRsGreet", args, 1, &result));
    TOME_EXPECT_STR(tome_test_string_result(result), "Hello from Rust, Ζεύς! (4 letters)");
}
#endif

#ifdef TOME_TEST_WASM_EXAMPLE
static void wasm_mod(void) {
    uint32_t result = 0;
    uint32_t args[2] = {2, 3};
    TOME_EXPECT(tome_test_install(TOME_TEST_MODS_DIR "/hello-wasm"));
    TOME_EXPECT(tome_test_start());
    if (!TOME_EXPECT_STATE("hello-wasm", "active")) return;
    TOME_EXPECT_LOGGED("[hello-wasm] enabled");
    tome_test_load_map();
    TOME_EXPECT(tome_test_call("HelloWasmAdd", args, 2, &result));
    TOME_EXPECT(result == 5);
    args[0] = tome_test_string("Jaina");
    TOME_EXPECT(tome_test_call("HelloWasmGreet", args, 1, &result));
    TOME_EXPECT_STR(tome_test_string_result(result), "Hello, Jaina");
}
#endif

/* The crash fixture crashes where the mode.txt in its folder says. */
static int install_crash(const char* mode) {
    char path[512];
    FILE* file;
    if (!TOME_EXPECT(tome_test_install(TOME_TEST_MODS_DIR "/crash"))) return 0;
    snprintf(path, sizeof(path), "%s\\tome\\mods\\crash\\mode.txt", tome_test_folder());
    file = fopen(path, "w");
    if (!TOME_EXPECT(file != NULL)) return 0;
    fputs(mode, file);
    fclose(file);
    return TOME_EXPECT(tome_test_start());
}

static void crash_in_native(void) {
    uint32_t result = 7;
    uint32_t args[1] = {41};
    if (!install_crash("native")) return;
    if (!TOME_EXPECT_STATE("crash", "active")) return;
    tome_test_load_map();
    /* The call returns (0), the mod is turned off, the test goes on. */
    TOME_EXPECT(tome_test_call("CrashNative", args, 1, &result));
    TOME_EXPECT(result == 0);
    TOME_EXPECT_LOGGED("crash crashed in native CrashNative");
    TOME_EXPECT_STATE("crash", "error");
    TOME_EXPECT(strstr(tome_test_reason("crash"), "crashed") != NULL);
}

static void crash_in_enable(void) {
    if (!install_crash("enable")) return;
    TOME_EXPECT_STATE("crash", "error");
    TOME_EXPECT_LOGGED("crash crashed in tome_mod_enable");
}

static void missing_dependency(void) {
    TOME_EXPECT(tome_test_install(TOME_TEST_SOURCE_DIR "/tests/e2e/mods/needs-ghost"));
    TOME_EXPECT(tome_test_start());
    TOME_EXPECT(strstr(tome_test_reason("needs-ghost"), "\"ghost\"") != NULL);
    TOME_EXPECT_STR(tome_test_state("nobody"), "missing");
}

/* A mod for Reign of Chaos only, tested in that game. */
static void reign_of_chaos(void) {
    TOME_EXPECT(tome_test_install(TOME_TEST_SOURCE_DIR "/tests/e2e/mods/roc-only"));
    TOME_EXPECT(!tome_test_set_edition(7));
    TOME_EXPECT(tome_test_set_edition(TOME_EDITION_REIGN_OF_CHAOS));
    TOME_EXPECT(tome_test_start());
    TOME_EXPECT(!tome_test_set_edition(TOME_EDITION_FROZEN_THRONE)); /* after start */
    TOME_EXPECT_STATE("roc-only", "active");
}

/* The smaller helpers, on the crash fixture in a mode that doesn't crash. */
static void helpers(void) {
    uint32_t result = 0;
    uint32_t args[1] = {1};
    if (!install_crash("none")) return;
    TOME_EXPECT(!tome_test_install(TOME_TEST_SOURCE_DIR "/tests/e2e/mods/needs-ghost")); /* after start */
    TOME_EXPECT_STATE("crash", "active");
    TOME_EXPECT(tome_test_log_count("Loaded crash") == 1);
    TOME_EXPECT(tome_test_reload("crash"));
    TOME_EXPECT(!tome_test_reload("nobody"));
    TOME_EXPECT(!tome_test_reload(NULL));
    TOME_EXPECT(tome_test_log_count("Loaded crash") == 2);
    TOME_EXPECT(tome_test_log_count(NULL) == 0);
    TOME_EXPECT(strstr(tome_test_log(), "Loaded crash") != NULL);
    TOME_EXPECT(!tome_test_has_native("CrashNative")); /* bound at the map load */
    tome_test_load_map();
    TOME_EXPECT(tome_test_has_native("CrashNative"));
    TOME_EXPECT(!tome_test_has_native("NoSuchNative"));
    TOME_EXPECT(!tome_test_has_native(NULL));
    TOME_EXPECT(tome_test_call("CrashNative", args, 1, &result) && result == 2);
    TOME_EXPECT(tome_test_set_setting("crash", "key", "{not json") == TOME_ERR_INVALID_ARGUMENT);
    TOME_EXPECT_LOGGED("tome_test_set_setting(crash, key): not JSON");
}

/* Only with TOME_TEST_SHOW_FAILURES set (the tome_test_failures test):
 * each check fails, and says why. */
static void shows_failures(void) {
    if (!getenv("TOME_TEST_SHOW_FAILURES")) return;
    TOME_EXPECT(tome_test_install(TOME_TEST_SOURCE_DIR "/tests/e2e/mods/needs-ghost"));
    TOME_EXPECT(tome_test_start());
    TOME_EXPECT_LOGGED("a line nobody logs");
    TOME_EXPECT_STATE("needs-ghost", "active");
    TOME_EXPECT_STR("got", "wanted");
    TOME_EXPECT_STR(NULL, "wanted");
}

/* The same, before tome_test_start, and then a crash. */
static void shows_a_crash(void) {
    if (!getenv("TOME_TEST_SHOW_FAILURES")) return;
    TOME_EXPECT(tome_test_install(TOME_TEST_SOURCE_DIR "/tests/e2e/mods/needs-ghost"));
    *(volatile int*)0 = 1;
}

static const tome_test_case cases[] = {
    {"helpers", helpers},
    {"shows_failures", shows_failures},
    {"shows_a_crash", shows_a_crash},
#ifdef TOME_TEST_RUST_EXAMPLE
    {"rust_mod", rust_mod},
#endif
#ifdef TOME_TEST_WASM_EXAMPLE
    {"wasm_mod", wasm_mod},
#endif
    {"crash_in_native", crash_in_native},
    {"crash_in_enable", crash_in_enable},
    {"missing_dependency", missing_dependency},
    {"reign_of_chaos", reign_of_chaos},
};

int main(int argc, char** argv) {
    return TOME_TEST_MAIN(argc, argv, cases);
}
