#include "core/background_mute.h"
#include "test.h"

using namespace tome;
using Action = BackgroundMute::Action;

TEST(background_mute_round_trip) {
    BackgroundMute m;
    CHECK(m.activate(false, false) == Action::mute);
    CHECK(m.muted_by_us());
    CHECK(m.activate(true, true) == Action::unmute);
    CHECK(!m.muted_by_us());
}

TEST(background_mute_keeps_the_users_mute) {
    // Muted in the system mixer before leaving: coming back leaves it muted.
    BackgroundMute m;
    CHECK(m.activate(false, true) == Action::none);
    CHECK(!m.muted_by_us());
    CHECK(m.activate(true, true) == Action::none);
}

TEST(background_mute_repeated_messages) {
    BackgroundMute m;
    CHECK(m.activate(true, false) == Action::none);  // activated at startup
    CHECK(m.activate(false, false) == Action::mute);
    CHECK(m.activate(false, true) == Action::none);  // our own mute
    CHECK(m.muted_by_us());
    CHECK(m.activate(true, true) == Action::unmute);
    CHECK(m.activate(true, false) == Action::none);
}

TEST(background_mute_unmuted_while_away) {
    // The user unmuted it from the mixer while the game was in the background.
    BackgroundMute m;
    CHECK(m.activate(false, false) == Action::mute);
    CHECK(m.activate(true, false) == Action::none);
    CHECK(!m.muted_by_us());
    CHECK(m.activate(false, false) == Action::mute);
}

TEST(background_mute_close_undoes_our_mute) {
    // Destroying the active window deactivates the game first.
    BackgroundMute m;
    CHECK(m.activate(false, false) == Action::mute);
    CHECK(m.close(true) == Action::unmute);
    CHECK(!m.muted_by_us());
    CHECK(m.activate(false, false) == Action::none);  // nothing after close
}

TEST(background_mute_close_keeps_the_users_mute) {
    BackgroundMute m;
    CHECK(m.activate(false, true) == Action::none);
    CHECK(m.close(true) == Action::none);
    BackgroundMute n;
    CHECK(n.close(false) == Action::none);  // in the foreground, not muted
}
