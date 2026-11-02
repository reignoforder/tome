// SPDX-License-Identifier: GPL-3.0-or-later
#include "background_mute.h"

namespace tome {

BackgroundMute::Action BackgroundMute::activate(bool active, bool muted) {
    if (closed_) return Action::none;
    if (!active) {
        if (muted) return Action::none;  // muted already: leave it to whoever did
        muted_by_us_ = true;
        return Action::mute;
    }
    if (!muted_by_us_) return Action::none;
    muted_by_us_ = false;
    // Unmuted from the mixer while away: nothing left to undo.
    return muted ? Action::unmute : Action::none;
}

BackgroundMute::Action BackgroundMute::close(bool muted) {
    Action action = activate(true, muted);
    closed_ = true;
    return action;
}

} // namespace tome
