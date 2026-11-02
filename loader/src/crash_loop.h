// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Crash loops (docs/research.md §9 item 1): a start that never gets to the
// main menu counts in tome/startups (core/crash_report.h); one that shows
// the main menu, or starts a game (-loadfile, the World Editor's Test Map,
// never shows the menu), and runs on for kGoodAfterMs sets the count back to 0. After
// crash_report::kStartsBeforeSafeMode failed starts in a row the next one
// is in safe mode, and its main menu offers to keep safe mode (tome/safe).
// So a mod that crashes the game before the menu never means removing
// tome.mix by hand.
//
// Only starts where Tome can see the main menu count (its hook installed),
// and only in the game, not the World Editor. A start the player closes
// before the menu counts too.

#include "paths.h"

#include <string>

namespace tome::crash_loop {

constexpr unsigned kGoodAfterMs = 5000;

// DllMain, before safe mode is decided: reads tome/startups. Returns the
// safe mode reason if the starts before were a crash loop, otherwise "".
std::string check(const Paths& paths);

// DllMain, once the main menu hook is in: counts this start as failed until
// the main menu has shown.
void count_start();

// Bootstrap thread, once the log is open: logs what check() found.
void log_status();

// A crash report was written (any thread): if this start is still counted,
// it's the crash the safe mode popup names.
void note_crash(const std::string& stem);

// The main menu was built, or a game started (main thread): calls settled
// after kGoodAfterMs, the first time.
void on_main_menu();
void on_game_start();

// The game has run on for a while after either: this start is a good one
// (the count goes back to 0), and after a crash loop the popup offers to
// keep safe mode.
void settled();

} // namespace tome::crash_loop
