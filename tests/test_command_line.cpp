#include "core/command_line.h"
#include "test.h"

using namespace tome;

TEST(command_line_split) {
    auto args = command_line::split(L"  \"C:\\Program Files\\war3.exe\" -window\t-loadfile \"Maps\\My Map.w3m\" ");
    CHECK_EQ(args.size(), size_t(4));
    CHECK(args[0] == L"C:\\Program Files\\war3.exe");
    CHECK(args[1] == L"-window");
    CHECK(args[2] == L"-loadfile");
    CHECK(args[3] == L"Maps\\My Map.w3m");

    CHECK(command_line::split(L"").empty());
    CHECK(command_line::split(L" \t ").empty());
    args = command_line::split(L"a\\\\\"b c\" d\\\"e f\\g");
    CHECK_EQ(args.size(), size_t(3));
    CHECK(args[0] == L"a\\b c");  // 2 backslashes + quote: one backslash, quote opens
    CHECK(args[1] == L"d\"e");    // 1 backslash + quote: a literal quote
    CHECK(args[2] == L"f\\g");    // backslashes not before a quote stay
    args = command_line::split(L"\"say \"\"hi\"\"\"");
    CHECK_EQ(args.size(), size_t(1));
    CHECK(args[0] == L"say \"hi\"");
}

TEST(command_line_has_flag) {
    CHECK(command_line::has_flag(L"war3.exe -window -tomesafe", L"-tomesafe"));
    CHECK(command_line::has_flag(L"war3.exe -TomeSafe -opengl", L"-tomesafe"));
    CHECK(command_line::has_flag(L"\"C:\\Games\\War3\\war3.exe\" \"-tomesafe\"", L"-tomesafe"));
    CHECK(!command_line::has_flag(L"war3.exe -window", L"-tomesafe"));
    CHECK(!command_line::has_flag(L"war3.exe -tomesafer", L"-tomesafe"));
    CHECK(!command_line::has_flag(L"war3.exe -loadfile -tomesafe.w3m", L"-tomesafe"));
    // The program's own path doesn't count.
    CHECK(!command_line::has_flag(L"-tomesafe -window", L"-tomesafe"));
    CHECK(!command_line::has_flag(L"", L"-tomesafe"));
}
