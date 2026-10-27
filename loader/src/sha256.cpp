#include "sha256.h"

#include <windows.h>
#include <bcrypt.h>

#include <vector>

namespace tome {

std::string sha256_file(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }

    std::string result;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<UCHAR> buffer(1 << 20);
        DWORD read = 0;
        bool ok = true;
        while (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read > 0) {
            if (BCryptHashData(hash, buffer.data(), read, 0) != 0) {
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
    CloseHandle(file);
    return result;
}

} // namespace tome
