#include "patch.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace tome {

bool patch_call(uintptr_t call_address, uintptr_t expected_target, const void* new_target, std::string& error) {
    auto insn = reinterpret_cast<uint8_t*>(call_address);
    char buf[160];

    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(insn, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
        snprintf(buf, sizeof(buf), "0x%08lx is not mapped", static_cast<unsigned long>(call_address));
        error = buf;
        return false;
    }
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
    DWORD old_protect;
    if (!VirtualProtect(insn + 1, sizeof(new_rel), PAGE_EXECUTE_READWRITE, &old_protect)) {
        error = "VirtualProtect failed";
        return false;
    }
    std::memcpy(insn + 1, &new_rel, sizeof(new_rel));
    VirtualProtect(insn + 1, sizeof(new_rel), old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), insn, 5);
    return true;
}

bool patch_pointer(uintptr_t address, uintptr_t expected, const void* new_value, std::string& error) {
    auto slot = reinterpret_cast<uintptr_t*>(address);
    char buf[160];
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(slot, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
        snprintf(buf, sizeof(buf), "0x%08lx is not mapped", static_cast<unsigned long>(address));
        error = buf;
        return false;
    }
    if (*slot != expected) {
        snprintf(buf, sizeof(buf), "pointer at 0x%08lx is 0x%08lx, expected 0x%08lx",
                 static_cast<unsigned long>(address), static_cast<unsigned long>(*slot),
                 static_cast<unsigned long>(expected));
        error = buf;
        return false;
    }
    DWORD old_protect;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old_protect)) {
        error = "VirtualProtect failed";
        return false;
    }
    *slot = reinterpret_cast<uintptr_t>(new_value);
    VirtualProtect(slot, sizeof(*slot), old_protect, &old_protect);
    return true;
}

} // namespace tome
