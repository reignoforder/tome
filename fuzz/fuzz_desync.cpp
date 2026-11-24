// SPDX-License-Identifier: GPL-3.0-or-later
// Other players' state checksums (remote input): each line of the input is
// one SyncStoredInteger action of Tome's checksum cache, "<sender
// byte><mission>\t<key>", parsed and fed to a checker, with this computer's
// own hashes for some rounds in between. What parses must encode back to the
// same action, and what waits stays bounded.
#include "core/desync.h"
#include "core/metadata.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace tome::desync;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string_view input(reinterpret_cast<const char*>(data), size);
    Checker checker;
    std::vector<Comparison> found;
    uint32_t local_round = 0;
    size_t start = 0;
    while (start < input.size()) {
        size_t end = input.find('\n', start);
        if (end == std::string_view::npos) end = input.size();
        std::string_view line = input.substr(start, end - start);
        start = end + 1;
        if (line.empty()) continue;
        const uint8_t sender = static_cast<uint8_t>(line[0]);
        line.remove_prefix(1);
        // A sender byte of 0xFF: this computer finishes a round instead.
        if (sender == 0xFF) {
            checker.local(++local_round, "local-mod", local_round * 7, found);
            continue;
        }
        size_t tab = line.find('\t');
        std::string_view mission = line.substr(0, tab);
        std::string_view key = tab == std::string_view::npos ? std::string_view() : line.substr(tab + 1);
        uint32_t round = 0;
        std::string mod;
        uint64_t hash = 0;
        if (!parse(mission, key, round, mod, hash)) continue;
        if (round == 0 || !tome::is_valid_mod_id(mod)) __builtin_trap();
        const Part back = encode(round, mod, hash);
        if (back.mission != mission || back.key != key) __builtin_trap();
        checker.remote(sender % 24, round, mod, hash, found);
        if (checker.waiting() > kMaxWaiting || checker.kept() > kWindow + 1) __builtin_trap();
    }
    for (const Comparison& c : found) {
        if (c.mod_id.empty() || c.round == 0) __builtin_trap();
    }
    return 0;
}
