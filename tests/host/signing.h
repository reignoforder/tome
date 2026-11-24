// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Signing fixture mods in tome_host_tests (monocypher; not in tome_game_tests).

#include "harness.h"

#include <cstdint>
#include <string>

namespace host {

// Signs the installed crash fixture as sdk/support/tome_sign.py would, with
// the key made from `seed_byte`; returns the public key (base64).
std::string sign_crash(Home& home, uint8_t seed_byte);

} // namespace host
