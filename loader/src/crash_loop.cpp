// SPDX-License-Identifier: GPL-3.0-or-later
#include "crash_loop.h"

#include "core/crash_report.h"
#include "log.h"
#include "main_thread.h"
#include "popups.h"
#include "unwind.h"

#include <atomic>

namespace tome::crash_loop {
namespace {

Paths g_paths;
bool g_checked = false;
crash_report::Starts g_starts;  // as read by check
bool g_loop = false;            // check() asked for safe mode
std::atomic<bool> g_counting{false};
bool g_reached = false;  // the main menu or a game; main thread
SRWLOCK g_lock = SRWLOCK_INIT;  // the file, between note_crash and the main menu

std::wstring starts_file() { return g_paths.tome_dir + L"\\startups"; }
std::wstring safe_file() { return g_paths.tome_dir + L"\\safe"; }

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

void write_starts() {
    write_file_atomic(starts_file(), crash_report::format_starts(g_starts));
}

std::string last_crash_summary(const std::string& stem) {
    if (stem.empty()) return "";
    std::string text;
    if (!read_file(g_paths.tome_dir + L"\\crashes\\" + widen(stem) + L".txt", text)) return "";
    return crash_report::summary_of(text);
}

void offer_safe_mode(int starts, const std::string& last_report) {
    if (exists(safe_file())) return;  // already kept
    popups::show(crash_report::crash_loop_prompt(starts, last_crash_summary(last_report)), [](int choice) {
        if (choice != crash_report::kKeepSafeMode) {
            TOME_INFO("Crash loop: safe mode for this start only");
            return;
        }
        HANDLE file = CreateFileW(safe_file().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
        if (file != INVALID_HANDLE_VALUE || GetLastError() == ERROR_FILE_EXISTS) {
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            TOME_INFO("Crash loop: created tome/safe; the next starts are in safe mode until it's turned off in "
                      "the mod menu");
        } else {
            TOME_ERROR("Crash loop: couldn't create tome/safe (error %lu)", GetLastError());
        }
    });
}

} // namespace

void settled() {
    int failed = 0;
    std::string last_report;
    {
        ExclusiveLock hold(g_lock);
        if (!g_counting.exchange(false)) return;
        failed = g_starts.count - 1;  // the ones before this
        last_report = g_starts.last_report;
        g_starts = {};
        if (!DeleteFileW(starts_file().c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) write_starts();
    }
    TOME_INFO("Startup check: the game got going; the count of failed starts is back to 0");
    // Popups touch frames: only from the main thread, which needs the
    // dispatcher (without it, post_delayed ran this on a thread of its own).
    if (g_loop && main_thread::installed()) main_thread::post([=] { offer_safe_mode(failed, last_report); });
}

std::string check(const Paths& paths) {
    g_paths = paths;
    g_checked = true;
    g_starts = {};  // a missing or unreadable file counts 0
    std::string text;
    if (read_file(starts_file(), text)) g_starts = crash_report::parse_starts(text);
    g_loop = crash_report::is_crash_loop(g_starts.count);
    return g_loop ? crash_report::safe_mode_reason(g_starts.count) : "";
}

void count_start() {
    if (!g_checked) return;
    ExclusiveLock hold(g_lock);
    ++g_starts.count;
    write_starts();
    g_counting = true;
}

void log_status() {
    if (!g_checked || !g_counting) return;
    int before = g_starts.count - 1;
    if (before == 0) return;
    TOME_WARN("Startup check: the last %d start(s) didn't reach the main menu%s%s", before,
              g_starts.last_report.empty() ? "" : "; last crash report: tome\\crashes\\",
              g_starts.last_report.empty() ? "" : (g_starts.last_report + ".txt").c_str());
}

void note_crash(const std::string& stem) {
    if (!g_counting) return;
    // Not ExclusiveLock: the writer thread has no guard frames, and a crash
    // in the middle of settled() leaves the file as it was.
    if (!TryAcquireSRWLockExclusive(&g_lock)) return;
    if (g_counting) {
        g_starts.last_report = stem;
        write_starts();
    }
    ReleaseSRWLockExclusive(&g_lock);
}

namespace {

void reached() {
    if (g_reached) return;
    g_reached = true;
    if (g_counting) main_thread::post_delayed(kGoodAfterMs, settled);
}

} // namespace

void on_main_menu() {
    reached();
}

void on_game_start() {
    reached();
}

} // namespace tome::crash_loop
