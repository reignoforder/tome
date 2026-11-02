// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Tome's own settings: the mod menu's Tome > Settings page, saved in
// tome/settings.json (tome_settings.h). Described with the same schema as a
// mod's settings (settings.h), so the menu shows them the same way.
//
//   widescreen  bool, default on: the widescreen patch shows more of the map
//               at the sides instead of stretching the 3D view
//               (patches/widescreen.cpp)

#include "settings.h"

namespace tome::tome_settings {

constexpr const char* kWidescreen = "widescreen";

// The schema, parsed once.
const settings::Schema& schema();

// Setting `key`'s boolean value in `values` (from settings::load_values),
// or its default if it's missing or not a bool.
bool get_bool(const json::Value& values, const char* key);

} // namespace tome::tome_settings
