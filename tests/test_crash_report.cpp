// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/crash_report.h"
#include "test.h"

using namespace tome;
using namespace tome::crash_report;

TEST(crash_report_starts_file) {
    CHECK_EQ(parse_starts("").count, 0);
    CHECK_EQ(parse_starts("2\n").count, 2);
    CHECK_EQ(parse_starts("2\r\n").count, 2);
    CHECK_EQ(parse_starts(" 3 \n\n").count, 3);
    CHECK_EQ(parse_starts("x\n").count, 0);
    CHECK_EQ(parse_starts("-1\n").count, 0);
    CHECK_EQ(parse_starts("99999999999\n").count, 0);  // not a count Tome wrote

    Starts s = parse_starts("4\ncrash-20261030-142501\n");
    CHECK_EQ(s.count, 4);
    CHECK_EQ(s.last_report, std::string("crash-20261030-142501"));
    // Only a report's stem: never a path out of tome/crashes.
    CHECK_EQ(parse_starts("4\n..\\..\\x\n").last_report, std::string());
    CHECK_EQ(parse_starts("4\ncrash-2026103a-142501\n").last_report, std::string());

    CHECK_EQ(format_starts(s), std::string("4\ncrash-20261030-142501\n"));
    CHECK_EQ(format_starts({}), std::string("0\n\n"));
    Starts round = parse_starts(format_starts(s));
    CHECK_EQ(round.count, s.count);
    CHECK_EQ(round.last_report, s.last_report);
}

TEST(crash_report_crash_loop) {
    CHECK(!is_crash_loop(0));
    CHECK(!is_crash_loop(kStartsBeforeSafeMode - 1));
    CHECK(is_crash_loop(kStartsBeforeSafeMode));
    CHECK(is_crash_loop(kStartsBeforeSafeMode + 5));
    CHECK_EQ(safe_mode_reason(3), std::string("the last 3 starts didn't reach the main menu"));

    popup::Spec spec = crash_loop_prompt(3, "");
    CHECK(popup::validate(spec).empty());
    CHECK_EQ(spec.options.size(), size_t(2));
    CHECK_EQ(spec.options[kKeepSafeMode], std::string("Keep Safe Mode"));
    CHECK_EQ(spec.cancel_option, kThisStart);
    CHECK_EQ(spec.default_option, kThisStart);
    CHECK(spec.text.find("last 3 times") != std::string::npos);
    CHECK(spec.text.find("Last crash") == std::string::npos);
    spec = crash_loop_prompt(4, "access violation reading 0x00000004 at hello.dll+0x10 (mod hello)");
    CHECK(spec.text.find("Last crash: access violation reading 0x00000004 at hello.dll+0x10 (mod hello)") !=
          std::string::npos);
    CHECK(popup::estimate_lines(spec.text, 60) <= popup::kMaxTextLines);
}

TEST(crash_report_describe) {
    const uint32_t read[2] = {0, 4}, write[2] = {1, 0x1234}, exec[2] = {8, 0xdead0000};
    CHECK_EQ(describe_exception(0xC0000005, read, 2), std::string("access violation reading 0x00000004"));
    CHECK_EQ(describe_exception(0xC0000005, write, 2), std::string("access violation writing 0x00001234"));
    CHECK_EQ(describe_exception(0xC0000005, exec, 2), std::string("access violation executing 0xdead0000"));
    CHECK_EQ(describe_exception(0xC0000005, nullptr, 0), std::string("access violation"));
    CHECK_EQ(describe_exception(0xC00000FD, nullptr, 0), std::string("stack overflow"));
    CHECK_EQ(describe_exception(0xE0000001, nullptr, 0), std::string("exception 0xe0000001"));
    CHECK(exception_name(0x12345678) == nullptr);

    Location at;
    at.address = 0x6f01a2b0;
    CHECK_EQ(where(at), std::string("0x6f01a2b0"));
    at.module = "game.dll";
    at.offset = 0x1a2b0;
    CHECK_EQ(where(at), std::string("game.dll+0x1a2b0"));
    at.module = "hello.3.dll";
    at.offset = 0x10;
    at.mod = "hello";
    CHECK_EQ(where(at), std::string("hello.3.dll+0x10 (mod hello)"));
}

