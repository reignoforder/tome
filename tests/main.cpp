#include "test.h"

int main() {
    for (const test::Case& c : test::registry()) {
        int before = test::failures();
        c.fn();
        std::printf("%s %s\n", test::failures() == before ? "ok  " : "FAIL", c.name);
    }
    std::printf("\n%zu tests, %d failed checks\n", test::registry().size(), test::failures());
    return test::failures() == 0 ? 0 : 1;
}
