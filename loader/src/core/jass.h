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

} // namespace tome::jass
