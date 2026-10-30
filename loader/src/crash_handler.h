#pragma once

// Crash reports for faults outside the guarded calls into mods
// (docs/research.md §9 item 9; guard.h handles the guarded ones).
//
// game.dll sets an unhandled-exception filter of its own early in its main
// function (0x0199F0, the filter 0x03B590), which writes the game's error
// log and minidump and starts BlizzardError.exe. Tome's filter goes in front
// of it: it writes tome/crashes/crash-<time>.txt (core/crash_report.h) and
// a minidump next to it, logs a line, then hands the exception to the
// game's filter as before. The writing happens on a thread of Tome's own,
// started at bootstrap (a crashed thread may have no stack left, or hold a
// lock), which the filter waits for, at most kWriteTimeoutMs.

#include "core/crash_report.h"
#include "game_build.h"
#include "paths.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tome::crash_handler {

constexpr DWORD kWriteTimeoutMs = 15000;

// DllMain: puts the filter in front of the one there is (the game's).
// Reports go into <paths.tome_dir>/crashes.
void install(const Paths& paths, DWORD main_thread);

// The bootstrap thread: loads dbghelp.dll and starts the writer thread.
// Until then (or without dbghelp) reports are written by the crashed thread
// itself, without a minidump. Nothing without install.
void start(const GameBuild& build);

// The main menu was built: if something replaced Tome's filter since
// install, puts it in front again, ahead of that one.
void check_filter();

// The mods after a refresh (any thread), for the next report: their lines,
// and which loaded DLL is whose (module base, mod id).
void set_mods(std::vector<crash_report::ModLine> mods, std::vector<std::pair<uintptr_t, std::string>> modules);

// A mod's DLL was just loaded (before its tome_mod_load and enable, which
// may start threads of their own): whose it is, until the next set_mods.
void add_module(uintptr_t base, const std::string& id);

// Writes a report for `pointers` (the crash of thread `thread`) as the
// filter does and returns its stem ("crash-20261030-142501"; "" if it
// couldn't). For tests; the filter calls it too.
std::string write_report(EXCEPTION_POINTERS* pointers, DWORD thread);

// Called on the writer thread with the stem after a report was written
// (crash_loop.cpp notes it). One sink.
using ReportSink = void (*)(const std::string& stem);
void set_report_sink(ReportSink sink);

} // namespace tome::crash_handler
