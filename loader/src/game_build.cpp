// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_build.h"

#include "sha256.h"

#include <cstdio>
#include <vector>

namespace tome {
namespace {

uint32_t pe_timestamp_of(HMODULE module) {
    auto base = reinterpret_cast<const uint8_t*>(module);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }
    return nt->FileHeader.TimeDateStamp;
}

std::string file_version_of(const std::wstring& path) {
    DWORD ignored = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) {
        return {};
    }
    std::vector<uint8_t> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) {
        return {};
    }
    VS_FIXEDFILEINFO* info = nullptr;
    UINT info_len = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &info_len) || !info) {
        return {};
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
             static_cast<unsigned>(HIWORD(info->dwFileVersionMS)),
             static_cast<unsigned>(LOWORD(info->dwFileVersionMS)),
             static_cast<unsigned>(HIWORD(info->dwFileVersionLS)),
             static_cast<unsigned>(LOWORD(info->dwFileVersionLS)));
    return buf;
}

} // namespace

bool identify_game_build(HMODULE game, GameBuild& out) {
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(game, path, MAX_PATH);
    if (len == 0 || len == MAX_PATH) {
        return false;
    }

    out.module = game;
    out.base = reinterpret_cast<uintptr_t>(game);
    out.path.assign(path, len);
    out.pe_timestamp = pe_timestamp_of(game);
    out.file_version = file_version_of(out.path);
    out.sha256 = sha256_file(out.path);
    out.known = find_build_by_sha256(out.sha256.c_str());
    out.edition = read_edition(out.base, out.known);
    return true;
}

Edition read_edition(uintptr_t base, const BuildMappings* known) {
    if (!base || !known || !known->game.expansion) return Edition::Unknown;
    return edition_from_game(*reinterpret_cast<const volatile int32_t*>(base + known->game.expansion));
}

bool running_on_wine() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version");
}

} // namespace tome
