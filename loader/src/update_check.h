// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The update check (core/update_check.h has the rest): the first time the
// main menu shows, asks whether to check if tome/settings.json doesn't say,
// and then, if allowed and a day has passed, asks GitHub for Tome's latest
// release over WinHTTP on a thread of its own. A newer version gets a popup
// once (with a button for its release page) and the Tome page's "Updates:"
// line. tome/update.json keeps what the last check found.
//
// Developer builds (TOME_DEV) never ask or check unless
// TOME_DEBUG_UPDATE_URL names the URL to ask instead of GitHub's (http://
// allowed there), so tests and tools/game.sh don't see the question.

#include "paths.h"

#include <string>

namespace tome::update_check {

// Reads tome/update.json and rolls back an update that didn't start
// (core/update_check.h, should_roll_back). Bootstrap, as early as the log is
// open (before the mods' first load); doesn't need tome_settings.
void init(const Paths& paths);

// The main menu was set up (main thread). The first time only: the question,
// or a check if one is due, or the popup for a newer version found before.
void on_main_menu();

// One of Tome's settings changed (main thread): turning the check on checks
// now if one is due.
void on_setting_changed(const std::string& key);

// The file this tome.mix's code was loaded from: tome.mix, or tome.mix.old
// once an update this session has put the new version in its place (the
// module's own path, GetModuleFileName, then names the new file). "" if
// unknown. Thread-safe.
std::wstring loaded_file();

// The Tome page's "Updates:" line.
std::string status();

} // namespace tome::update_check
