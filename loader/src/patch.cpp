// SPDX-License-Identifier: GPL-3.0-or-later
#include "patch.h"

#include <windows.h>

#include "pe.h"
#include "shade.h"
#include "unwind.h"

#include <cstdio>
#include <cstring>

namespace tome {

namespace {
SRWLOCK g_originals_lock = SRWLOCK_INIT;
std::map<uintptr_t, uint8_t> g_originals;
} // namespace

void note_original(uintptr_t address, size_t n) {
    ExclusiveLock hold(g_originals_lock);
    auto at = reinterpret_cast<const uint8_t*>(address);
    for (size_t i = 0; i < n; ++i) g_originals.try_emplace(address + i, at[i]);
}

std::map<uintptr_t, uint8_t> original_bytes() {
    SharedLock hold(g_originals_lock);
    return g_originals;
}

bool readable(uintptr_t address, size_t size) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
        return false;
    }
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return address + size <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

namespace {

// Fails unless all `n` bytes at `address` can be read, so the caller's
// comparison can't fault on a wrong mapping.
bool check_readable(uintptr_t address, size_t n, std::string& error) {
    if (readable(address, n)) return true;
    char buf[64];
    snprintf(buf, sizeof(buf), "0x%08lx is not mapped", static_cast<unsigned long>(address));
    error = buf;
    return false;
}

// Copies `n` bytes over `address`, making the page writable around the write
// (`protect`), then flushes the instruction cache in case it was code.
// `before` runs once the page is writable, so nothing fails after it (it
// may fail itself, setting `error`: then nothing is written).
template <typename Before>
bool write(uintptr_t address, const void* src, size_t n, DWORD protect, std::string& error, Before&& before) {
    auto at = reinterpret_cast<void*>(address);
    DWORD old_protect;
    if (!VirtualProtect(at, n, protect, &old_protect)) {
        error = "VirtualProtect failed";
        return false;
    }
    if (!before()) {
        VirtualProtect(at, n, old_protect, &old_protect);
        return false;
    }
    note_original(address, n);
    std::memcpy(at, src, n);
    VirtualProtect(at, n, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return true;
}

bool write(uintptr_t address, const void* src, size_t n, DWORD protect, std::string& error) {
    return write(address, src, n, protect, error, [] { return true; });
}

// Whether `address` is in an executable section of a loaded module (by its
// section headers, which Shade maps too).
bool in_code(uintptr_t address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module)) {
        return false;
    }
    const IMAGE_SECTION_HEADER* section = pe::section_at(module, address);
    return section && (section->Characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE)) != 0;
}

} // namespace

bool patch_call(uintptr_t call_address, uintptr_t expected_target, const void* new_target, std::string& error) {
    if (!check_readable(call_address, 5, error)) return false;
    auto insn = reinterpret_cast<const uint8_t*>(call_address);
    char buf[160];
    if (insn[0] != 0xE8) {
        snprintf(buf, sizeof(buf), "0x%08lx is not a call instruction (found byte 0x%02x)",
                 static_cast<unsigned long>(call_address), insn[0]);
        error = buf;
        return false;
    }
    int32_t rel;
    std::memcpy(&rel, insn + 1, sizeof(rel));
    uintptr_t current = call_address + 5 + static_cast<uintptr_t>(rel);
    if (current != expected_target) {
        snprintf(buf, sizeof(buf), "call at 0x%08lx targets 0x%08lx, expected 0x%08lx",
                 static_cast<unsigned long>(call_address), static_cast<unsigned long>(current),
                 static_cast<unsigned long>(expected_target));
        error = buf;
        return false;
    }

    // Under Shade the call is C code: Shade redirects it, reading the call's
    // target from memory, so before the write (which keeps memory as on
    // Windows). Only once the page is writable: a redirect never stays
    // behind a failed patch, since Shade can't take one back.
    int32_t new_rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(new_target) - (call_address + 5));
    return write(call_address + 1, &new_rel, sizeof(new_rel), PAGE_EXECUTE_READWRITE, error, [&] {
        if (!shade::active() || shade::hook_call(call_address, reinterpret_cast<uintptr_t>(new_target))) return true;
        snprintf(buf, sizeof(buf), "Shade can't redirect the call at 0x%08lx",
                 static_cast<unsigned long>(call_address));
        error = buf;
        return false;
    });
}

bool patch_bytes(uintptr_t address, const uint8_t* expected, const uint8_t* replacement, size_t n,
                 std::string& error) {
    if (!check_readable(address, n, error)) return false;
    if (std::memcmp(reinterpret_cast<const void*>(address), expected, n) != 0) {
        char buf[160];
        snprintf(buf, sizeof(buf), "bytes at 0x%08lx aren't what the mapping expects",
                 static_cast<unsigned long>(address));
        error = buf;
        return false;
    }
    if (shade::active()) {
        // Recompiled code doesn't change when its bytes do.
        error = "code patches don't apply under Shade (the game's code is recompiled)";
        return false;
    }
    return write(address, replacement, n, PAGE_EXECUTE_READWRITE, error);
}

bool patch_pointer(uintptr_t address, uintptr_t expected, const void* new_value, std::string& error) {
    if (!check_readable(address, sizeof(uintptr_t), error)) return false;
    uintptr_t current = *reinterpret_cast<const uintptr_t*>(address);
    if (current != expected) {
        char buf[160];
        snprintf(buf, sizeof(buf), "pointer at 0x%08lx is 0x%08lx, expected 0x%08lx",
                 static_cast<unsigned long>(address), static_cast<unsigned long>(current),
                 static_cast<unsigned long>(expected));
        error = buf;
        return false;
    }
    // A pointer inside an instruction (an operand) is compiled in under
    // Shade: writing it would change nothing and look like it worked.
    if (shade::active() && in_code(address)) {
        char buf[160];
        snprintf(buf, sizeof(buf), "0x%08lx is inside code, which doesn't change under Shade (it's recompiled)",
                 static_cast<unsigned long>(address));
        error = buf;
        return false;
    }
    auto value = reinterpret_cast<uintptr_t>(new_value);
    return write(address, &value, sizeof(value), PAGE_READWRITE, error);
}

} // namespace tome
