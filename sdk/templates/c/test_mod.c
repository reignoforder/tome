/*
 * The mod's tests, without the game (tome_test.h): Tome loads the built mod
 * against a fake game. Run them with ctest (under Wine on Linux).
 */
#include <tome/tome_test.h>

static void adds(void) {
    uint32_t args[2] = {2, 3}, result = 0;
    TOME_EXPECT(tome_test_install(TOME_TEST_MODS_DIR "/my-c-mod"));
    TOME_EXPECT(tome_test_start());
    if (!TOME_EXPECT_STATE("my-c-mod", "active")) return;
    tome_test_load_map();
    TOME_EXPECT(tome_test_call("MyCModAdd", args, 2, &result));
    TOME_EXPECT(result == 5);
}

static void logs_the_game_start(void) {
    tome_event_info info;
    TOME_EXPECT(tome_test_install(TOME_TEST_MODS_DIR "/my-c-mod"));
    TOME_EXPECT(tome_test_start());
    tome_test_event_defaults(TOME_EVENT_GAME_START, &info);
    info.map_name = "Echo Isles";
    tome_test_event_info(&info);
    TOME_EXPECT_LOGGED("[my-c-mod] game started on Echo Isles");
    TOME_EXPECT(tome_test_chat("/mycmod hi") == 1);
}

static const tome_test_case cases[] = {
    {"adds", adds},
    {"logs_the_game_start", logs_the_game_start},
};

int main(int argc, char** argv) {
    return TOME_TEST_MAIN(argc, argv, cases);
}
