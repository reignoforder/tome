// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Checks a mapping file against the game.dll it claims to describe: every
// function's "bytes", every call site's `E8 rel32` and target, and which
// function every import slot imports. Tome runs this on the loaded module
// before patching anything; a unit test runs it on the file in the local
// install.

#include "mappings.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace tome::mappings {

// `image` is game.dll as the Windows loader lays it out (sections at their
// RVAs), `size` bytes of it readable. Returns one message per mismatch,
// each naming the symbol; empty means the build matches. `originals`, if
// given, are bytes as they were before something patched them, by RVA: they
// count instead of what the image has there now.
std::vector<std::string> verify_code(const Build& build, const uint8_t* image, size_t size,
                                     const std::map<uint32_t, uint8_t>* originals = nullptr);

} // namespace tome::mappings
