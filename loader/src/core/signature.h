// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// tome.sig: a mod author's Ed25519 signature over a mod's files, at the top of
// the mod folder (or package). It doesn't make a mod safe: it says who made
// it. When the user allows a signed mod, the key goes into state.json with
// the approval; a later version signed with the same key is allowed without
// asking (core/mod_state.h), one signed with another key, or not at all,
// asks. Signed by sdk/support/tome_sign.py.
//
// The file (JSON, at most kMaxFileSize bytes):
//   {"schema": 1, "key": "<base64, 32 bytes>", "signature": "<base64, 64 bytes>"}
// The signature is over message(): the mod's id, its code hash (the SHA-256
// of the manifest of every file but the top tome.mod.json and tome.sig,
// mod_state::manifest) and the SHA-256 of its tome.mod.json.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tome::signature {

constexpr std::string_view kFile = "tome.sig";
constexpr size_t kMaxFileSize = 4096;
constexpr int kSchema = 1;

using Key = std::array<uint8_t, 32>;
using Bytes = std::array<uint8_t, 64>;

struct Signature {
    Key key{};
    Bytes signature{};
};

// tome.sig's text. False, with `error` set, for anything but a well-formed
// file of a known schema.
bool parse(std::string_view text, Signature& out, std::string& error);

// What's signed: "tome-signature-v1\n<id>\n<code hash>\n<tome.mod.json SHA-256>\n"
// (hashes as 64 lowercase hex digits).
std::string message(std::string_view id, std::string_view code_hash, std::string_view metadata_sha256);

// Whether `key` is a point of small order: anyone can make signatures that
// verify under it, so parse refuses it. Never made by a real key generator.
bool small_order(const Key& key);

// Whether `sig` is a valid Ed25519 signature of `message` by its key.
bool verify(const Signature& sig, std::string_view message);

// Keys as state.json and the mod menu show them: standard base64 (44
// characters, padded). decode_key is false for anything but such a key.
std::string encode_key(const Key& key);
bool decode_key(std::string_view text, Key& key);

// Standard base64 (RFC 4648, padded, no whitespace). decode is false for
// any other character, wrong padding, or bits left over.
std::string base64_encode(const uint8_t* data, size_t size);
bool base64_decode(std::string_view text, std::vector<uint8_t>& out);

} // namespace tome::signature
