// SPDX-License-Identifier: GPL-3.0-or-later
// Lobby chat lines and game cache keys from other players (remote input):
// decode them, reassemble sets, and run them through a lobby on both sides.
#include "core/handshake.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

using namespace tome::handshake;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string_view input(reinterpret_cast<const char*>(data), size);

    ModSet local = {{"hello", "1.0.0", "1a2b3c4d"}, {"other", "2.0.0", "00ff00ff"}};
    std::vector<LobbyPlayer> others = {{1, "a"}, {2, "b"}};
    Lobby host, client, known;  // known: a client that knows the host (player 1)
    known.set_host(1);
    host.update(true, local, others, 0);
    client.update(false, local, others, 0);
    known.update(false, local, others, 0);

    // Each line of the input is one chat line; the first byte of a line picks the sender.
    Assembler assembler;
    GameRecord record;
    size_t start = 0;
    double now = 1;
    while (start <= input.size()) {
        size_t end = input.find('\n', start);
        if (end == std::string_view::npos) end = input.size();
        std::string_view line = input.substr(start, end - start);
        start = end + 1;
        int from = line.empty() ? 1 : 1 + static_cast<uint8_t>(line[0]) % 3;

        if (is_tome_line(line)) {
            Line decoded;
            if (decode(line, decoded)) {
                ModSet set;
                if (assembler.add(decoded, set)) describe(set);
            }
            host.receive(from, line);
            client.receive(from, line);
            known.receive(from, line);
        }
        SyncMod mod;
        if (decode_key(line, mod)) {
            record.add(from, mod, static_cast<int>(line.size() % 4));
            encode_key(mod);
        }
        host.update(true, local, others, now);
        client.update(false, local, others, now += 0.5);
        known.update(false, local, others, now);
    }
    std::vector<std::string> problems;
    host.can_start(problems);
    host.take_outgoing();
    client.take_outgoing();
    host.take_notes();
    client.take_notes();
    known.take_outgoing();
    known.take_notes();
    for (const auto& [slot, set] : record.sets()) explain(compare(local, set), "you");
    return 0;
}
