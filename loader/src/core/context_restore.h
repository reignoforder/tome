// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// When gl-context-restore makes the OpenGL context again
// (patches/gl_context_restore.cpp, docs/known-bugs.md "Under Wine"). Kept
// here, apart from the hook, so the edges can be tested.
//
// The patch feeds it every CGxDeviceOpenGl::CanDraw result. A rebuild is due
// on the first frame that can draw after one that couldn't, but only if a
// frame was drawn before (nothing to lose at startup) and the device was
// fullscreen when it stopped drawing (Wine drops the context there, not in a
// window that's minimized). A rebuild that fails stays due, so the next
// frame that can draw tries again instead of drawing with no context.

#include <cstdint>

namespace tome {

class ContextRestore {
public:
    // One frame: CanDraw's result and whether the device is fullscreen.
    // Returns true when the context should be made again now.
    bool frame(bool can_draw, bool fullscreen);

    // How the rebuild frame() asked for went. Success clears it; a failure
    // keeps it due for the next frame.
    void rebuilt(bool ok);

    // Rebuilds that failed since the last one that worked.
    uint32_t failures() const { return failures_; }

private:
    bool drawn_ = false;    // a frame was drawn since startup
    bool pending_ = false;  // a rebuild is due
    uint32_t failures_ = 0;
};

} // namespace tome
