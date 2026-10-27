#pragma once

// Popups on the game's screens (core/popup.h has the layout): a queue of
// mods' and Tome's own popups, shown one at a time on whatever screen is up
// (frames::popup_parent), moved to the next screen when that one goes away.
//
// The frames come from TomePopup.fdf, which mod_menu.cpp puts in the
// generated overlay and Tome.toc with its own. They can't
// be destroyed, so each screen gets one set, reused by every popup shown
// there: a full-screen button that takes the clicks meant for what's
// behind, the backdrop, title and message, and one button per slot, each
// with a context of its own. Clicks come through frames::on_click.

#include "core/keys.h"
#include "core/popup.h"

#include <tome/tome.h>

#include <cstdint>
#include <functional>
#include <string>

namespace tome::popups {

// Fills the version 10 part of the API table (with restart_game).
void fill_api(tome_api& api);

// Tome's own popup; `done` gets the choice (TOME_POPUP_CLOSED if it was
// taken away). Any thread.
uint32_t show(popup::Spec spec, std::function<void(int)> done);

// A screen was built or went away: shows the popup there. Main thread.
void on_screen();

// Every frame (main thread, on menus too). A shown popup's frames are shown
// again every 250 ms: once, the first set made after a mod menu click stayed
// hidden although it had been shown (seen once in 13 tries, cause unknown),
// and a popup that can't be seen must not keep taking every key.
void on_frame();

// A key was pressed (main thread), before the mod menu and keybinds see it.
// True if a popup is showing and took it.
bool on_key(keys::Key key, bool repeat);

} // namespace tome::popups
