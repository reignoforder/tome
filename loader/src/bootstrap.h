// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <windows.h>

namespace tome {

// Called from DllMain. Must not do real work (loader lock is held); it only
// starts the bootstrap thread.
void start_bootstrap(HMODULE self);

} // namespace tome
