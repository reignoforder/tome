// SPDX-License-Identifier: GPL-3.0-or-later
#include "context_restore.h"

namespace tome {

bool ContextRestore::frame(bool can_draw, bool fullscreen) {
    if (!can_draw) {
        if (drawn_ && fullscreen) pending_ = true;
        return false;
    }
    drawn_ = true;
    return pending_;
}

void ContextRestore::rebuilt(bool ok) {
    if (ok) {
        pending_ = false;
        failures_ = 0;
    } else {
        ++failures_;
    }
}

} // namespace tome
