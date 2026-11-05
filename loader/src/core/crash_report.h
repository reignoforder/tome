// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Crash loops and crash reports (docs/research.md §9 item 1 and 9), the
// part without Windows APIs: crash_handler.cpp and crash_loop.cpp do the rest.
//
// Crash loops: Tome counts the starts that never reached the main menu in
// tome/startups. After kStartsBeforeSafeMode of them in a row, the next start
// is in safe mode, and the main menu offers to keep it that way.
//
// Crash reports: a fault nothing handled (outside the guarded calls into
// mods, which guard.h handles) gets tome/crashes/crash-<time>.txt (what
// faulted, where, the registers, likely callers, the mods and the log's
// tail) and a minidump next to it, before the game's own error handler runs.

#include "popup.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tome::crash_report {

// --- crash loops ---------------------------------------------------------------

constexpr int kStartsBeforeSafeMode = 3;

// tome/startups: the starts in a row that didn't reach the main menu, and
// the report of the last crash among them ("crash-20261030-142501"; "" if
// none crashed where Tome saw it). A missing or unreadable file counts 0.
struct Starts {
    int count = 0;
    std::string last_report;
};
Starts parse_starts(std::string_view file);
std::string format_starts(const Starts& starts);

// Whether `starts` failed starts in a row call for safe mode.
inline bool is_crash_loop(int starts) { return starts >= kStartsBeforeSafeMode; }

// What safe mode says asked for it ("the last 3 starts didn't reach the main menu").
std::string safe_mode_reason(int starts);

// The main menu's popup after a crash loop: option kKeepSafeMode writes
// tome/safe (safe mode until it's turned off in the mod menu), kThisStart
// leaves it at this start. `last_crash` is the summary (summary_of) of
// Starts::last_report, "" if there's none.
constexpr int kKeepSafeMode = 0, kThisStart = 1;
popup::Spec crash_loop_prompt(int starts, std::string_view last_crash);

// --- reports -------------------------------------------------------------------

// "access violation", "stack overflow", ...; nullptr for codes it doesn't know.
const char* exception_name(uint32_t code);

// What the exception was: "access violation reading 0x00000004",
// "stack overflow", "exception 0xe0000001".
std::string describe_exception(uint32_t code, const uint32_t* info, uint32_t info_count);

// An address as "game.dll+0x1a2b" (and " (mod hello)" in a mod's DLL), or bare.
struct Location {
    uint32_t address = 0;
    std::string module;  // file name; "" outside any module
    uint32_t offset = 0;
    std::string mod;     // id of the mod whose DLL it is; "" if none
};
std::string where(const Location& at);

struct ModLine {
    std::string id, version, state;
};

struct Report {
    std::string time;        // "2026-10-30 14:25:01"
    std::string tome_version;
    std::string game;        // "1.27b (game.dll 1.27.1.7085)", or what's known
    uint32_t code = 0;
    std::vector<uint32_t> info;  // ExceptionInformation
    Location at;             // the faulting instruction
    uint32_t thread = 0;
    bool main_thread = false;
    // eax ebx ecx edx esi edi ebp esp eip eflags
    uint32_t registers[10] = {};
    std::vector<Location> callers;  // likely return addresses on the stack, innermost first
    std::vector<ModLine> mods;      // the mods as of the last refresh; empty if unknown
    bool mods_known = false;
    std::string dump;        // the minidump's file name; "" if it couldn't be written
    std::string dump_error;  // why not
    std::string log_tail;
};

// The one-line summary: "access violation reading 0x00000004 at game.dll+0x1a2b".
std::string summary(const Report& report);

// The whole report file. Its first line is "Summary: " + summary().
std::string format(const Report& report);

// The summary from a report file's text; "" if it has none.
std::string summary_of(std::string_view report_text);

// The last `lines` lines of `log` (all of it if it has fewer).
std::string tail(std::string_view log, size_t lines);

// "crash-20261030-142501"; names sort by time.
std::string file_stem(int year, int month, int day, int hour, int minute, int second);

// Of the files in tome/crashes (names), the ones to delete so that `keep`
// reports (a stem's .txt and .dmp) are left, the newest. Other names stay.
constexpr size_t kKeepReports = 5;
std::vector<std::string> to_remove(std::vector<std::string> names, size_t keep = kKeepReports);

} // namespace tome::crash_report
