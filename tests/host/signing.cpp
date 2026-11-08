// SPDX-License-Identifier: GPL-3.0-or-later
#include "signing.h"

#include "core/signature.h"
#include "monocypher-ed25519.h"
#include "sha256.h"

#include <cstring>

namespace host {

std::string sign_crash(Home& home, uint8_t seed_byte) {
    uint8_t seed[32], secret[64];
    tome::signature::Key key;
    std::memset(seed, seed_byte, sizeof(seed));
    crypto_ed25519_key_pair(secret, key.data(), seed);
    const std::wstring dir = home.paths().mods_dir + L"\\crash";
    const std::string code = crash_hash(home);
    const std::string text = tome::signature::message("crash", code, tome::sha256_file(dir + L"\\tome.mod.json"));
    tome::signature::Signature sig;
    sig.key = key;
    crypto_ed25519_sign(sig.signature.data(), secret, reinterpret_cast<const uint8_t*>(text.data()), text.size());
    const std::string public_key = tome::signature::encode_key(key);
    home.write(L"tome\\mods\\crash\\tome.sig",
               "{\"schema\": 1, \"key\": \"" + public_key + "\", \"signature\": \"" +
                   tome::signature::base64_encode(sig.signature.data(), sig.signature.size()) + "\"}");
    return public_key;
}

} // namespace host
