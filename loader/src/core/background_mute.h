// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// When background-mute mutes and unmutes the game's audio
// (patches/background_mute.cpp, docs/known-bugs.md "Under Wine"). Kept here,
// apart from the Core Audio calls, so the edges can be tested.
//
// The patch feeds it every WM_ACTIVATEAPP of the game window. Leaving mutes
// the audio session unless it's muted already (by the user, in the system
// mixer); coming back unmutes it only if Tome was the one that muted it, so
// a mute the user set stays. Repeated messages in the same direction change
// nothing. When the window is destroyed (the game quits), close() undoes
// Tome's mute: Windows would keep it for the next run, and destroying the
// active window deactivates the game first, which muted it.

namespace tome {

class BackgroundMute {
public:
    enum class Action { none, mute, unmute };

    // The window was activated or deactivated; `muted` is the session's mute
    // state right now. Returns what to do to it.
    Action activate(bool active, bool muted);

    // The game window is being destroyed: undoes Tome's mute, and nothing
    // mutes after it.
    Action close(bool muted);

    // Whether Tome muted the session and hasn't unmuted it yet.
    bool muted_by_us() const { return muted_by_us_; }

private:
    bool muted_by_us_ = false;
    bool closed_ = false;
};

} // namespace tome
