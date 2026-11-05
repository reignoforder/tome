// SPDX-License-Identifier: GPL-3.0-or-later
// Crash loops (crash_loop.h) and crash reports (crash_handler.h): the
// startup count in tome/startups, safe mode after a loop, and reports
// written for a crash, also a real one nothing handled.
#include "harness.h"
#include "test.h"

#include "core/crash_report.h"
#include "crash_handler.h"
#include "crash_loop.h"
#include "log.h"
#include "paths.h"

#include <windows.h>

using host::Home;
using namespace tome;

namespace {

crash_report::Starts read_starts(const Home& home) {
    std::string text;
    if (!home.read(L"tome\\startups", text)) return {};
    return crash_report::parse_starts(text);
}

// An access violation reading 0x4 at `at`, as Windows describes one.
struct FakeCrash {
    EXCEPTION_RECORD record = {};
    CONTEXT context = {};
    EXCEPTION_POINTERS pointers = {&record, &context};

    explicit FakeCrash(void* at) {
        RtlCaptureContext(&context);
        record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
        record.ExceptionAddress = at;
        record.NumberParameters = 2;
        record.ExceptionInformation[0] = 0;
        record.ExceptionInformation[1] = 4;
    }
};

__attribute__((noinline)) void crash_site() {
    asm volatile("" ::: "memory");
}

} // namespace

// Starts that don't reach the main menu count up; the third in a row asks
// for safe mode; a start that shows the main menu sets the count back.
TEST(host_crash_loop_count) {
    Home home;
    host::open_log(home);
    CHECK_EQ(crash_loop::check(home.paths()), std::string());
    crash_loop::count_start();
    CHECK_EQ(read_starts(home).count, 1);

    // The starts before were cut short twice: not a loop yet.
    CHECK(home.write(L"tome\\startups", "2\n\n"));
    CHECK_EQ(crash_loop::check(home.paths()), std::string());

    CHECK(home.write(L"tome\\startups", "3\ncrash-20261030-142501\n"));
    CHECK_EQ(crash_loop::check(home.paths()), std::string("the last 3 starts didn't reach the main menu"));
    crash_loop::count_start();
    crash_loop::log_status();
    CHECK_LOG("the last 3 start(s) didn't reach the main menu; last crash report: "
              "tome\\crashes\\crash-20261030-142501.txt");
    crash_report::Starts starts = read_starts(home);
    CHECK_EQ(starts.count, 4);
    CHECK_EQ(starts.last_report, std::string("crash-20261030-142501"));

    // A crash during this start: the popup will name it.
    crash_loop::note_crash("crash-20261030-150000");
    CHECK_EQ(read_starts(home).last_report, std::string("crash-20261030-150000"));

    crash_loop::settled();
    CHECK(!home.exists(L"tome\\startups"));
    CHECK_LOG("Startup check: the game got going; the count of failed starts is back to 0");
    // Later crashes in this session aren't startup crashes.
    crash_loop::note_crash("crash-20261030-160000");
    CHECK(!home.exists(L"tome\\startups"));
}

// The World Editor and starts Tome can't follow to the main menu don't count.
TEST(host_crash_loop_uncounted) {
    Home home;
    crash_loop::note_crash("crash-20261030-150000");  // not counting
    crash_loop::count_start();  // before check(): nothing to count against
    CHECK(!home.exists(L"tome\\startups"));
    CHECK_EQ(crash_loop::check(home.paths()), std::string());
    crash_loop::settled();  // never counted
    CHECK(!home.exists(L"tome\\startups"));
}

