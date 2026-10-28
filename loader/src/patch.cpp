#include "patch.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace tome {

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
bool write(uintptr_t address, const void* src, size_t n, DWORD protect, std::string& error) {
    auto at = reinterpret_cast<void*>(address);
    DWORD old_protect;
    if (!VirtualProtect(at, n, protect, &old_protect)) {
        error = "VirtualProtect failed";
        return false;
    }
    std::memcpy(at, src, n);
    VirtualProtect(at, n, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return true;
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

    int32_t new_rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(new_target) - (call_address + 5));
    return write(call_address + 1, &new_rel, sizeof(new_rel), PAGE_EXECUTE_READWRITE, error);
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
    auto value = reinterpret_cast<uintptr_t>(new_value);
    return write(address, &value, sizeof(value), PAGE_READWRITE, error);
}

} // namespace tome
