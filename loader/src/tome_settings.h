// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Tome's own settings (core/tome_settings.h) and their file,
// tome/settings.json: read at startup, written when the player changes one
// in the mod menu (Tome > Settings).

#include "core/json.h"
#include "paths.h"

#include <string>

namespace tome::tome_settings {

// Reads tome/settings.json; a missing or invalid file gives the defaults.
void init(const Paths& paths);

// Every setting's value (settings::load_values: the defaults filled in).
json::Value values();

// Sets one (coerced to its schema) and saves the file. False with `error`
// for an unknown key or a value of the wrong type.
bool set(const std::string& key, const json::Value& value, std::string& error);

// The widescreen setting; any thread, cheap enough for every frame.
bool widescreen();

} // namespace tome::tome_settings
