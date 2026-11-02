// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// JASS native declarations, as written in common.j or a mod's metadata:
//   native SetUnitX takes unit whichUnit, real newX returns nothing
// and the engine's signature strings for them: "(Hunit;R)V".

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tome::jass {

struct NativeDecl {
    std::string name;
    bool is_constant = false;
    std::vector<std::string> param_types;  // JASS type names, e.g. "unit", "real"
    std::string return_type;               // "nothing" for none

    // Engine signature: I integer, R real, B boolean, S string, C code,
    // V nothing, H<type>; for every handle type.
    std::string signature() const;

    // Bit i set if parameter i is `code` (parameters past the 32nd aren't counted).
    uint32_t code_mask() const;
};

bool parse_native(std::string_view text, NativeDecl& out, std::string& error);

bool is_valid_identifier(std::string_view name);

// A `local` declaration whose name matches a `global` variable of a different
// type: the local/global name-shadowing typecast used by memhack/ACE maps
// (docs/known-bugs.md). Line numbers are 1-based, from the local declaration.
struct ShadowFinding {
    std::string name;
    std::string global_type;
    std::string local_type;
    int line = 0;
};

// Scans a whole map script (war3map.j) for the shadow pattern above. Returns
// true if any were found. Line-oriented and deliberately conservative: it
// skips `//` comments and string literals and looks at the leading words of
// each declaration, so unusual formatting yields false negatives, never false
// positives on well-formed scripts.
//
// `prelude` holds the scripts compiled before it (common.j, Blizzard.j):
// their globals can be shadowed too (e.g. a `local integer bj_lastCreatedUnit`),
// but only the map script's own lines are reported.
bool scan_shadow_exploit(std::string_view script, std::vector<ShadowFinding>& findings,
                         const std::vector<std::string_view>& prelude = {});

} // namespace tome::jass
