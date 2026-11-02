/* SPDX-License-Identifier: MIT-0 */
/*
 * Tests for mods, without the game.
 *
 * tome_test.dll is the real Tome (the code in tome.mix) running against a
 * fake Warcraft III 1.27b: it installs your built mod in a Tome folder of
 * its own (in %TEMP%), allows it, loads and turns it on as the game would,
 * and lets a test call its natives, send chat commands, raise events,
 * press its keybinds and change its settings, then check what it did,
 * mostly through what it logged. Mods in any language can be tested: the
 * test drives the built mod folder (C, C++, Rust or WebAssembly).
 *
 * What the fake game has: JASS natives (integer, real, boolean, handle,
 * string and code arguments; integer, real, boolean and string results),
 * chat commands, every event (raised by the test, with the fields it
 * fills in), keybinds, settings, assets (they mount, but nothing reads
 * them), hooks (they install, but the game's
 * functions never run), services and messages between mods, and the mod
 * lifecycle (allowing, enabling, disabling, reloading, crashes). What it
 * hasn't: the game's own natives, frames and screens, popups, units, the
 * game's files, a running map script. API functions that need those
 * return an error, as in the game before a map loads.
 *
 * A test is a 32-bit program linked with tome_test.dll; tome_add_mod_test
 * in TomeMod.cmake builds one and adds it to CTest (under Wine on Linux).
 * Tome keeps its state for the whole process, so each test case runs in a
 * process of its own: tome_test_main does that.
 *
 *   static void greets(void) {
 *       uint32_t result;
 *       TOME_EXPECT(tome_test_install(MY_MOD_DIR));
 *       TOME_EXPECT(tome_test_start());
 *       TOME_EXPECT_STATE("my-mod", "active");
 *       tome_test_load_map();
 *       uint32_t args[] = {tome_test_string("Ζεύς")};
 *       TOME_EXPECT(tome_test_call("MyGreet", args, 1, &result));
 *       TOME_EXPECT_STR(tome_test_string_result(result), "Hello, Ζεύς!");
 *   }
 *   static const tome_test_case cases[] = {{"greets", greets}};
 *   int main(int argc, char** argv) { return TOME_TEST_MAIN(argc, argv, cases); }
 *
 * Strings are UTF-8. Strings returned by these functions stay valid until
 * the next call that returns one, unless stated otherwise. Call everything
 * from the test's own thread (it plays the game's main thread).
 */
#ifndef TOME_TOME_TEST_H
#define TOME_TOME_TEST_H

#include "tome.h"

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef TOME_TEST_BUILDING
#define TOME_TEST_API __declspec(dllexport)
#else
#define TOME_TEST_API __declspec(dllimport)
#endif

/* --- Setting up ---------------------------------------------------------- */

/* Copies a mod (a built mod folder, e.g. <build>/mods/my-mod, or a .tome)
 * into the test's tome/mods/, before tome_test_start. Install the mods it
 * depends on too. A Unix path ("/home/...", under Wine) works as well.
 * 1 if copied. */
TOME_TEST_API int TOME_CALL tome_test_install(const char* path);

/* The test's game folder (a Windows path), with Tome's folder in it: the
 * installed mods are in <it>\tome\mods\<folder>, for a test that adds a
 * file to one before tome_test_start. */
TOME_TEST_API const char* TOME_CALL tome_test_folder(void);

/* Starts Tome: it finds the installed mods, and each one is allowed (as
 * if the player had said yes) and turned on, so mods with code load and
 * their tome_mod_load and tome_mod_enable run. 1 if Tome started; whether
 * each mod did is tome_test_state. Once per process. */
TOME_TEST_API int TOME_CALL tome_test_start(void);

/* The game exits: every mod is disabled and unloaded (tome_mod_disable,
 * tome_mod_unload). tome_test_main calls it after each case if the case
 * didn't. */
TOME_TEST_API void TOME_CALL tome_test_stop(void);

