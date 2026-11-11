// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Tome's own settings (core/tome_settings.h) and their file,
// tome/settings.json: read at startup, written when the player changes one
// in the mod menu (Tome > Settings).

#include "core/json.h"
#include "core/settings.h"
#include "paths.h"

#include <string>
#include <string_view>

namespace tome::tome_settings {

// Reads tome/settings.json and the -tomenopatch option without logging:
// safe in DllMain, where the patches need them (patch_off). A missing or
// invalid file gives the defaults; so does null `paths` (Tome's folders
// couldn't be found), the option still counting.
void load(const Paths* paths);

// Logs what's wrong in the file and the settings in force, as load read
// them (in DllMain), and reads harden_heap for this start.
void init();

// Tome's settings with one per built-in patch (core/tome_settings.h,
// with_patches): what the mod menu shows and set() accepts.
const settings::Schema& schema_with_patches();

// schema_with_patches with each patch's state this start (in force, or why
// not: patches::outcome) first in its description: what the mod menu shows,
// since a setting's value only says what the next start does.
settings::Schema menu_schema();

// Why built-in patch `name` stays off this start, or "" to apply it
// (patches::OffReason): its setting, or -tomenopatch, as load read them.
std::string patch_off(std::string_view name);

// Every setting's value (settings::load_values: the defaults filled in).
json::Value values();

// Sets one (coerced to its schema) and saves the file. False with `error`
// for an unknown key or a value of the wrong type.
bool set(const std::string& key, const json::Value& value, std::string& error);

// The widescreen setting; any thread, cheap enough for every frame.
bool widescreen();

// The update_check setting; any thread, without copying values().
bool update_check_on();

// The update_install setting; the same.
bool update_install_on();

// The harden_heap setting, as read at startup (init): a change applies from
// the next start.
bool harden_heap_at_start();

// Whether tome/settings.json has `key` (the player chose it, or was asked).
bool stored(const std::string& key);

} // namespace tome::tome_settings
