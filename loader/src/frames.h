// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The game's UI frames, for Tome's own menu and for mods (the frame part of
// the tome_api table, API version 3, and the 1.31-named JASS natives).
//
// Mods create their frames from on_ui callbacks: TOME_UI_MAIN_MENU runs right
// after the main menu is built (the CMainMenu Setup hook in mod_menu.cpp),
// the other screens when events.cpp sees the game build them. Clicks reach a
// CObserver of Tome's own, registered on each button with the command id
// Tome hands out.
//
// Everything that touches a frame runs on the game's main thread. Before
// calling a method that only some frame classes have, Tome checks the class
// through the game's RTTI (core/rtti.h).

#include "mappings.h"

#include <tome/tome.h>

#include <cstdint>
#include <functional>
#include <string>

namespace tome::frames {

// Binds the game functions and declares the JASS natives. Called from
// DllMain on the main thread, after natives::install_hooks.
void init(uintptr_t game_base, const BuildMappings& build, uint32_t main_thread_id);
bool ready();
bool on_main_thread();
// Without init (the fake game of tome_test.dll and tome-check): the thread
// the API's main-thread checks accept.
void set_main_thread(uint32_t main_thread_id);

// Fills the version 3 part of the API table (unavailable stubs if init didn't run).
void fill_api(tome_api& api);

// --- for Tome's own UI (main thread) -----------------------------------------

void* get(const char* name, int context = 0);
void* create(const char* definition, void* parent, int priority, int context);
bool set_point(void* frame, int point, void* relative, int relative_point, float x, float y);
bool set_text(void* frame, const char* text);  // false unless it's a text frame
bool set_width(void* frame, float width);
void set_visible(void* frame, bool visible);
bool set_animation(void* sprite, const int* tags, int count);  // false unless it's a sprite frame
bool load_toc(const char* path);

// Calls `fn` when `button` (a control registered as `name`, `context`) is
// clicked, through the same observer as mods' buttons. Replaces an earlier
// callback for the same button. False if it isn't a control.
bool on_click(void* button, const char* name, int context, std::function<void()> fn);

// Clicks a control as the player would (its listeners hear about it). False
// unless it's a control.
bool click(void* control);
// True if CControl::Click on the control would reach its listeners (it has
// a click registration, CObserver::HasEvent); a disabled one has none.
// Main thread.
bool takes_clicks(void* control);
// Gives the frame the keyboard focus (CFrame::SetFocus); true if it took it.
// Main thread.
bool set_focus(void* frame);

// True while a game's UI is up (main thread).
bool game_showing();

// Whether a game object's class (from its RTTI) is `name` or derives from it.
bool is_class(void* object, const char* name);

// True while the keyboard goes to a text box (the chat box, an edit box).
bool typing();

// The frame with the keyboard focus, or null; and taking it away.
void* focused();
void clear_focus();

// An edit box's text (UTF-8), and replacing it (no change event). False
// unless it's an edit box.
bool edit_text(void* frame, std::string& out);
bool set_edit_text(void* frame, const char* text);

// --- screens (mod_menu.cpp, events.cpp) ----------------------------------------

// After the game's Setup and Tome's own frames: runs the mods' callbacks.
void on_main_menu_setup(void* menu);

// Another screen (a tome_ui_event other than the main menu) was just built
// with `root` as its frame: runs the mods' callbacks for it.
void on_screen(int32_t ui_event, void* root);

// True while the main menu from the last Setup still exists and no map has
// loaded since (main thread).
bool main_menu_showing();

// Where a popup goes (popups.cpp): the root frame of the screen showing now,
// with `in_game` set for the game's own UI; the main menu's ControlLayer; or
// null while there's none to use (the loading screen, before the first menu).
void* popup_parent(bool& in_game);

// Runs callbacks that mods registered off the main thread while their screen
// already existed (the first mod load). Post it once the dispatcher runs.
void catch_up();

} // namespace tome::frames