// A report for a crash in a mod's DLL (here the test program stands in for
// one): what, where, whose, the mods, the log's tail and a minidump.
TEST(host_crash_report_written) {
    Home home;
    host::open_log(home);
    crash_loop::check(home.paths());
    crash_loop::count_start();
    crash_handler::install(home.paths(), GetCurrentThreadId());
    crash_handler::set_report_sink(crash_loop::note_crash);
    GameBuild build;
    build.file_version = "1.27.1.7085";
    crash_handler::start(build);
    CHECK_LOG("Crash reports: tome\\crashes");
    crash_handler::set_mods({{"fake", "1.0.0", "active"}, {"other", "2.0.0", "disabled"}},
                            {{reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)), "fake"}});
    TOME_INFO("the last line before the crash");

    FakeCrash crash(reinterpret_cast<void*>(&crash_site));
    std::string stem = crash_handler::write_report(&crash.pointers, GetCurrentThreadId());
    CHECK(!stem.empty());
    std::string text;
    CHECK(home.read(L"tome\\crashes\\" + widen(stem) + L".txt", text));
    std::string summary = crash_report::summary_of(text);
    CHECK(summary.find("access violation reading 0x00000004 at tome_host_tests.exe+0x") == 0);
    CHECK(summary.find("(mod fake)") != std::string::npos);
    CHECK(text.find("Game: game.dll 1.27.1.7085") != std::string::npos);
    CHECK(text.find("(the game's main thread)") != std::string::npos);
    CHECK(text.find("  other                     2.0.0       disabled\n") != std::string::npos);
    CHECK(text.find("the last line before the crash\n") != std::string::npos);
    CHECK(text.find("Minidump: " + stem + ".dmp") != std::string::npos);
    CHECK(home.exists(L"tome\\crashes\\" + widen(stem) + L".dmp"));
    CHECK_LOG("Crash outside a mod call: access violation reading 0x00000004 at tome_host_tests.exe+0x");
    // The start hadn't reached the main menu: the popup will name this one.
    CHECK_EQ(read_starts(home).last_report, stem);

    // Old reports go: five are kept, this one among them.
    CHECK(home.write(L"tome\\crashes\\notes.txt", "mine"));
    for (int i = 0; i < 6; ++i) {
        CHECK(home.write(L"tome\\crashes\\" + widen(crash_report::file_stem(2020, 1, 1, 0, 0, i)) + L".txt", "x"));
    }
    std::string second = crash_handler::write_report(&crash.pointers, GetCurrentThreadId());
    CHECK(!second.empty());
    CHECK(home.exists(L"tome\\crashes\\notes.txt"));
    CHECK(!home.exists(L"tome\\crashes\\crash-20200101-000000.txt"));
    CHECK(home.exists(L"tome\\crashes\\crash-20200101-000005.txt"));
    CHECK(home.exists(L"tome\\crashes\\" + widen(second) + L".txt"));
}

namespace {

Home* g_unhandled_home = nullptr;

// The filter that was there before Tome's (the game's, in the game): Tome's
// ran first and wrote its report, then handed the crash on.
LONG WINAPI after_tome(EXCEPTION_POINTERS* pointers) {
    CHECK_EQ(pointers->ExceptionRecord->ExceptionCode, DWORD(EXCEPTION_ACCESS_VIOLATION));
    CHECK_LOG("Crash outside a mod call: access violation writing 0x00000008");
    std::string text;
    bool found = false;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW((g_unhandled_home->paths().tome_dir + L"\\crashes\\*.txt").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        found = g_unhandled_home->read(L"tome\\crashes\\" + std::wstring(data.cFileName), text);
        FindClose(find);
    }
    CHECK(found);
    CHECK(text.find("Summary: access violation writing 0x00000008 at tome_host_tests.exe+0x") == 0);
    CHECK(text.find("Called from (likely, innermost first):\n  tome_host_tests.exe+0x") != std::string::npos);
    delete g_unhandled_home;  // removes it if the checks passed
    std::fflush(stdout);
    ExitProcess(test::failures() == 0 ? 0 : 1);
}

// Read at run time, so the compiler can't see the null pointer (and put a trap there instead).
volatile int* volatile g_null = nullptr;

__attribute__((noinline)) void write_through(volatile int* p) {
    p[2] = 1;
}

} // namespace

// A real fault nothing handles: Tome's filter, in front of the one that
// was there, reports it, then that one still gets it.
TEST(host_crash_report_unhandled) {
    g_unhandled_home = new Home;
    host::open_log(*g_unhandled_home);
    SetUnhandledExceptionFilter(after_tome);
    crash_handler::install(g_unhandled_home->paths(), GetCurrentThreadId());
    crash_handler::start(GameBuild{});
    write_through(g_null);
    CHECK(!"the fault didn't reach the filters");
}
