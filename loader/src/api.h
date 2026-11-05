// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "game_build.h"

#include <tome/tome.h>

namespace tome {

// Fills the parts of the API table that depend on the detected game build.
void init_api(const GameBuild& build);

const tome_api* api_table();

} // namespace tome
