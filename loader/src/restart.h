#pragma once

// Restarting the game (the mod API's restart_game, and the mod menu's
// "restart required" popup): Tome closes the game window like Alt+F4 (in a
// game, it presses Exit on the Esc menu's Confirm Exit panel, which closing
// the window would show), so the game shuts down as usual and every mod is
// disabled and unloaded. Once the game's shutdown has run, Tome starts a
// helper that waits for this process to exit, then starts war3.exe again
// with the same command line.
// It has to wait outside the game: war3.exe refuses to start while another
// instance runs ("unable to initialize"), before it ever loads Tome.
//
// The helper is tome.mix itself, run by rundll32 (TomeRestartW below);
// DllMain does nothing else in that process.

#include <windows.h>

namespace tome::restart {

// Asks for a restart on behalf of `who` (for the log). Main thread. False if
// there's no game window to close.
bool request(const char* who);

// From the game's shutdown, after the mods were unloaded: starts the helper
// if a restart was asked for.
void on_shutdown();

// Whether this process is the helper (rundll32), where Tome must not start.
bool is_helper_process();

} // namespace tome::restart

// rundll32 "<tome.mix>",TomeRestart <pid>|<exe>|<command line>: waits for
// process <pid> to exit (at most 60 s), then starts <exe> with <command line>.
extern "C" __declspec(dllexport) void CALLBACK TomeRestartW(HWND, HINSTANCE, LPWSTR args, int);
