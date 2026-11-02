/* Tests of the hello example without the game (include/tome/tome_test.h):
 * Tome itself loads the built mod against a fake game, and each case drives
 * it and checks what it did, mostly through its log lines. */
#include <tome/tome_test.h>

/* Every case starts the same way: the mod installed, allowed and turned on. */
static int start(void) {
    return TOME_EXPECT(tome_test_install(TOME_TEST_MODS_DIR "/hello")) && TOME_EXPECT(tome_test_start()) &&
           TOME_EXPECT_STATE("hello", "active");
}

static void loads_and_enables(void) {
    if (!start()) return;
    TOME_EXPECT_LOGGED("[hello] CTextFrame::SetText is a thiscall function");
    TOME_EXPECT_LOGGED("[hello] enabled (1 time this session)");
}

static void natives(void) {
    uint32_t result = 0;
    uint32_t args[1];
    if (!start()) return;
    tome_test_load_map();

    args[0] = 42;
    TOME_EXPECT(tome_test_call("HelloLog", args, 1, NULL));
    TOME_EXPECT_LOGGED("[hello] HelloLog(42) called from JASS");

    args[0] = tome_test_real(1.25f);
    TOME_EXPECT(tome_test_call("HelloDouble", args, 1, &result));
    TOME_EXPECT(tome_test_real_result(result) == 2.5f);

    args[0] = tome_test_string("Ζεύς");
    TOME_EXPECT(tome_test_call("HelloGreet", args, 1, &result));
    TOME_EXPECT_STR(tome_test_string_result(result), "Hello, Ζεύς! (4 letters)");
}

static void code_values(void) {
    uint32_t result = 0;
    uint32_t args[1] = {7};  /* a function of the map script */
    if (!start()) return;
    tome_test_load_map();

    tome_test_set_code_result(7, 0);
    TOME_EXPECT(tome_test_call("HelloCall", args, 1, &result));
    TOME_EXPECT(result == 0);
    TOME_EXPECT_LOGGED("[hello] HelloCall: callback returned 0 (status 0)");

    TOME_EXPECT(tome_test_call("HelloRun", args, 1, NULL));
    TOME_EXPECT(tome_test_code_runs(7) == 2);

    /* Kept until the game starts. */
    TOME_EXPECT(tome_test_call("HelloOnGameStart", args, 1, NULL));
    tome_test_event(TOME_EVENT_GAME_START);
    TOME_EXPECT(tome_test_code_runs(7) == 3);
}

static void chat_command_and_settings(void) {
    if (!start()) return;
    TOME_EXPECT(tome_test_chat("/hello Thrall") == 1);
    TOME_EXPECT_LOGGED("[hello] /hello command: Hello, Thrall. (x1, 5.0 s)");
    TOME_EXPECT(tome_test_chat("/nothing") == 0);

    TOME_EXPECT(tome_test_set_setting("hello", "greeting", "\"Lok'tar\"") == TOME_OK);
    TOME_EXPECT(tome_test_set_setting("hello", "shout", "true") == TOME_OK);
    TOME_EXPECT_LOGGED("[hello] setting changed: greeting");
    TOME_EXPECT_STR(tome_test_setting("hello", "greeting"), "\"Lok'tar\"");
    TOME_EXPECT(tome_test_chat("/hello") == 1);
    TOME_EXPECT_LOGGED("[hello] /hello command: Lok'tar, world! (x1, 5.0 s)");

    /* Out of its range: clamped, as the Mods menu would. The wrong type: refused. */
    TOME_EXPECT(tome_test_set_setting("hello", "repeat", "9") == TOME_OK);
    TOME_EXPECT_STR(tome_test_setting("hello", "repeat"), "5");
    TOME_EXPECT(tome_test_set_setting("hello", "repeat", "\"twice\"") == TOME_ERR_INVALID_ARGUMENT);
    TOME_EXPECT_STR(tome_test_setting("hello", "repeat"), "5");
}

static void keybind(void) {
    if (!start()) return;
    TOME_EXPECT(tome_test_press("hello", "greet"));
    TOME_EXPECT_LOGGED("[hello] Greet key: Hello, world. (x1, 5.0 s)");
}

static void events(void) {
    tome_event_info info;
    if (!start()) return;
    tome_test_event_defaults(TOME_EVENT_DAY, &info);
    info.amount = 6.0f;
    tome_test_event_info(&info);
    TOME_EXPECT_LOGGED("[hello] day breaks at 6.00");
}

static void disable_and_enable(void) {
    uint32_t args[1] = {1};
    if (!start()) return;
    tome_test_load_map();
    TOME_EXPECT(tome_test_enable("hello", 0));
    TOME_EXPECT_STATE("hello", "disabled");
    TOME_EXPECT_LOGGED("[hello] disabled");
    /* Its natives are still bound, and do nothing. */
    tome_test_clear_log();
    TOME_EXPECT(tome_test_call("HelloLog", args, 1, NULL));
    TOME_EXPECT(!tome_test_logged("HelloLog(1)"));
    TOME_EXPECT(tome_test_chat("/hello") == 0);

    TOME_EXPECT(tome_test_enable("hello", 1));
    TOME_EXPECT_LOGGED("[hello] enabled (2 times this session)");
    tome_test_stop();
    TOME_EXPECT_LOGGED("[hello] unloading");
}

static const tome_test_case cases[] = {
    {"loads_and_enables", loads_and_enables},
    {"natives", natives},
    {"code_values", code_values},
    {"chat_command_and_settings", chat_command_and_settings},
    {"keybind", keybind},
    {"events", events},
    {"disable_and_enable", disable_and_enable},
};

int main(int argc, char** argv) {
    return TOME_TEST_MAIN(argc, argv, cases);
}
