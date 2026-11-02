// SPDX-License-Identifier: GPL-3.0-or-later
// JASS signatures (from tome.mod.json "natives", through the declaration
// parser), the native arguments Tome encodes for a Wasm mod by them, and the
// result check: the first line is a signature, the rest argument words and
// strings.
#include "core/wasm_layout.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

using namespace tome;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string_view text(reinterpret_cast<const char*>(data), size);
    const size_t line = text.find('\n');
    wasm::JassSignature sig;
    if (!wasm::parse_signature(text.substr(0, line), sig)) return 0;
    std::string_view rest = line == std::string_view::npos ? std::string_view() : text.substr(line + 1);
    std::vector<wasm::NativeArg> args(sig.params.size());
    for (size_t i = 0; i < args.size(); ++i) {
        if (rest.size() >= 4) {
            std::memcpy(&args[i].word, rest.data(), 4);
            rest.remove_prefix(4);
        }
        const size_t n = std::min<size_t>(rest.size(), args[i].word & 31);
        args[i].text = std::string(rest.substr(0, n));
        args[i].null = (args[i].word & 32) != 0;
        rest.remove_prefix(n);
    }
    const uint32_t base = args.empty() ? 0 : args[0].word & 0xFFFF0;
    const std::string block = wasm::encode_native_args(sig, args, base);
    // One word per parameter, then the strings: never shorter.
    if (block.size() < 4 * sig.params.size()) __builtin_trap();
    if (block.size() != wasm::encode_native_args(sig, args, 0).size()) __builtin_trap();
    for (const wasm::NativeArg& a : args) wasm::valid_native_result(sig, args, a.word);
    if (sig.result.kind == 'H' && !wasm::valid_native_result(sig, args, 0)) __builtin_trap();
    if (sig.result.kind == 'C' && wasm::valid_native_result(sig, args, 0)) __builtin_trap();
    return 0;
}
