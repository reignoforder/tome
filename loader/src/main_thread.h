// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Runs work on the game's main thread (docs/research.md §5.6).
//
// Tome subclasses the game window and posts itself a private message; the
// handler runs from the game's message loop, never in the middle of JASS
// execution or a frame. Mod lifecycle changes (enable, disable, unload) go
// through here, so a mod can't be unloaded while the engine is inside one of
// its natives. Needs no reverse engineering, so it works on every build.

#include <windows.h>

#include <functional>

namespace tome::main_thread {

bool install(HWND game_window);
bool installed();
HWND window();

// The game window's client area in pixels, kept from WM_SIZE (0, 0 while
// minimized or before install), so code that runs every frame doesn't ask
// Windows for it. Any thread.
void client_size(int& width, int& height);

// Queues `fn` for the main thread. If the dispatcher isn't installed (no game
// window found), runs it immediately on the calling thread instead.
void post(std::function<void()> fn);

// Called for every WM_KEYDOWN and WM_SYSKEYDOWN of the game window before the
// game sees it; returning true swallows the message. One hook (keybinds.cpp).
using KeyHook = bool (*)(UINT msg, WPARAM wparam, LPARAM lparam);
void set_key_hook(KeyHook hook);

// Called for every WM_ACTIVATEAPP of the game window (the game leaves or
// gets back the foreground) before the game sees it. One hook
// (patches/background_mute.cpp).
using ActivateHook = void (*)(bool active);
void set_activate_hook(ActivateHook hook);

// Called on WM_DESTROY of the game window (the game quits) before the game
// sees it. One hook (patches/background_mute.cpp).
using DestroyHook = void (*)();
void set_destroy_hook(DestroyHook hook);

// Like post(), after at least `delay_ms` milliseconds.
void post_delayed(unsigned delay_ms, std::function<void()> fn);

} // namespace tome::main_thread
