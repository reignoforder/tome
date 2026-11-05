// SPDX-License-Identifier: GPL-3.0-or-later
#include "sha256.h"

#include <windows.h>
#include <bcrypt.h>

#include <vector>

namespace tome {
namespace {

// Feeds chunks from `next` (which returns false when there are no more, or
// sets `ok` to false on a read error) into one hash.
template <typename Next>
std::string sha256_chunks(Next next) {
    std::string result;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        bool ok = true;
        const UCHAR* data = nullptr;
        ULONG size = 0;
        while (next(data, size, ok)) {
            if (size && BCryptHashData(hash, const_cast<UCHAR*>(data), size, 0) != 0) {
                ok = false;
                break;
            }
        }

        UCHAR digest[32];
        if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
            static const char hex[] = "0123456789abcdef";
            result.reserve(64);
            for (UCHAR byte : digest) {
                result += hex[byte >> 4];
                result += hex[byte & 0xF];
            }
        }
    }

    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return result;
}

} // namespace

std::string sha256_file(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::vector<UCHAR> buffer(1 << 20);
    std::string result = sha256_chunks([&](const UCHAR*& data, ULONG& size, bool& ok) {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            ok = false;
            return false;
        }
        data = buffer.data();
        size = read;
        return read > 0;
    });
    CloseHandle(file);
    return result;
}

std::string sha256_bytes(std::string_view bytes) {
    bool fed = false;
    return sha256_chunks([&](const UCHAR*& data, ULONG& size, bool&) {
        if (fed) return false;
        fed = true;
        data = reinterpret_cast<const UCHAR*>(bytes.data());
        size = static_cast<ULONG>(bytes.size());
        return true;
    });
}

} // namespace tome
