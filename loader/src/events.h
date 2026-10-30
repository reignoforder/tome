#pragma once

// Game events for mods (tome.h, API version 5): map load, game start and
// end, a per-frame tick, the screens other than the main menu, and chat
// commands, including Tome's own /tome. API version 8: the local selection,
// chat messages, pause and resume.
//
// Verified for single player (menus and -loadfile), hosted LAN games, saved
// games and replays (docs/reverse-engineering.md, "Games").
//
// Each comes from one of Tome's own handlers on a mapped game function,
// through the general hook manager (hooks.h):
//   CGlueMgr::SetScreen (after)       a menu screen was built -> on_ui; the loading
//                                     screen is the map load (natives bound, no files
//                                     read yet), the score screen the game end
//   CGameWar3::Reset (before)         game end when leaving without a score screen
//   CGlueMgr::EnterGame (after)       loading finished -> TOME_UI_GAME, game start
//   PresentFrame (before)             tick (per rendered frame)
//   OnNetGameTick (before)            game tick (per simulation step, game time)
//   GameShutdown (before)             game end if the program exits without Reset
//   CGameUI::HandleSlashCommand       chat commands; the line isn't sent
//   CGameUI::DisplayChatMessage       a chat message shown (API version 8)
//   CGameUI::SetPaused (after)        pause and resume
// The selection is compared with the last frame's from the PresentFrame
// handler (units.h).
// Everything runs on the game's main thread.

#include "mappings.h"

#include <tome/tome.h>

#include <cstdint>
#include <string>

namespace tome::events {

// Installs the hooks. Called from DllMain (or the bootstrap thread when
// game.dll loads later), after frames::init. `report` says what happened,
// for the log once it's open.
bool install(uintptr_t game_base, const BuildMappings& build, std::string& report);

// Fills the version 5 part of the API table (unavailable stubs if install didn't run).
void fill_api(tome_api& api);

// Frames presented so far (PresentFrame calls); any thread. Developer
// builds' input scripts (debug_input) count time in frames with it.
uint32_t frames();

// Developer builds: frames presented since the last UI timer tick (every UI
// animation runs on them), so a few in a row without one mean the UI has
// stopped moving. Other builds don't hook the UI timers: there it's every
// frame presented so far.
uint32_t frames_since_ui_tick();

// Developer builds: how many clicks controls have taken (CControl::Click:
// mouse, shortcut keys and Tome's own clicks all go through it). Always 0
// in other builds.
uint32_t control_clicks();

// Shows a message in the game's message area. False outside a game.
bool display_text(const std::string& text, float seconds);

} // namespace tome::events
