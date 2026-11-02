#pragma once

// Keybinds (tome.h, API version 7; core/keys.h): every installed mod's
// declared keybinds with the player's changes from tome/keybinds.json, the
// keyboard hook that runs them, and the API functions.
//
// Keys come from the game window's messages (WM_KEYDOWN, WM_SYSKEYDOWN)
// through Tome's subclass (main_thread.h), before the game sees them. The
// mod menu gets them first while it waits for a new key; otherwise a key
// bound to a running mod's callback runs it and doesn't reach the game.
// Nothing runs while the player types in the chat box or an edit box.

#include "core/keys.h"
#include "paths.h"

#include <tome/tome.h>

#include <windows.h>

#include <string>
#include <vector>

namespace tome::keybinds {

// Reads tome/keybinds.json and installs the keyboard hook. Main thread
// (after main_thread::install).
void init(const Paths& paths);

// Fills the keybind functions of the API table.
void fill_api(tome_api& api);

// The mod manager's list of every installed mod's declared keybinds (key =
// fallback = the default), before the mods start. Any thread; this module
// never calls back into the mod manager, whose lock may be held.
void set_mods(std::vector<keys::Binding> declared);

// Every installed mod's keybinds, in mod order, with conflicts filled in.
std::vector<keys::Binding> list();

// The player chose `key` for a binding (or keys::Key{} to unbind); saved to
// tome/keybinds.json. Main thread.
void set(const std::string& mod, const std::string& id, keys::Key key);

// Every binding back to its mod's default.
void reset_all();

// Runs the callback of mod `mod`'s keybind `id` as a press of its key would,
// for the fake game of tome_test.dll (no keyboard). False if none is registered.
bool press_for_test(const std::string& mod, const std::string& id);

} // namespace tome::keybinds
