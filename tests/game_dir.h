// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The game folder the tests read (only read: input/README.md): TOME_GAME_DIR
// from the environment (`make test <version>` sets it to input/<version>),
// else the build's default, TOME_TEST_GAME_DIR (input/1.27b).

#include <cstdlib>
#include <string>

namespace test {

inline std::string game_dir() {
    const char* dir = std::getenv("TOME_GAME_DIR");
    return dir && *dir ? dir : TOME_TEST_GAME_DIR;
}

} // namespace test
