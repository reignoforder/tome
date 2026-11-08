// SPDX-License-Identifier: GPL-3.0-or-later
// Tome under Shade (loader/src/shade.*), with the fake shade.dll
// (tests/shade/fake_shade.cpp) loaded before anything runs: only in
// tome_hook_runtime_shade_tests, which runs the whole hook runtime suite
// (test_hook_runtime.cpp, test_guard.cpp) that way too.

#include "hooks.h"
#include "patch.h"
#include "shade.h"
#include "shade/fake_shade.h"
#include "test.h"

#include <windows.h>

#include <cstring>

using namespace tome;

extern "C" {
void shade_race();         // int cdecl(int x): x + 10
void shade_no_stub();      // int cdecl(int x): x + 20
void shade_not_lifted();   // int cdecl(int x): x + 30
void shade_caller();       // int cdecl(int x): shade_callee(x) * 2, a call at shade_call_site
void shade_call_site();
void shade_callee();       // int cdecl(int x): x + 1
void shade_caller_2();     // the same, through shade_call_site_2
void shade_call_site_2();
int shade_replacement(int x) { return x + 100; }
extern const void* shade_code_pointer;  // in .text: an operand, as far as Shade is concerned
const void* shade_data_pointer = reinterpret_cast<const void*>(&shade_callee);  // in .data
}

asm(R"(
    .intel_syntax noprefix
    .text
    .globl _shade_race
_shade_race:
    push ebp
    mov ebp, esp
    mov eax, [ebp + 8]
    add eax, 10
    pop ebp
    ret
    .balign 16
    .globl _shade_no_stub
_shade_no_stub:
    push ebp
    mov ebp, esp
    mov eax, [ebp + 8]
    add eax, 20
    pop ebp
    ret
    .balign 16
    .globl _shade_not_lifted
_shade_not_lifted:
    push ebp
    mov ebp, esp
    mov eax, [ebp + 8]
    add eax, 30
    pop ebp
    ret
    .balign 16
    .globl _shade_callee
_shade_callee:
    push ebp
    mov ebp, esp
    mov eax, [ebp + 8]
    inc eax
    pop ebp
    ret
    .balign 16
    .globl _shade_caller
_shade_caller:
    push ebp
    mov ebp, esp
    push dword ptr [ebp + 8]
    .globl _shade_call_site
_shade_call_site:
    call _shade_callee
    add esp, 4
    add eax, eax
    pop ebp
    ret
    .balign 16
    .globl _shade_caller_2
_shade_caller_2:
    push ebp
    mov ebp, esp
    push dword ptr [ebp + 8]
    .globl _shade_call_site_2
_shade_call_site_2:
    call _shade_callee
    add esp, 4
    add eax, eax
    pop ebp
    ret
    .balign 4
    .globl _shade_code_pointer
_shade_code_pointer:
    .long _shade_callee
    .att_syntax prefix
)");

namespace {

// Before main: shade::active() looks once.
const fake_shade::Api* g_shade = fake_shade::load();

using CdeclOne = int (*)(int);

template <typename T>
T as(void (*fn)()) {
    return reinterpret_cast<T>(reinterpret_cast<void*>(fn));
}

uint32_t address_of(void (*fn)()) { return reinterpret_cast<uint32_t>(reinterpret_cast<void*>(fn)); }

std::vector<int16_t> bytes_of(void (*fn)(), size_t n) {
    const auto* code = reinterpret_cast<const uint8_t*>(fn);
    return std::vector<int16_t>(code, code + n);
}

volatile LONG g_race_result = 0;
volatile LONG g_race_done = 0;

void call_race() {
    InterlockedExchange(&g_race_result, as<CdeclOne>(shade_race)(5));
    InterlockedExchange(&g_race_done, 1);
}

} // namespace

TEST(shade_is_active_with_its_dll) {
    CHECK(g_shade != nullptr);
    CHECK(shade::active());
}

// Shade's hook is on before ShadeHookPushJump returns: a call on another
// thread that comes in then finds the Target complete (the trampoline is
// from ShadeOriginal, stored before), not a null trampoline.
TEST(shade_call_while_the_hook_goes_on) {
    if (!g_shade) return;
    g_shade->during_hook(&call_race);
    std::string error;
    hooks::Target* t = hooks::install("shade_race", address_of(shade_race), mappings::Convention::Cdecl, "int(int x)",
                                      bytes_of(shade_race, 8), error);
    CHECK(t != nullptr);
    for (int i = 0; i < 5000 && !g_race_done; ++i) Sleep(1);
    CHECK_EQ(g_race_done, 1L);
    CHECK_EQ(g_race_result, 15L);
    CHECK_EQ(g_shade->hooked(address_of(shade_race)), 1);
    CHECK_EQ(as<CdeclOne>(shade_race)(1), 11);  // through the hook now
}