/* --- Mods ---------------------------------------------------------------- */

/* A mod's state as the Mods menu shows it: "active", "disabled", "error"
 * (a problem with it, a missing dependency or a crash: tome_test_reason
 * says which), "approval" (not allowed: a changed .tome), "waiting" (a
 * multiplayer game's lock), or "missing" if no installed mod has that id. */
TOME_TEST_API const char* TOME_CALL tome_test_state(const char* id);
/* Why it's in that state ("" if there's nothing to say): an error in its
 * tome.mod.json, a missing dependency, the crash. */
TOME_TEST_API const char* TOME_CALL tome_test_reason(const char* id);
/* Turns a mod off (0) or on (1), as the Mods menu does. 1 if it's known. */
TOME_TEST_API int TOME_CALL tome_test_enable(const char* id, int enabled);
/* Reloads a running mod as a rebuild would (disable, unload, load, enable). */
TOME_TEST_API int TOME_CALL tome_test_reload(const char* id);

/* --- The log ------------------------------------------------------------- */

/* 1 if a line of tome.log since the start (or tome_test_clear_log)
 * contains `text`. Mods' lines start with their id: "[my-mod] ...". */
TOME_TEST_API int TOME_CALL tome_test_logged(const char* text);
/* How many lines since then contain `text`. */
TOME_TEST_API unsigned TOME_CALL tome_test_log_count(const char* text);
/* Forgets the lines so far: later checks only see what's logged after this. */
TOME_TEST_API void TOME_CALL tome_test_clear_log(void);
/* The whole tome.log of this test (the file isn't cleared). */
TOME_TEST_API const char* TOME_CALL tome_test_log(void);

/* --- JASS ---------------------------------------------------------------- */

/* A map loads: the game binds every native (InitNatives), and from now on
 * a map script runs, so natives can return strings and run code values.
 * Raises no event: follow it with tome_test_event(TOME_EVENT_MAP_LOAD) and
 * TOME_EVENT_GAME_START if the mod waits for them. */
TOME_TEST_API void TOME_CALL tome_test_load_map(void);
/* 1 if the game bound a native of that name (a mod's, or Tome's own). */
TOME_TEST_API int TOME_CALL tome_test_has_native(const char* name);
/* Calls a native the way a map script does, with `count` arguments (at
 * most 16) of 32 bits each: integers and booleans as they are, handles as
 * their ids, and reals, strings and code values from the helpers below.
 * `result` (may be NULL) gets what it returned: an integer, a boolean, a
 * real's bits (tome_test_real_result) or a string's id
 * (tome_test_string_result). 1 if the native is bound, 0 if it isn't. A
 * call that crashes still returns 1, with a result of 0: the mod is then
 * turned off, as in the game (tome_test_state says "error"). */
TOME_TEST_API int TOME_CALL tome_test_call(const char* name, const uint32_t* args, size_t count, uint32_t* result);
/* A real argument. Valid for the rest of the test. */
TOME_TEST_API uint32_t TOME_CALL tome_test_real(float value);
/* A string argument (the text is copied). Valid for the rest of the test. */
TOME_TEST_API uint32_t TOME_CALL tome_test_string(const char* text);
/* The text of a string result (jass_string_create's), or NULL for a null
 * string or a value that isn't one. Valid for the rest of the test. */
TOME_TEST_API const char* TOME_CALL tome_test_string_result(uint32_t value);
/* A real result. */
static inline float tome_test_real_result(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}
/* Code arguments are any nonzero number the test picks, standing for a
 * function of the map script. Mods run them with jass_code_call and
 * jass_code_execute: tome_test_code_runs counts the runs of one, and
 * tome_test_set_code_result sets what it returns as a condition (default 1). */
TOME_TEST_API unsigned TOME_CALL tome_test_code_runs(uint32_t code);
TOME_TEST_API void TOME_CALL tome_test_set_code_result(uint32_t code, int result);

