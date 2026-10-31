#include "signature.h"

#include "json.h"

#include "monocypher-ed25519.h"

#include <cstring>

namespace tome::signature {
namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int digit(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// The encodings (sign bit cleared) of points of small order: y = 0, 1,
// p - 1, p and p + 1 (0 and 1 again) and the two order-8 points' y. Any
// message "verifies" under such a key for some signature anyone can make, so
// it can't stand for an author. Honest keys are never among them.
constexpr uint8_t kSmallOrder[][32] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x26, 0xe8, 0x95, 0x8f, 0xc2, 0xb2, 0x27, 0xb0, 0x45, 0xc3, 0xf4, 0x89, 0xf2, 0xef, 0x98, 0xf0,
     0xd5, 0xdf, 0xac, 0x05, 0xd3, 0xc6, 0x33, 0x39, 0xb1, 0x38, 0x02, 0x88, 0x6d, 0x53, 0xfc, 0x05},
    {0xc7, 0x17, 0x6a, 0x70, 0x3d, 0x4d, 0xd8, 0x4f, 0xba, 0x3c, 0x0b, 0x76, 0x0d, 0x10, 0x67, 0x0f,
     0x2a, 0x20, 0x53, 0xfa, 0x2c, 0x39, 0xcc, 0xc6, 0x4e, 0xc7, 0xfd, 0x77, 0x92, 0xac, 0x03, 0x7a},
    {0xec, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
    {0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
    {0xee, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
};

template <size_t N>
bool decode_exact(std::string_view text, std::array<uint8_t, N>& out) {
    std::vector<uint8_t> bytes;
    if (!base64_decode(text, bytes) || bytes.size() != N) return false;
    std::memcpy(out.data(), bytes.data(), N);
    return true;
}

} // namespace

std::string base64_encode(const uint8_t* data, size_t size) {
    std::string text;
    text.reserve((size + 2) / 3 * 4);
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t n = (uint32_t{data[i]} << 16) | (i + 1 < size ? uint32_t{data[i + 1]} << 8 : 0) |
                           (i + 2 < size ? uint32_t{data[i + 2]} : 0);
        text += kAlphabet[(n >> 18) & 63];
        text += kAlphabet[(n >> 12) & 63];
        text += i + 1 < size ? kAlphabet[(n >> 6) & 63] : '=';
        text += i + 2 < size ? kAlphabet[n & 63] : '=';
    }
    return text;
}

bool base64_decode(std::string_view text, std::vector<uint8_t>& out) {
    out.clear();
    if (text.size() % 4 != 0) return false;
    for (size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        int d[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = text[i + k];
            if (c == '=' && last && k >= 2) {
                d[k] = 0;
                ++pad;
                continue;
            }
            if (pad) return false;  // a digit after '='
            d[k] = digit(c);
            if (d[k] < 0) return false;
        }
        const uint32_t n = (uint32_t(d[0]) << 18) | (uint32_t(d[1]) << 12) | (uint32_t(d[2]) << 6) | uint32_t(d[3]);
        // Bits the padding drops must be zero, so each value has one encoding.
        if ((pad == 1 && (n & 0xFF)) || (pad == 2 && (n & 0xFFFF))) return false;
        out.push_back(static_cast<uint8_t>(n >> 16));
        if (pad < 2) out.push_back(static_cast<uint8_t>(n >> 8));
        if (pad < 1) out.push_back(static_cast<uint8_t>(n));
    }
    return true;
}

bool small_order(const Key& key) {
    Key y = key;
    y[31] &= 0x7f;
    for (const auto& bad : kSmallOrder) {
        if (std::memcmp(y.data(), bad, y.size()) == 0) return true;
    }
    return false;
}

std::string encode_key(const Key& key) {
    return base64_encode(key.data(), key.size());
}

bool decode_key(std::string_view text, Key& key) {
    return decode_exact(text, key);
}

bool parse(std::string_view text, Signature& out, std::string& error) {
    if (text.size() > kMaxFileSize) {
        error = "larger than " + std::to_string(kMaxFileSize) + " bytes";
        return false;
    }
    json::Value root;
    json::ParseResult parsed = json::parse(text, root);
    if (!parsed.ok) {
        error = parsed.error;
        return false;
    }
    if (!root.is_object()) {
        error = "not a JSON object";
        return false;
    }
    const json::Value* schema = root.find("schema");
    if (!schema || !schema->is_number() || schema->as_number() != kSchema) {
        error = "\"schema\" isn't " + std::to_string(kSchema);
        return false;
    }
    const json::Value* key = root.find("key");
    if (!key || !key->is_string() || !decode_key(key->as_string(), out.key)) {
        error = "\"key\" isn't a base64 Ed25519 public key";
        return false;
    }
    if (small_order(out.key)) {
        error = "\"key\" is a weak key (a point of small order) that can't stand for an author";
        return false;
    }
    const json::Value* sig = root.find("signature");
    if (!sig || !sig->is_string() || !decode_exact(sig->as_string(), out.signature)) {
        error = "\"signature\" isn't a base64 Ed25519 signature";
        return false;
    }
    return true;
}

std::string message(std::string_view id, std::string_view code_hash, std::string_view metadata_sha256) {
    std::string text = "tome-signature-v1\n";
    text += id;
    text += '\n';
    text += code_hash;
    text += '\n';
    text += metadata_sha256;
    text += '\n';
    return text;
}

bool verify(const Signature& sig, std::string_view message) {
    return crypto_ed25519_check(sig.signature.data(), sig.key.data(), reinterpret_cast<const uint8_t*>(message.data()),
                                message.size()) == 0;
}

} // namespace tome::signature