// Everything that can fail does so before the hook goes on: nothing is
// hooked, and the function's bytes are as they were.
TEST(shade_failed_stub_hooks_nothing) {
    if (!g_shade) return;
    const std::vector<int16_t> before = bytes_of(shade_no_stub, 8);
    g_shade->fail("ShadeMakeStub");
    std::string error;
    CHECK(!hooks::install("shade_no_stub", address_of(shade_no_stub), mappings::Convention::Cdecl, "int(int x)",
                          before, error));
    CHECK(error.find("Shade made no stub") != std::string::npos);
    CHECK_EQ(g_shade->hooked(address_of(shade_no_stub)), -1);
    CHECK(bytes_of(shade_no_stub, 8) == before);
    CHECK(hooks::find("shade_no_stub") == nullptr);
    CHECK_EQ(as<CdeclOne>(shade_no_stub)(1), 21);
}

// Refused by ShadeOriginal or by ShadeHookPushJump itself.
TEST(shade_refuses_what_it_did_not_recompile) {
    if (!g_shade) return;
    for (const char* native : {"ShadeOriginal", "ShadeHookPushJump"}) {
        g_shade->fail(native);
        std::string error;
        CHECK(!hooks::install("shade_not_lifted", address_of(shade_not_lifted), mappings::Convention::Cdecl,
                              "int(int x)", bytes_of(shade_not_lifted, 8), error));
        CHECK(error.find("isn't the start of a function Shade recompiled") != std::string::npos);
        CHECK_EQ(g_shade->hooked(address_of(shade_not_lifted)), -1);
        CHECK(hooks::find("shade_not_lifted") == nullptr);
        CHECK_EQ(as<CdeclOne>(shade_not_lifted)(1), 31);
    }
}

// Shade reads the call's target from memory, so it's asked before the write.
TEST(shade_redirects_a_call_before_writing_it) {
    if (!g_shade) return;
    std::string error;
    CHECK(patch_call(address_of(shade_call_site), address_of(shade_callee),
                     reinterpret_cast<const void*>(&shade_replacement), error));
    CHECK_EQ(g_shade->call_target(address_of(shade_call_site)), address_of(shade_callee));
    CHECK_EQ(as<CdeclOne>(shade_caller)(1), 202);

    // Refused: nothing written.
    const std::vector<int16_t> before = bytes_of(shade_call_site_2, 5);
    g_shade->fail("ShadeHookCall");
    CHECK(!patch_call(address_of(shade_call_site_2), address_of(shade_callee),
                      reinterpret_cast<const void*>(&shade_replacement), error));
    CHECK(error.find("Shade can't redirect the call") != std::string::npos);
    CHECK(bytes_of(shade_call_site_2, 5) == before);
    CHECK_EQ(as<CdeclOne>(shade_caller_2)(1), 4);
}

TEST(shade_code_patches_are_off) {
    static uint8_t code[4] = {0x90, 0x90, 0x90, 0xC3};
    const uint8_t replacement[4] = {0xCC, 0xCC, 0xCC, 0xC3};
    std::string error;
    CHECK(!patch_bytes(reinterpret_cast<uintptr_t>(code), code, replacement, 4, error));
    CHECK(error.find("don't apply under Shade") != std::string::npos);
    CHECK_EQ(code[0], 0x90);
}

// A pointer in code is an instruction's operand, compiled in under Shade:
// refused rather than written for nothing. Data (vtables) is written.
TEST(shade_pointer_patches_in_code_are_refused) {
    const auto callee = reinterpret_cast<uintptr_t>(reinterpret_cast<void*>(&shade_callee));
    std::string error;
    CHECK(!patch_pointer(reinterpret_cast<uintptr_t>(&shade_code_pointer), callee,
                         reinterpret_cast<const void*>(&shade_replacement), error));
    CHECK(error.find("inside code, which doesn't change under Shade") != std::string::npos);
    CHECK(shade_code_pointer == reinterpret_cast<const void*>(callee));

    CHECK(patch_pointer(reinterpret_cast<uintptr_t>(&shade_data_pointer), callee,
                        reinterpret_cast<const void*>(&shade_replacement), error));
    CHECK(shade_data_pointer == reinterpret_cast<const void*>(&shade_replacement));
}
