// SPDX-License-Identifier: GPL-3.0-or-later
// A stand-in for Shade's "shade.dll" (../shade, runtime/src/hooks.cpp) on
// Windows and Wine, so the tests run Tome's Shade paths (loader/src/shade.*).
// Like Shade, it doesn't change the game's bytes: a hook is a record, its
// trampoline runs the function's own code, and Tome's own writes (the jmp,
// the call's rel32) are what divert calls here. Each native also fails on
// request, and a hook can be made to catch a call on another thread while
// it's going on (FakeShadeDuringHook), as Shade's hook is on before its
// native returns. Each function has one trampoline (ShadeOriginal), the
// same before, during and after its hook.

#include "core/x86.h"

#include <windows.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
uint8_t* g_code = nullptr;  // trampolines and stubs, never freed
size_t g_used = 0;
constexpr size_t kCodeSize = 1 << 16;

std::map<uint32_t, bool> g_hooked;     // function -> hooked now (false: unhooked)
std::map<uint32_t, uint32_t> g_original;  // function -> its trampoline
std::map<uint32_t, uint32_t> g_calls;  // call site -> the call's target in memory when it was redirected
std::string g_fail;                    // the native that fails next
void (*g_during)() = nullptr;
volatile LONG g_entered = 0;

uint8_t* alloc(size_t n) {
    if (!g_code) {
        g_code = static_cast<uint8_t*>(VirtualAlloc(nullptr, kCodeSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    }
    if (!g_code || g_used + n > kCodeSize) return nullptr;
    uint8_t* p = g_code + g_used;
    g_used += (n + 15) & ~size_t(15);
    std::memset(p, 0xCC, n);
    return p;
}

void put_jmp(uint8_t* at, uint32_t to) {
    at[0] = 0xE9;
    const uint32_t rel = to - (reinterpret_cast<uint32_t>(at) + 5);
    std::memcpy(at + 1, &rel, 4);
}

// push value; jmp dest
uint8_t* push_jump(uint32_t dest, uint32_t value) {
    uint8_t* s = alloc(16);
    if (!s) return nullptr;
    s[0] = 0x68;
    std::memcpy(s + 1, &value, 4);
    put_jmp(s + 5, dest);
    return s;
}

bool fails(const char* native) {
    if (g_fail != native) return false;
    g_fail.clear();
    return true;
}

// The trampoline of `target`, made on first use; 0 if its code can't be moved. Under g_lock.
uint32_t original_of(uint32_t target) {
    if (auto it = g_original.find(target); it != g_original.end()) return it->second;
    uint8_t* trampoline = alloc(64);
    if (!trampoline) return 0;
    std::vector<uint8_t> moved;
    size_t consumed = 0;
    std::string error;
    if (!tome::x86::relocate(reinterpret_cast<const uint8_t*>(target), 32, target,
                             reinterpret_cast<uint32_t>(trampoline), 5, moved, consumed, error) ||
        moved.size() + 5 > 64) {
        return 0;
    }
    std::memcpy(trampoline, moved.data(), moved.size());
    put_jmp(trampoline + moved.size(), target + consumed);
    FlushInstructionCache(GetCurrentProcess(), trampoline, 64);
    return g_original[target] = reinterpret_cast<uint32_t>(trampoline);
}

DWORD WINAPI run_during(void* fn) {
    reinterpret_cast<void (*)()>(fn)();
    return 0;
}

// Diverts `target` to `push value; jmp dest` (counting entries), runs
// `during` on another thread until its call has come in, then puts the
// bytes back: memory is as it was when the native returns.
void catch_a_call(uint32_t target, uint32_t dest, uint32_t value, void (*during)()) {
    uint8_t* s = alloc(32);
    if (!s) return;
    // lock inc dword [g_entered]
    s[0] = 0xF0, s[1] = 0xFF, s[2] = 0x05;
    const auto counter = reinterpret_cast<uint32_t>(&g_entered);
    std::memcpy(s + 3, &counter, 4);
    s[7] = 0x68;
    std::memcpy(s + 8, &value, 4);
    put_jmp(s + 12, dest);
    FlushInstructionCache(GetCurrentProcess(), s, 32);

    auto* code = reinterpret_cast<uint8_t*>(target);
    uint8_t saved[5];
    std::memcpy(saved, code, 5);
    DWORD old;
    VirtualProtect(code, 5, PAGE_EXECUTE_READWRITE, &old);
    put_jmp(code, reinterpret_cast<uint32_t>(s));
    FlushInstructionCache(GetCurrentProcess(), code, 5);

    g_entered = 0;
    HANDLE thread = CreateThread(nullptr, 0, run_during, reinterpret_cast<void*>(during), 0, nullptr);
    for (int i = 0; i < 5000 && !g_entered; ++i) Sleep(1);
    Sleep(50);  // let it get as far as it can before the native returns
    std::memcpy(code, saved, 5);
    VirtualProtect(code, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), code, 5);
    if (thread) CloseHandle(thread);
}

} // namespace

