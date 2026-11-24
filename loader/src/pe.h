// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The headers of a loaded module (one the loader mapped: its headers are in
// memory, readable; under Shade too).

#include <windows.h>

#include <cstdint>

namespace tome::pe {

// The module's NT headers, nullptr if it has no valid DOS/NT signature.
inline const IMAGE_NT_HEADERS32* nt_headers(HMODULE module) {
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt : nullptr;
}

// The section of `module` that holds `address`, nullptr if none does.
inline const IMAGE_SECTION_HEADER* section_at(HMODULE module, uintptr_t address) {
    const IMAGE_NT_HEADERS32* nt = nt_headers(module);
    if (!nt) return nullptr;
    const uintptr_t rva = address - reinterpret_cast<uintptr_t>(module);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (size_t i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const uint32_t size = section[i].Misc.VirtualSize ? section[i].Misc.VirtualSize : section[i].SizeOfRawData;
        if (rva >= section[i].VirtualAddress && rva - section[i].VirtualAddress < size) return &section[i];
    }
    return nullptr;
}

} // namespace tome::pe
