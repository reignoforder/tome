#pragma once

#include "paths.h"

#include <functional>

namespace tome {

// Watches tome/state.json and everything under mods/, and calls `on_change`
// (on the watcher thread) once changes have settled for a moment. Under Wine,
// which misses changes inside subfolders, it's also called every couple of
// seconds as a fallback, so `on_change` must be cheap when nothing changed.
bool start_watcher(const Paths& paths, std::function<void()> on_change);

} // namespace tome
