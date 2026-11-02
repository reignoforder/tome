// SPDX-License-Identifier: GPL-3.0-or-later
// Chat lines the player types (and /tome's arguments).
#include "core/chat.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string_view input(reinterpret_cast<const char*>(data), size);
    tome::chat::Command cmd;
    if (tome::chat::parse(input, cmd)) {
        tome::chat::valid_name(cmd.name);
        tome::chat::reserved(cmd.name);
        if (cmd.name == "tome") tome::chat::parse_tome(cmd.args);
    }
    tome::chat::parse_tome(input);
    return 0;
}