TEST(crash_report_format) {
    Report r;
    r.time = "2026-10-30 14:25:01";
    r.tome_version = "0.1.0";
    r.game = "1.27b (game.dll 1.27.1.7085)";
    r.code = 0xC0000005;
    r.info = {0, 4};
    r.at = {0x6f01a2b0, "game.dll", 0x1a2b0, ""};
    r.thread = 42;
    r.main_thread = true;
    r.registers[8] = 0x6f01a2b0;
    r.callers = {{0x10001234, "hello.3.dll", 0x1234, "hello"}};
    r.mods_known = true;
    r.mods = {{"hello", "1.2.0", "active"}};
    r.dump = "crash-20261030-142501.dmp";
    r.log_tail = "[14:25:00.000] [   42] [INFO ] Bootstrap complete\n";

    std::string text = format(r);
    CHECK_EQ(summary(r), std::string("access violation reading 0x00000004 at game.dll+0x1a2b0"));
    CHECK_EQ(summary_of(text), summary(r));
    CHECK(text.find("Thread: 42 (the game's main thread)") != std::string::npos);
    CHECK(text.find("eip=6f01a2b0") != std::string::npos);
    CHECK(text.find("  hello.3.dll+0x1234 (mod hello)\n") != std::string::npos);
    CHECK(text.find("  hello                     1.2.0       active\n") != std::string::npos);
    CHECK(text.find("Minidump: crash-20261030-142501.dmp") != std::string::npos);
    CHECK(text.find("Bootstrap complete\n") != std::string::npos);

    r.dump.clear();
    r.dump_error = "error 5";
    r.mods_known = false;
    r.callers.clear();
    text = format(r);
    CHECK(text.find("Minidump: not written (error 5)") != std::string::npos);
    CHECK(text.find("Mods:\n  (unknown)") != std::string::npos);
    CHECK(text.find("(nothing found on the stack)") != std::string::npos);

    CHECK_EQ(summary_of("no summary\n"), std::string());
    CHECK_EQ(summary_of("Summary: x\r\nTime: y"), std::string("x"));
}

TEST(crash_report_tail) {
    CHECK_EQ(tail("", 3), std::string());
    CHECK_EQ(tail("a\nb\nc\n", 0), std::string());
    CHECK_EQ(tail("a\nb\nc\n", 2), std::string("b\nc\n"));
    CHECK_EQ(tail("a\nb\nc", 2), std::string("b\nc"));
    CHECK_EQ(tail("a\nb\nc\n", 3), std::string("a\nb\nc\n"));
    CHECK_EQ(tail("a\nb\nc\n", 10), std::string("a\nb\nc\n"));
}

TEST(crash_report_files) {
    CHECK_EQ(file_stem(2026, 10, 30, 14, 5, 1), std::string("crash-20261030-140501"));
    std::vector<std::string> names = {
        "crash-20261030-140501.txt", "crash-20261030-140501.dmp", "crash-20261029-090000.txt",
        "crash-20261031-000000.txt", "crash-20261031-000000.dmp", "notes.txt", "crash-x.txt",
    };
    std::vector<std::string> expected = {"crash-20261029-090000.txt"};
    CHECK(to_remove(names, 2) == expected);
    expected = {"crash-20261029-090000.txt", "crash-20261030-140501.dmp", "crash-20261030-140501.txt"};
    CHECK(to_remove(names, 1) == expected);
    CHECK(to_remove(names, 0).size() == size_t(5));  // never notes.txt or crash-x.txt
    CHECK(to_remove(names, 3).empty());
}
