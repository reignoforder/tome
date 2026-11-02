// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The settings part of the mod API (tome.h, API version 7): reading and
// changing a mod's settings, and telling it when they change. The values
// themselves live in the mod manager (ModManager::setting, set_setting).

#include <tome/tome.h>

namespace tome::settings_api {

// Fills the settings functions of the API table and connects the mod
// manager's change notifications to the mods' callbacks.
void fill_api(tome_api& api);

} // namespace tome::settings_api
