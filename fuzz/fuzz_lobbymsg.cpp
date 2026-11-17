// SPDX-License-Identifier: GPL-3.0-or-later
// Mods' lobby messages from other players (remote input): each line of the
// input is one lobby chat line, its first byte the sender, run through a
// reassembler. Whatever completes must be a valid message, and encode's
// lines of it must reassemble to the same message and fit the chat.
#include "core/lobbymsg.h"
#include "core/metadata.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

using namespace tome::lobbymsg;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string_view input(reinterpret_cast<const char*>(data), size);
    Reassembler reassembler;
    size_t start = 0;
    while (start < input.size()) {
        size_t end = input.find('\n', start);
        if (end == std::string_view::npos) end = input.size();
        std::string_view line = input.substr(start, end - start);
        start = end + 1;
        if (line.empty()) continue;
        const int sender = static_cast<uint8_t>(line[0]) % 4;
        line.remove_prefix(1);
        Message message;
        if (reassembler.add(sender, line, message) != Reassembler::Result::Complete) continue;
        if (message.data.size() > kMaxMessage || !tome::is_valid_mod_id(message.mod_id)) __builtin_trap();
        Reassembler again;
        Message back;
        Reassembler::Result r = Reassembler::Result::Bad;
        for (const std::string& l : encode(message.mod_id, 7, message.data)) {
            if (l.size() > kMaxLine) __builtin_trap();
            r = again.add(0, l, back);
        }
        if (r != Reassembler::Result::Complete || back.data != message.data || back.mod_id != message.mod_id) {
            __builtin_trap();
        }
    }
    return 0;
}