extern "C" {

__declspec(dllexport) uint32_t __stdcall ShadeVersion() { return 2; }

__declspec(dllexport) uint32_t __stdcall ShadeOriginal(uint32_t target) {
    AcquireSRWLockExclusive(&g_lock);
    const uint32_t result = fails("ShadeOriginal") ? 0 : original_of(target);
    ReleaseSRWLockExclusive(&g_lock);
    return result;
}

__declspec(dllexport) uint32_t __stdcall ShadeHookPushJump(uint32_t target, uint32_t dest, uint32_t value) {
    AcquireSRWLockExclusive(&g_lock);
    uint32_t result = 0;
    // For this call only, even if it fails: never a later test's hook.
    void (*during)() = g_during;
    g_during = nullptr;
    do {
        if (fails("ShadeHookPushJump")) break;
        const uint32_t trampoline = original_of(target);
        if (!trampoline) break;
        g_hooked[target] = true;
        if (during) catch_a_call(target, dest, value, during);
        result = trampoline;
    } while (false);
    ReleaseSRWLockExclusive(&g_lock);
    return result;
}

__declspec(dllexport) uint32_t __stdcall ShadeMakeStub(uint32_t dest, uint32_t value) {
    AcquireSRWLockExclusive(&g_lock);
    uint8_t* s = fails("ShadeMakeStub") ? nullptr : push_jump(dest, value);
    if (s) FlushInstructionCache(GetCurrentProcess(), s, 16);
    ReleaseSRWLockExclusive(&g_lock);
    return reinterpret_cast<uint32_t>(s);
}

__declspec(dllexport) uint32_t __stdcall ShadeHookCall(uint32_t site, uint32_t dest) {
    (void)dest;  // Tome's write of the call's rel32 is what redirects it here
    AcquireSRWLockExclusive(&g_lock);
    uint32_t ok = 0;
    const auto* code = reinterpret_cast<const uint8_t*>(site);
    if (!fails("ShadeHookCall") && code[0] == 0xE8) {
        int32_t rel;
        std::memcpy(&rel, code + 1, 4);
        g_calls[site] = site + 5 + static_cast<uint32_t>(rel);  // Shade reads it from memory too
        ok = 1;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

__declspec(dllexport) uint32_t __stdcall ShadeUnhook(uint32_t target) {
    AcquireSRWLockExclusive(&g_lock);
    auto it = g_hooked.find(target);
    const bool ok = it != g_hooked.end();
    if (ok) it->second = false;
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

// For the tests (tests/shade/fake_shade.h).

__declspec(dllexport) void FakeShadeFail(const char* native) {
    AcquireSRWLockExclusive(&g_lock);
    g_fail = native;
    ReleaseSRWLockExclusive(&g_lock);
}

__declspec(dllexport) void FakeShadeDuringHook(void (*fn)()) {
    AcquireSRWLockExclusive(&g_lock);
    g_during = fn;
    ReleaseSRWLockExclusive(&g_lock);
}

// 1 hooked, 0 unhooked again, -1 never hooked.
__declspec(dllexport) int FakeShadeHooked(uint32_t target) {
    AcquireSRWLockShared(&g_lock);
    auto it = g_hooked.find(target);
    const int state = it == g_hooked.end() ? -1 : it->second ? 1 : 0;
    ReleaseSRWLockShared(&g_lock);
    return state;
}

// The target the call at `site` had in memory when ShadeHookCall redirected it; 0 if it didn't.
__declspec(dllexport) uint32_t FakeShadeCallTarget(uint32_t site) {
    AcquireSRWLockShared(&g_lock);
    auto it = g_calls.find(site);
    const uint32_t target = it == g_calls.end() ? 0 : it->second;
    ReleaseSRWLockShared(&g_lock);
    return target;
}

} // extern "C"
