// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Tiny test harness: no exceptions, no dependencies.

#include <cstdio>
#include <string>
#include <vector>

namespace test {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failures() {
    static int count = 0;
    return count;
}

struct Register {
    Register(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void fail(const char* file, int line, const std::string& message) {
    ++failures();
    std::printf("  FAIL %s:%d: %s\n", file, line, message.c_str());
}

} // namespace test

#define TEST(name)                                              \
    static void test_##name();                                  \
    static test::Register register_##name(#name, test_##name);  \
    static void test_##name()

#define CHECK(cond)                                                  \
    do {                                                             \
        if (!(cond)) test::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

#define CHECK_EQ(a, b)                                                                  \
    do {                                                                                \
        auto&& check_a_ = (a);                                                          \
        auto&& check_b_ = (b);                                                          \
        if (!(check_a_ == check_b_)) test::fail(__FILE__, __LINE__, "CHECK_EQ(" #a ", " #b ")"); \
    } while (0)
