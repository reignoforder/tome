#pragma once

// The mod menu (docs/research.md §8), built from the game's own UI frames so
// it looks native: a "Mods" button on the main menu that opens a panel in
// the chained panel model, and one in the in-game Esc menu that opens the
// same pages in place of its Help panel. The pages (mod list, details,
// settings, keybinds, Tome and safe mode) come from core/mod_menu_model.h.
//
// How it gets there: Tome writes its own TomeModMenu.fdf into the generated
// overlay (overlay.h), and has the game parse it on top of its own
// frame definitions. When the main menu is set up (a CMainMenu vtable hook),
// Tome creates its frames inside the menu, binds the buttons, and receives
// their clicks through the menu's event handler (a second vtable hook) and
// Tome's observer (frames::on_click). In games, hooks on the Esc menu's
// Setup and SetPanel do the same. The game's own UI files are never modified.

#include "core/keys.h"
#include "mappings.h"

#include <cstdint>
#include <string>

namespace tome::mod_menu {

// Installs the hooks. Safe to call from DllMain; does no file I/O itself.
// Call frames::init first.
bool install_hooks(uintptr_t game_base, const BuildMappings& build, std::string& report);

// The game has usually loaded FrameDef.toc before Miles loads Tome. If so,
// this mounts the overlay and loads Tome's definitions now. Must run on the
// game's main thread, before the main menu is built.
void reload_frame_definitions(std::string& report);

// The Esc menu's hooks, through the hook manager (call after hooks::init).
bool install_game_hooks(std::string& report);

// Refreshes the panel if it's open, e.g. after mods changed on disk.
// Main thread only.
void on_mods_changed();

// A key was pressed (main thread), before anything else sees it. True if
// the menu took it: the new key for a keybind, Esc (back one page; on the
// list, closes the menu), Enter in one of its text boxes (saves it).
bool on_key(keys::Key key, bool repeat);

// Tome's "Open the mod menu" keybind: on the main menu, opens or closes the
// panel; in a game, opens the Esc menu with Mods showing. False if neither
// is possible right now.
bool open_from_key();

} // namespace tome::mod_menu
