// SPDX-License-Identifier: GPL-3.0-or-later
// tome.sig (from mods, read before the mod is allowed): parse, verify, and
// a base64 round trip of what it decoded.
#include "core/signature.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace tome;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string_view text(reinterpret_cast<const char*>(data), size);

    std::vector<uint8_t> bytes;
    if (signature::base64_decode(text, bytes)) {
        // A decoded value has one encoding: the one it came from.
        if (signature::base64_encode(bytes.data(), bytes.size()) != text) __builtin_trap();
    }

    signature::Signature sig;
    std::string error;
    if (!signature::parse(text, sig, error)) return 0;
    signature::Key key;
    if (!signature::decode_key(signature::encode_key(sig.key), key) || key != sig.key) __builtin_trap();
    signature::verify(sig, signature::message("fuzz", std::string(64, 'a'), std::string(64, 'b')));
    signature::verify(sig, text);
    return 0;
}
