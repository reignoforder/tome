#pragma once

#include <windows.h>

namespace tome {

// Called from DllMain. Must not do real work (loader lock is held); it only
// starts the bootstrap thread.
void start_bootstrap(HMODULE self);

} // namespace tome