/* --- Playing ------------------------------------------------------------- */

/* A line typed in the in-game chat box ("/my-command a b"). 1 if a chat
 * command (a mod's or /tome) took it; 0 if it would have gone to the game. */
TOME_TEST_API int TOME_CALL tome_test_chat(const char* line);
/* Raises `event` (tome_event) for the mods that listen to it, with the
 * fields a real one has when the game says nothing more (map "", players
 * -1, no units...). */
TOME_TEST_API void TOME_CALL tome_test_event(int32_t event);
/* Those fields, for filling in some before tome_test_event_info. */
TOME_TEST_API void TOME_CALL tome_test_event_defaults(int32_t event, tome_event_info* out);
/* Raises `info->event` with these fields (`info->size` = sizeof(*info)).
 * Pointers in it need only stay valid during the call. */
TOME_TEST_API void TOME_CALL tome_test_event_info(const tome_event_info* info);
/* Presses mod `mod`'s keybind `id` (from its tome.mod.json "keybinds"),
 * whatever key it's bound to. 1 if the mod had a callback on it. */
TOME_TEST_API int TOME_CALL tome_test_press(const char* mod, const char* id);
/* Changes a setting as the Mods menu does (validated, saved, then the mod's
 * on_setting_changed runs). `json` is the value: "true", "3", "0.5",
 * "\"text\"". A number out of its range is clamped to it, as in the menu.
 * TOME_ERR_INVALID_ARGUMENT for an unknown setting or a value of the wrong
 * type (the log says why). */
TOME_TEST_API tome_result TOME_CALL tome_test_set_setting(const char* mod, const char* key, const char* json);
/* A setting's current value as JSON ("true", "3", "\"text\""), or NULL if
 * the mod has no such setting. */
TOME_TEST_API const char* TOME_CALL tome_test_setting(const char* mod, const char* key);

/* --- Running cases ------------------------------------------------------- */

typedef struct tome_test_case {
    const char* name;
    void (*run)(void);
} tome_test_case;

/* Runs every case in a process of its own (this program again, with
 * `--run <name>`) and reports each one; returns the exit code: 0 if all
 * passed. `<program> <text>` runs only the cases whose name contains
 * <text>. A failed case keeps its Tome folder; its path is printed. */
TOME_TEST_API int TOME_CALL tome_test_main(int argc, char** argv, const tome_test_case* cases, size_t count);
#define TOME_TEST_MAIN(argc, argv, cases) tome_test_main((argc), (argv), (cases), sizeof(cases) / sizeof((cases)[0]))

/* Records a failure of the running case (it goes on). */
TOME_TEST_API void TOME_CALL tome_test_fail(const char* file, int line, const char* message);
/* The checks below, as functions: each records a failure with what it got
 * (the log's lines for a missing one, the reason for a wrong state) and
 * returns 1 if the check passed. */
TOME_TEST_API int TOME_CALL tome_test_check_logged(const char* file, int line, const char* text);
TOME_TEST_API int TOME_CALL tome_test_check_state(const char* file, int line, const char* id, const char* state);
TOME_TEST_API int TOME_CALL tome_test_check_str(const char* file, int line, const char* got, const char* expected,
                                                const char* what);

#define TOME_EXPECT(cond) \
    ((cond) ? 1 : (tome_test_fail(__FILE__, __LINE__, "TOME_EXPECT(" #cond ")"), 0))
#define TOME_EXPECT_LOGGED(text) tome_test_check_logged(__FILE__, __LINE__, (text))
#define TOME_EXPECT_STATE(id, state) tome_test_check_state(__FILE__, __LINE__, (id), (state))
/* Two strings are equal (NULL only equals NULL). */
#define TOME_EXPECT_STR(got, expected) tome_test_check_str(__FILE__, __LINE__, (got), (expected), #got)

#ifdef __cplusplus
}
#endif

#endif /* TOME_TOME_TEST_H */
