// SPDX-License-Identifier: GPL-3.0-or-later
// Which functions a Wasm mod may hook: a mapping's signature (a mod's own
// mapping file supplies them) through core/wasm_hooks.
#include "core/wasm_hooks.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

using namespace tome;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    wasm::HookPoint point;
    std::string why;
    if (wasm::hook_point("CUnit::SetHeroStr", std::string_view(reinterpret_cast<const char*>(data), size), point, why)) {
        if (point.args.size() > wasm::kMaxHookArgs || wasm::hook_block_size(point) > 1024) __builtin_trap();
    } else if (why.empty()) {
        __builtin_trap();
    }
    return 0;
}
