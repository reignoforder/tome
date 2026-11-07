// SPDX-License-Identifier: GPL-3.0-or-later
// Mods' messages from other players (remote input): each line of the input
// is one SyncStoredInteger action, "<sender byte><value 4 bytes><mission>\t<key>",
// run through a reassembler. Whatever completes must be a valid message, and
// encode's parts of it must reassemble to the same message.
#include "core/metadata.h"
#include "core/netmsg.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

using namespace tome::netmsg;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string_view input(reinterpret_cast<const char*>(data), size);
    Reassembler reassembler;
    size_t start = 0;
    while (start < input.size()) {
        size_t end = input.find('\n', start);
        if (end == std::string_view::npos) end = input.size();
        std::string_view line = input.substr(start, end - start);
        start = end + 1;
        if (line.size() < 5) continue;
        const int sender = static_cast<uint8_t>(line[0]) % 4;
        int32_t value;
        std::memcpy(&value, line.data() + 1, 4);
        line.remove_prefix(5);
        size_t tab = line.find('\t');
        std::string_view mission = line.substr(0, tab);
        std::string_view key = tab == std::string_view::npos ? std::string_view() : line.substr(tab + 1);
        Message message;
        if (reassembler.add(sender, mission, key, value, message) != Reassembler::Result::Complete) continue;
        if (message.data.size() > kMaxMessage || !tome::is_valid_mod_id(message.mod_id)) __builtin_trap();
        // What encode makes of it comes back whole.
        Reassembler again;
        Message back;
        Reassembler::Result r = Reassembler::Result::Bad;
        for (const Part& p : encode(message.mod_id, 7, message.data)) r = again.add(0, p.mission, p.key, p.value, back);
        if (r != Reassembler::Result::Complete || back.data != message.data || back.mod_id != message.mod_id) {
            __builtin_trap();
        }
    }
    return 0;
}
