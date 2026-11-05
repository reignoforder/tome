// SPDX-License-Identifier: GPL-3.0-or-later
// A map script (war3map.j, from a downloaded map) through the shadow
// scanner, and native declarations (from tome.mod.json) through parse_native.
#include "core/jass.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

using namespace tome::jass;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string_view script(reinterpret_cast<const char*>(data), size);
    std::vector<ShadowFinding> findings;
    // The first line stands in for common.j/Blizzard.j.
    size_t nl = script.find('\n');
    std::vector<std::string_view> prelude;
    if (nl != std::string_view::npos) prelude.push_back(script.substr(0, nl));
    scan_shadow_exploit(script, findings, prelude);

    NativeDecl decl;
    std::string error;
    if (parse_native(script, decl, error)) {
        decl.signature();
        decl.code_mask();
    }
    return 0;
}
