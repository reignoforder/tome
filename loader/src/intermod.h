// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The inter-mod part of the mod API (tome.h, API version 11): services other
// mods can look up and messages on topics. The tables live in core/intermod.

#include <tome/tome.h>

namespace tome::intermod_api {

void fill_api(tome_api& api);

} // namespace tome::intermod_api
