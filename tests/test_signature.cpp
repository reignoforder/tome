// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/signature.h"
#include "test.h"

#include <cstring>

using namespace tome;
using namespace tome::signature;

namespace {

bool contains(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

std::vector<uint8_t> hex(std::string_view text) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < text.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(std::stoi(std::string(text.substr(i, 2)), nullptr, 16)));
    }
    return out;
}

Signature make(std::string_view key_hex, std::string_view sig_hex) {
    Signature s;
    std::vector<uint8_t> key = hex(key_hex), sig = hex(sig_hex);
    std::memcpy(s.key.data(), key.data(), s.key.size());
    std::memcpy(s.signature.data(), sig.data(), s.signature.size());
    return s;
}

std::string sig_file(std::string_view key, std::string_view sig, int schema = 1) {
    return "{\"schema\": " + std::to_string(schema) + ", \"key\": \"" + std::string(key) + "\", \"signature\": \"" +
           std::string(sig) + "\"}";
}

// sdk/support/tome_sign.py's signature of message("hello", 64 'a', 64 'b')
// with the secret key 00 01 02 ... 1f: both implementations must agree.
constexpr const char* kPythonKey = "A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg=";
constexpr const char* kPythonSig =
    "1ZY1zVovhKbr3QO1O0OAjGOFjXgRHs1UU6UJibQ0CxwRJ5LYn3O3BhmfDEcO6FR/jk64fCChxzr0m7gGlhTuBA==";

} // namespace

TEST(signature_rfc8032_vectors) {
    // RFC 8032, section 7.1, tests 1 and 2.
    Signature one = make("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
                         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
    CHECK(verify(one, ""));
    CHECK(!verify(one, "x"));
    Signature two = make("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
                         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00");
    CHECK(verify(two, "\x72"));
    CHECK(!verify(two, "\x73"));
    two.signature[10] ^= 1;
    CHECK(!verify(two, "\x72"));
}

TEST(signature_refuses_small_order_keys) {
    // An all-zero key and signature "verify" (Ed25519 allows it): such keys
    // can't stand for an author.
    CHECK(verify(Signature{}, ""));
    Key key{};
    CHECK(small_order(key));
    key[31] = 0x80;  // the same y with the sign bit
    CHECK(small_order(key));
    key = {};
    key[0] = 0x01;
    CHECK(small_order(key));
    std::vector<uint8_t> p_plus_1 = hex("eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f");
    std::memcpy(key.data(), p_plus_1.data(), key.size());
    CHECK(small_order(key));
    std::vector<uint8_t> order_8 = hex("c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa");
    std::memcpy(key.data(), order_8.data(), key.size());
    CHECK(small_order(key));
    CHECK(decode_key(kPythonKey, key));
    CHECK(!small_order(key));

    Signature s;
    std::string error;
    CHECK(!parse(sig_file("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=", kPythonSig), s, error));
    CHECK(contains(error, "weak key"));
}

TEST(signature_matches_the_python_signer) {
    Signature s;
    std::string error;
    CHECK(parse(sig_file(kPythonKey, kPythonSig), s, error));
    const std::string a(64, 'a'), b(64, 'b');
    CHECK(verify(s, message("hello", a, b)));
    CHECK(!verify(s, message("hellp", a, b)));
    CHECK(!verify(s, message("hello", b, b)));
    CHECK(!verify(s, message("hello", a, a)));
    CHECK_EQ(encode_key(s.key), std::string(kPythonKey));
}

TEST(signature_message) {
    CHECK_EQ(message("hello", "11", "22"), std::string("tome-signature-v1\nhello\n11\n22\n"));
}

TEST(signature_parse_refuses) {
    Signature s;
    std::string error;
    CHECK(!parse("", s, error));
    CHECK(!parse("[]", s, error));
    CHECK(!parse(sig_file(kPythonKey, kPythonSig, 2), s, error));
    CHECK(contains(error, "schema"));
    CHECK(!parse("{\"key\": \"" + std::string(kPythonKey) + "\", \"signature\": \"" + kPythonSig + "\"}", s, error));
    CHECK(!parse(sig_file("AAAA", kPythonSig), s, error));
    CHECK(contains(error, "key"));
    CHECK(!parse(sig_file(kPythonKey, kPythonKey), s, error));  // 32 bytes, not 64
    CHECK(contains(error, "signature"));
    CHECK(!parse(sig_file(kPythonKey, std::string(kPythonSig) + " "), s, error));
    std::string big = sig_file(kPythonKey, kPythonSig) + std::string(kMaxFileSize, ' ');
    CHECK(!parse(big, s, error));
    CHECK(contains(error, "larger"));
}

TEST(signature_base64) {
    auto round = [](std::string_view bytes) {
        return base64_encode(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    };
    CHECK_EQ(round(""), std::string(""));
    CHECK_EQ(round("f"), std::string("Zg=="));
    CHECK_EQ(round("fo"), std::string("Zm8="));
    CHECK_EQ(round("foo"), std::string("Zm9v"));
    CHECK_EQ(round("foobar"), std::string("Zm9vYmFy"));
    std::vector<uint8_t> out;
    CHECK(base64_decode("Zm9vYg==", out));
    CHECK_EQ(std::string(out.begin(), out.end()), std::string("foob"));
    CHECK(base64_decode("", out) && out.empty());
    CHECK(!base64_decode("Zm9", out));       // not a multiple of 4
    CHECK(!base64_decode("Zm=v", out));      // a digit after '='
    CHECK(!base64_decode("Z===", out));      // too much padding
    CHECK(!base64_decode("Zg==Zg==", out));  // padding before the end
    CHECK(!base64_decode("Zh==", out));      // bits left over
    CHECK(!base64_decode("Zm9-", out));      // URL-safe alphabet
    CHECK(!base64_decode("Zm9v\n", out));
    Key key;
    CHECK(decode_key(kPythonKey, key));
    CHECK(!decode_key("Zm9v", key));
}
