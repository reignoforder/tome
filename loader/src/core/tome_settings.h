// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Tome's own settings: the mod menu's Tome > Settings page, saved in
// tome/settings.json (tome_settings.h). Described with the same schema as a
// mod's settings (settings.h), so the menu shows them the same way.
//
//   widescreen  bool, default on: the widescreen patch shows more of the map
//               at the sides instead of stretching the 3D view
//               (patches/widescreen.cpp)
//   update_check  bool, default off: asks GitHub once a day for a newer Tome
//               (update_check.cpp); asked on the first start, while the file
//               doesn't have it
//   update_install  bool, default off: with update_check, installs a signed
//               newer version as soon as it's found instead of offering
//               Update Now; it runs from the next start
//   harden_heap  bool, default off: at startup, turns on Windows' heap
//               termination on corruption for the game process
//               (HeapEnableTerminationOnCorruption; bootstrap.cpp); from the
//               next start, since it can't be turned off in a running process
//   patch.<name>  bool, default on: one per built-in patch (patches.h) without
//               a setting above of its own name (widescreen has one), after
//               the settings above (with_patches). Off, the patch isn't
//               applied from the next start; -tomenopatch=<name>[,<name>...]
//               (or =all) on the command line does the same for one start
//               (patch_off_reason)

#include "settings.h"

#include <string>
#include <string_view>
#include <vector>

namespace tome::tome_settings {

constexpr const char* kWidescreen = "widescreen";
constexpr const char* kUpdateCheck = "update_check";
constexpr const char* kUpdateInstall = "update_install";
constexpr const char* kHardenHeap = "harden_heap";
constexpr const char* kPatchPrefix = "patch.";
// The command-line option that turns patches off for one start.
constexpr const wchar_t* kNoPatchOption = L"-tomenopatch";

// The schema, parsed once.
const settings::Schema& schema();

// Setting `key`'s boolean value in `values` (from settings::load_values),
// or its default if it's missing or not a bool.
bool get_bool(const json::Value& values, const char* key);

// The same with the defaults of `schema` (e.g. with_patches').
bool get_bool(const settings::Schema& schema, const json::Value& values, const char* key);

// A built-in patch, as its setting describes it.
struct PatchInfo {
    std::string_view name;     // e.g. "memhack-guard"
    std::string_view summary;  // patches::Patch::summary: lowercase, no full stop
    std::string now;           // its state this start ("In force."), first in the description; "" for none
};

// "patch.<name>": the setting of patch `name`.
std::string patch_key(std::string_view name);

// `base` followed by a setting per patch (in the order given), default on;
// none for a patch named like a setting of `base` (that setting governs it).
settings::Schema with_patches(const settings::Schema& base, const std::vector<PatchInfo>& patches);

// Why patch `name` stays off this start, or "" to apply it: `flags` (the
// -tomenopatch values, command_line::list_values) names it or "all", or
// its setting in `values` is false.
std::string patch_off_reason(std::string_view name, const std::vector<std::string>& flags, const json::Value& values);

} // namespace tome::tome_settings
