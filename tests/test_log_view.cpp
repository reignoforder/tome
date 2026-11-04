// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/log_line.h"
#include "core/log_view.h"
#include "test.h"

using tome::chat::LogFilter;
using tome::log_view::format;

namespace {

const char* const kLog =
    "[12:00:01.000] [  100] [INFO ] Tome 0.1.0 starting\r\n"
    "[12:00:02.500] [  100] [DEBUG] detail\r\n"
    "[12:00:03.000] [ 2048] [WARN ] hello: a | pipe\r\n"
    "[12:00:04.000] [ 2048] [ERROR] crashed\r\n"
    "  called from x.dll+0x10\r\n"
    "[12:00:05.000] [  100] [INFO ] Chat command: /tome logs\r\n";

} // namespace

TEST(log_view_last_lines) {
    auto lines = format(kLog, LogFilter::All, 10);
    CHECK_EQ(lines.size(), size_t(5));
    CHECK_EQ(lines[0], std::string("12:00:01 |cff80c0ffINFO|r Tome 0.1.0 starting"));
    CHECK_EQ(lines[1], std::string("12:00:02 |cff808080DEBUG|r detail"));
    CHECK_EQ(lines[2], std::string("12:00:03 |cffffa000WARN|r hello: a || pipe"));
    CHECK_EQ(lines[3], std::string("12:00:04 |cffff4040ERROR|r crashed   called from x.dll+0x10"));

    auto two = format(kLog, LogFilter::All, 2);
    CHECK_EQ(two.size(), size_t(2));
    CHECK(two[0].find("crashed") != std::string::npos);
    CHECK(two[1].find("/tome logs") != std::string::npos);
}

TEST(log_view_filters) {
    auto warn = format(kLog, LogFilter::Warn, 10);
    CHECK_EQ(warn.size(), size_t(2));
    CHECK(warn[0].find("pipe") != std::string::npos);
    CHECK(warn[1].find("crashed") != std::string::npos);

    auto error = format(kLog, LogFilter::Error, 10);
    CHECK_EQ(error.size(), size_t(1));
    CHECK(error[0].find("crashed") != std::string::npos);

    CHECK(format("", LogFilter::All, 10).empty());
    CHECK(format(kLog, LogFilter::All, 0).empty());
}

TEST(log_view_cut_and_long) {
    // A tail read from the middle of a line: that piece is skipped.
    auto cut = format("ed line\r\n[12:00:01.000] [    1] [INFO ] next\r\n", LogFilter::All, 10);
    CHECK_EQ(cut.size(), size_t(1));
    CHECK(cut[0].find("next") != std::string::npos);

    std::string log = "[12:00:01.000] [    1] [INFO ] " + std::string(300, 'a') + "\r\n";
    auto lines = format(log, LogFilter::All, 10);
    CHECK_EQ(lines.size(), size_t(1));
    CHECK_EQ(lines[0].size(), std::string("12:00:01 |cff80c0ffINFO|r ").size() + tome::log_view::kMaxMessage + 3);

    // Not cut inside a UTF-8 sequence.
    log = "[12:00:01.000] [    1] [INFO ] " + std::string(199, 'a') + "\xc3\xa9\r\n";
    lines = format(log, LogFilter::All, 10);
    CHECK_EQ(lines.size(), size_t(1));
    if (!lines.empty()) CHECK(lines[0].find("\xc3") == std::string::npos);

    // Invalid UTF-8 (a mod logs any bytes) reaches the game as U+FFFD.
    lines = format("[12:00:01.000] [    1] [WARN ] [m] \xff ok\r\n", LogFilter::All, 10);
    CHECK_EQ(lines.size(), size_t(1));
    if (!lines.empty()) CHECK_EQ(lines[0], std::string("12:00:01 |cffffa000WARN|r [m] \xef\xbf\xbd ok"));
}

// What the logger writes (log_line.h, as log.cpp calls it) reads back.
TEST(log_view_reads_logger_lines) {
    using tome::log::Level;
    std::string log;
    char line[2200];
    int len = tome::log::format_line(line, sizeof(line), 9, 5, 7, 42, 123456, Level::Error, "boom");
    CHECK(len > 0);
    if (len > 0) log.append(line, static_cast<size_t>(len));
    len = tome::log::format_line(line, sizeof(line), 23, 59, 59, 999, 7, Level::Debug, "late");
    if (len > 0) log.append(line, static_cast<size_t>(len));
    std::string long_message(3000, 'x');  // cut short, still one line
    len = tome::log::format_line(line, sizeof(line), 0, 0, 0, 0, 1, Level::Warn, long_message.c_str());
    CHECK_EQ(len, int(sizeof(line)) - 1);
    if (len > 0) log.append(line, static_cast<size_t>(len));

    auto lines = format(log, LogFilter::All, 10);
    CHECK_EQ(lines.size(), size_t(3));
    if (lines.size() == 3) {
        CHECK_EQ(lines[0], std::string("09:05:07 |cffff4040ERROR|r boom"));
        CHECK_EQ(lines[1], std::string("23:59:59 |cff808080DEBUG|r late"));
        CHECK(lines[2].rfind("00:00:00 |cffffa000WARN|r xxx", 0) == 0);
    }
}

TEST(log_view_continuations_joined) {
    // Each continuation line adds one space, its own spaces kept; CRs dropped.
    auto lines = format("[12:00:01.000] [    1] [ERROR] a\r\n b\r\n\r\nc\n", LogFilter::All, 10);
    CHECK_EQ(lines.size(), size_t(1));
    if (!lines.empty()) CHECK_EQ(lines[0], std::string("12:00:01 |cffff4040ERROR|r a  b  c"));
}
