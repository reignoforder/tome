// SPDX-License-Identifier: GPL-3.0-or-later
// The hook engine (loader/src/hooks.cpp) on real code: small functions in
// each calling convention, written in assembly with MSVC-style prologues,
// hooked in this process and called the way the game would call them.

#include "guard.h"
#include "hooks.h"
#include "test.h"

#include <windows.h>

#include <cstring>

using namespace tome;

extern "C" {
// Only their addresses are used; each is called through a pointer of its real type.
void rt_cdecl_add3();     // int cdecl(int a, int b, int c): a + b + c
void rt_stdcall_sub();    // int stdcall(int a, int b): a - b
void rt_thiscall_get();   // int thiscall(int* this, int add): *this + add
void rt_fastcall_mad();   // int fastcall(int a, int b, int c): a * b + c
void rt_call_first();     // int cdecl(int x): starts with a call; 7 + x
void rt_wide();           // uint64_t cdecl(int x): 0x12345678'00000000 | x
void rt_unhooked();      // void cdecl(void), never hooked
void rt_imported();      // int stdcall(int a): a * 2, called through g_rt_import_slot
void rt_float_half();    // float cdecl(float x): x / 2
void rt_double_scale();  // double stdcall(double x, int k): x * k
void rt_fastcall_wide(); // int fastcall(int64_t a, int b, int c): low(a) + high(a) + b - c
void rt_varsum();        // int cdecl(int n, ...): the sum of n ints
void rt_short_jmp();     // int cdecl(int x): x ? x : 100; starts with jmp short
void rt_triangle();      // int fastcall(int n): n + ... + 1, with a loop inside the first bytes
void rt_tiny();          // int cdecl(void): 1; returns within 4 bytes, then padding
void rt_gated();         // void cdecl(void), for the gate tests
void rt_counted();       // int cdecl(int x): x + 1, counting its calls in g_rt_counted_calls
void rt_inner();         // void cdecl(void), called from a handler of rt_counted in the crash tests
int g_rt_counted_calls;
// Calls an MSVC fastcall int(int64_t a, int b, int c) the way MSVC does: a on
// the stack, b in ecx, c in edx. (GCC's fastcall passes 64-bit arguments
// differently, so it can't make this call itself.)
int rt_call_fastcall_wide(const void* fn, uint32_t a_low, uint32_t a_high, int b, int c);
// Calls `fn` with `count` stack arguments of 1 and ecx/edx set; returns how
// far esp moved down across the call (0: the callee popped its arguments).
uint32_t rt_esp_delta(const void* fn, uint32_t count, uint32_t ecx, uint32_t edx);
}

asm(R"(
    .intel_syntax noprefix
    .text
    .globl _rt_cdecl_add3
_rt_cdecl_add3:
    push ebp
    mov ebp, esp
    mov eax, [ebp + 8]
    add eax, [ebp + 12]
    add eax, [ebp + 16]
    pop ebp
    ret

    .globl _rt_stdcall_sub
_rt_stdcall_sub:
    push ebp
    mov ebp, esp
    mov eax, [ebp + 8]
    sub eax, [ebp + 12]
    pop ebp
    ret 8

    .globl _rt_thiscall_get
_rt_thiscall_get:
    push ebp
    mov ebp, esp
    mov eax, [ecx]
    add eax, [ebp + 8]
    pop ebp
    ret 4

    .globl _rt_fastcall_mad
_rt_fastcall_mad:
    push ebp
    mov ebp, esp
    mov eax, ecx
    imul eax, edx
    add eax, [ebp + 8]
    pop ebp
    ret 4

    .globl _rt_call_first
_rt_call_first:
    call .Lrt_seven
    add eax, [esp + 4]
    ret
.Lrt_seven:
    mov eax, 7
    ret

    .globl _rt_wide
_rt_wide:
    push ebp
    mov ebp, esp
    mov eax, [ebp + 8]
    mov edx, 0x12345678
    pop ebp
    ret

    .globl _rt_imported
_rt_imported:
    mov eax, [esp + 4]
    add eax, eax
    ret 4

    .globl _rt_unhooked
_rt_unhooked:
    push ebp
    mov ebp, esp
    nop
    nop
    nop
    pop ebp
    ret

    .globl _rt_float_half
_rt_float_half:
    push ebp
    mov ebp, esp
    fld dword ptr [ebp + 8]
    fld1
    fadd st(0), st(0)
    fdivp st(1), st(0)
    pop ebp
    ret

    .globl _rt_double_scale
_rt_double_scale:
    push ebp
    mov ebp, esp
    fild dword ptr [ebp + 16]
    fmul qword ptr [ebp + 8]
    pop ebp
    ret 12

    .globl _rt_fastcall_wide
_rt_fastcall_wide:
    push ebp
    mov ebp, esp
    mov eax, [ebp + 8]
    add eax, [ebp + 12]
    add eax, ecx
    sub eax, edx
    pop ebp
    ret 8

    .globl _rt_varsum
_rt_varsum:
    push ebp
    mov ebp, esp
    push esi
    mov ecx, [ebp + 8]
    lea esi, [ebp + 12]
    xor eax, eax
.Lrt_varsum_next:
    test ecx, ecx
    jz .Lrt_varsum_done
    add eax, [esi]
    add esi, 4
    dec ecx
    jmp .Lrt_varsum_next
.Lrt_varsum_done:
    pop esi
    pop ebp
    ret

    .globl _rt_short_jmp
_rt_short_jmp:
    push ebp
    mov ebp, esp
    jmp .Lrt_short_body
    int3
    int3
    int3
.Lrt_short_body:
    mov eax, [ebp + 8]
    test eax, eax
    jnz .Lrt_short_done
    mov eax, 100
.Lrt_short_done:
    pop ebp
    ret

    .globl _rt_triangle
_rt_triangle:
    xor eax, eax
.Lrt_triangle_loop:
    add eax, ecx
    loop .Lrt_triangle_loop
    ret

    .globl _rt_tiny
_rt_tiny:
    xor eax, eax
    inc eax
    ret
    int3
    int3
    int3
    int3

    .globl _rt_gated
_rt_gated:
    push ebp
    mov ebp, esp
    nop
    nop
    nop
    pop ebp
    ret

    .globl _rt_counted
_rt_counted:
    push ebp
    mov ebp, esp
    mov eax, [ebp + 8]
    inc eax
    inc dword ptr [_g_rt_counted_calls]
    pop ebp
    ret

    .globl _rt_inner
_rt_inner:
    push ebp
    mov ebp, esp
    nop
    nop
    nop
    pop ebp
    ret

    .globl _rt_call_fastcall_wide
_rt_call_fastcall_wide:
    push ebp
    mov ebp, esp
    push dword ptr [ebp + 16]
    push dword ptr [ebp + 12]
    mov ecx, [ebp + 20]
    mov edx, [ebp + 24]
    call dword ptr [ebp + 8]
    mov esp, ebp
    pop ebp
    ret

    .globl _rt_esp_delta
_rt_esp_delta:
    push ebp
    mov ebp, esp
    push esi
    mov esi, esp
    mov ecx, [ebp + 12]
.Lrt_push:
    test ecx, ecx
    jz .Lrt_call
    push 1
    dec ecx
    jmp .Lrt_push
.Lrt_call:
    mov ecx, [ebp + 16]
    mov edx, [ebp + 20]
    call dword ptr [ebp + 8]
    mov eax, esi
    sub eax, esp
    mov esp, esi
    pop esi
    pop ebp
    ret
    .att_syntax prefix
)");

namespace {

using CdeclAdd3 = int (*)(int, int, int);
using StdcallSub = int(__attribute__((stdcall)) *)(int, int);
using StdcallImported = int(__attribute__((stdcall)) *)(int);
// An import address table slot, as game.dll has one per imported function.
volatile uintptr_t g_rt_import_slot = reinterpret_cast<uintptr_t>(&rt_imported);
using ThiscallGet = int(__attribute__((thiscall)) *)(int*, int);
using FastcallMad = int(__attribute__((fastcall)) *)(int, int, int);
using CdeclOne = int (*)(int);
using CdeclWide = uint64_t (*)(int);

template <typename T>
T as(void (*fn)()) {
    return reinterpret_cast<T>(reinterpret_cast<void*>(fn));
}

hooks::Target* hook(const char* name, void (*fn)(), mappings::Convention conv, const char* sig) {
    const auto* code = reinterpret_cast<const uint8_t*>(fn);
    std::vector<int16_t> bytes(code, code + 8);
    std::string error;
    hooks::Target* t = hooks::install(name, reinterpret_cast<uintptr_t>(fn), conv, sig, bytes, error);
    if (!t) test::fail(__FILE__, __LINE__, std::string(name) + ": " + error);
    return t;
}

hooks::Handler handler(int32_t phase, tome_hook_fn fn, int32_t priority = 0, void* user = nullptr) {
    hooks::Handler h;
    h.phase = phase;
    h.priority = priority;
    h.fn = fn;
    h.user = user;
    return h;
}

int g_seen[8];
uint32_t g_return_address;

void TOME_CALL record_args(tome_mod*, tome_hook_call* call, void*) {
    for (int i = 0; i < call->arg_count && i < 8; ++i) g_seen[i] = static_cast<int>(call->args[i]);
    g_return_address = call->return_address;
}

void TOME_CALL add_100_to_last_arg(tome_mod*, tome_hook_call* call, void*) {
    call->args[call->arg_count - 1] += 100;
}

void TOME_CALL negate_result(tome_mod*, tome_hook_call* call, void*) {
    call->result = static_cast<uint32_t>(-static_cast<int32_t>(call->result));
}

void TOME_CALL replace_twice(tome_mod*, tome_hook_call* call, void*) {
    hooks::call_next(call);
    uint32_t first = call->result;
    hooks::call_next(call);
    call->result += first;
}

void TOME_CALL skip_with_42(tome_mod*, tome_hook_call* call, void*) {
    call->skip_original = 1;
    call->result = 42;
}

} // namespace

TEST(hook_runtime_cdecl) {
    hooks::Target* t = hook("rt_cdecl_add3", rt_cdecl_add3, mappings::Convention::Cdecl, "int(int a, int b, int c)");
    if (!t) return;
    auto fn = as<CdeclAdd3>(rt_cdecl_add3);
    CHECK_EQ(fn(1, 2, 3), 6);  // hooked, empty chain
    hooks::add(t, handler(TOME_HOOK_BEFORE, record_args));
    hooks::add(t, handler(TOME_HOOK_BEFORE, add_100_to_last_arg, -1));
    hooks::add(t, handler(TOME_HOOK_AFTER, negate_result));
    CHECK_EQ(fn(1, 2, 3), -106);
    CHECK_EQ(g_seen[0], 1);
    CHECK_EQ(g_seen[2], 3);  // record_args ran first (priority 0 > -1)
    CHECK(g_return_address != 0);
    CHECK_EQ(rt_esp_delta(reinterpret_cast<const void*>(rt_cdecl_add3), 3, 0, 0), 12u);  // caller cleans

    CHECK_EQ(hooks::remove(t, nullptr, TOME_HOOK_AFTER, negate_result, nullptr), size_t(1));
    CHECK_EQ(fn(1, 2, 3), 106);

    // The same fn and user twice: remove_gate takes out only its own registration.
    hooks::Handler first = handler(TOME_HOOK_AFTER, negate_result);
    first.gate = std::make_shared<hooks::Gate>();
    hooks::Handler second = handler(TOME_HOOK_AFTER, negate_result);
    second.gate = std::make_shared<hooks::Gate>();
    hooks::add(t, first);
    hooks::add(t, second);
    CHECK_EQ(fn(1, 2, 3), 106);  // negated twice
    CHECK(hooks::remove_gate(t, first.gate.get()));
    CHECK(first.gate->revoked && !second.gate->revoked);
    CHECK_EQ(fn(1, 2, 3), -106);
    CHECK(!hooks::remove_gate(t, first.gate.get()));
    std::vector<const void*> removed;
    CHECK_EQ(hooks::remove(t, nullptr, TOME_HOOK_AFTER, negate_result, nullptr, &removed), size_t(1));
    CHECK(removed.size() == 1 && removed[0] == second.gate.get());
    CHECK_EQ(fn(1, 2, 3), 106);
    hooks::remove(t, nullptr, TOME_HOOK_BEFORE, add_100_to_last_arg, nullptr);
    hooks::remove(t, nullptr, TOME_HOOK_BEFORE, record_args, nullptr);
    CHECK_EQ(fn(1, 2, 3), 6);

    // Installing the same name again returns the same target.
    std::string error;
    CHECK(hooks::install("rt_cdecl_add3", 0, mappings::Convention::Cdecl, "int(int)", {}, error) == t);
    CHECK(hooks::find("rt_cdecl_add3") == t);
}

TEST(hook_runtime_stdcall_pops_its_arguments) {
    hooks::Target* t = hook("rt_stdcall_sub", rt_stdcall_sub, mappings::Convention::Stdcall, "int(int a, int b)");
    if (!t) return;
    hooks::add(t, handler(TOME_HOOK_REPLACE, replace_twice));
    auto fn = as<StdcallSub>(rt_stdcall_sub);
    CHECK_EQ(fn(10, 3), 14);
    CHECK_EQ(rt_esp_delta(reinterpret_cast<const void*>(rt_stdcall_sub), 2, 0, 0), 0u);
}

TEST(hook_runtime_thiscall) {
    hooks::Target* t =
        hook("rt_thiscall_get", rt_thiscall_get, mappings::Convention::Thiscall, "int(int* this, int add)");
    if (!t) return;
    hooks::add(t, handler(TOME_HOOK_BEFORE, record_args));
    hooks::add(t, handler(TOME_HOOK_BEFORE, add_100_to_last_arg, -1));
    int value = 5;
    auto fn = as<ThiscallGet>(rt_thiscall_get);
    CHECK_EQ(fn(&value, 1), 106);
    CHECK_EQ(g_seen[0], static_cast<int>(reinterpret_cast<uintptr_t>(&value)));
    CHECK_EQ(g_seen[1], 1);
    CHECK_EQ(rt_esp_delta(reinterpret_cast<const void*>(rt_thiscall_get), 1, reinterpret_cast<uintptr_t>(&value), 0),
             0u);
}

TEST(hook_runtime_fastcall) {
    hooks::Target* t =
        hook("rt_fastcall_mad", rt_fastcall_mad, mappings::Convention::Fastcall, "int(int a, int b, int c)");
    if (!t) return;
    hooks::add(t, handler(TOME_HOOK_BEFORE, record_args));
    auto fn = as<FastcallMad>(rt_fastcall_mad);
    CHECK_EQ(fn(6, 7, 8), 50);
    CHECK_EQ(g_seen[0], 6);
    CHECK_EQ(g_seen[1], 7);
    CHECK_EQ(g_seen[2], 8);
    hooks::add(t, handler(TOME_HOOK_BEFORE, add_100_to_last_arg, -1));  // the stack argument
    CHECK_EQ(fn(6, 7, 8), 150);
    CHECK_EQ(rt_esp_delta(reinterpret_cast<const void*>(rt_fastcall_mad), 1, 2, 3), 0u);
    hooks::add(t, handler(TOME_HOOK_BEFORE, skip_with_42, 50));
    CHECK_EQ(fn(6, 7, 8), 42);
    CHECK_EQ(rt_esp_delta(reinterpret_cast<const void*>(rt_fastcall_mad), 1, 2, 3), 0u);  // still pops when skipped
}

TEST(hook_runtime_moves_a_call) {
    hooks::Target* t = hook("rt_call_first", rt_call_first, mappings::Convention::Cdecl, "int(int x)");
    if (!t) return;
    hooks::add(t, handler(TOME_HOOK_AFTER, negate_result));
    CHECK_EQ(as<CdeclOne>(rt_call_first)(5), -12);
}

TEST(hook_runtime_wide_result) {
    hooks::Target* t = hook("rt_wide", rt_wide, mappings::Convention::Cdecl, "uint64_t(int x)");
    if (!t) return;
    hooks::add(t, handler(TOME_HOOK_BEFORE, add_100_to_last_arg));
    CHECK_EQ(as<CdeclWide>(rt_wide)(1), 0x1234567800000065ull);
}

TEST(hook_runtime_reserve) {
    // A reserved name is remembered and others aren't; api_hook/api_unhook
    // check this to keep mods off a built-in patch's functions.
    CHECK(!hooks::is_reserved("rt_reserved_name"));
    hooks::reserve("rt_reserved_name");
    CHECK(hooks::is_reserved("rt_reserved_name"));
    CHECK(!hooks::is_reserved("rt_other_name"));
    hooks::reserve("rt_reserved_name");  // idempotent
    CHECK(hooks::is_reserved("rt_reserved_name"));
}

TEST(hook_runtime_refuses_bad_bytes) {
    std::string error;
    std::vector<int16_t> wrong(8, 0x90);
    CHECK(!hooks::install("rt_wrong", reinterpret_cast<uintptr_t>(rt_unhooked), mappings::Convention::Cdecl,
                          "void(void)", wrong, error));
    CHECK(error.find("byte 0 is 0x55, expected 0x90") != std::string::npos);
    CHECK_EQ(reinterpret_cast<const uint8_t*>(rt_unhooked)[0], uint8_t(0x55));  // not touched
    CHECK_EQ(reinterpret_cast<const uint8_t*>(rt_cdecl_add3)[0], uint8_t(0xE9));  // hooked earlier

    const auto* code = reinterpret_cast<const uint8_t*>(rt_unhooked);
    std::vector<int16_t> bytes(code, code + 8);
    CHECK(!hooks::install("rt_again", reinterpret_cast<uintptr_t>(rt_cdecl_add3), mappings::Convention::Cdecl,
                          "int(int a, int b, int c)", bytes, error));
    CHECK(error.find("already hooked under the name \"rt_cdecl_add3\"") != std::string::npos);
    error.clear();
    CHECK(!hooks::find_or_install("Anything", error));
    CHECK(error.find("not mapped") != std::string::npos);
}

namespace {

void TOME_CALL double_float_arg_and_result(tome_mod*, tome_hook_call* call, void* user) {
    if (user) {  // BEFORE
        tome_hook_set_arg_float(call, 0, tome_hook_arg_float(call, 0) * 2);
    } else {     // AFTER
        call->result_float += 0.25;
    }
}

void TOME_CALL scale_double(tome_mod*, tome_hook_call* call, void* user) {
    if (user) {  // BEFORE: x is two words, k comes after them
        CHECK_EQ(call->arg_count, 3);
        tome_hook_set_arg_double(call, 0, tome_hook_arg_double(call, 0) + 1.0);
        call->args[2] += 1;
    } else {
        call->result_float = -call->result_float;
    }
}

void TOME_CALL skip_with_float(tome_mod*, tome_hook_call* call, void*) {
    call->skip_original = 1;
    call->result_float = 1.5;
}

void TOME_CALL check_wide_args(tome_mod*, tome_hook_call* call, void*) {
    CHECK_EQ(call->arg_count, 4);
    CHECK_EQ(tome_hook_arg_u64(call, 0), 0x0000000500000003ull);
    CHECK_EQ(call->args[2], 10u);  // b, from ecx
    CHECK_EQ(call->args[3], 4u);   // c, from edx
    tome_hook_set_arg_u64(call, 0, 0x0000001000000020ull);
}

int g_extra_seen = -1;

void TOME_CALL varsum_before(tome_mod*, tome_hook_call* call, void*) {
    // n, then the variadic ints after the fixed arguments.
    const int n = static_cast<int>(call->args[0]);
    g_extra_seen = call->extra_count >= n ? static_cast<int>(call->args[call->arg_count + n - 1]) : -1;
    call->args[call->arg_count] = 1000;  // the first variadic argument
}

void TOME_CALL add_one(tome_mod*, tome_hook_call* call, void*) {
    call->result += 1;
}

} // namespace

TEST(hook_runtime_float_and_double_results) {
    hooks::Target* t = hook("rt_float_half", rt_float_half, mappings::Convention::Cdecl, "float(float x)");
    if (!t) return;
    auto half = as<float (*)(float)>(rt_float_half);
    CHECK_EQ(half(3.0f), 1.5f);  // empty chain: st(0) passes through
    hooks::add(t, handler(TOME_HOOK_BEFORE, double_float_arg_and_result, 0, t));
    hooks::add(t, handler(TOME_HOOK_AFTER, double_float_arg_and_result));
    CHECK_EQ(half(3.0f), 3.25f);

    t = hook("rt_double_scale", rt_double_scale, mappings::Convention::Stdcall, "double(double x, int k)");
    if (!t) return;
    auto scale = as<double(__attribute__((stdcall)) *)(double, int)>(rt_double_scale);
    CHECK_EQ(scale(2.5, 4), 10.0);
    hooks::add(t, handler(TOME_HOOK_BEFORE, scale_double, 0, t));
    hooks::add(t, handler(TOME_HOOK_AFTER, scale_double));
    CHECK_EQ(scale(2.5, 4), -17.5);  // (2.5 + 1) * 5, negated
    hooks::add(t, handler(TOME_HOOK_BEFORE, skip_with_float, -1));
    CHECK_EQ(scale(2.5, 4), -1.5);   // skipped: the AFTER handler still negates
    // A balanced x87 stack: a leak would show up as NaN after 8 calls.
    for (int i = 0; i < 20; ++i) CHECK_EQ(scale(2.5, 4), -1.5);
}

TEST(hook_runtime_64_bit_arguments) {
    hooks::Target* t = hook("rt_fastcall_wide", rt_fastcall_wide, mappings::Convention::Fastcall,
                            "int(int64_t a, int b, int c)");
    if (!t) return;
    auto fn = reinterpret_cast<const void*>(rt_fastcall_wide);
    CHECK_EQ(rt_call_fastcall_wide(fn, 3, 5, 10, 4), 14);
    hooks::add(t, handler(TOME_HOOK_BEFORE, check_wide_args));
    CHECK_EQ(rt_call_fastcall_wide(fn, 3, 5, 10, 4), 0x30 + 6);
    hooks::remove(t, nullptr, TOME_HOOK_BEFORE, check_wide_args, nullptr);  // it expects these arguments
    hooks::add(t, handler(TOME_HOOK_AFTER, add_one));
    CHECK_EQ(rt_esp_delta(reinterpret_cast<const void*>(rt_fastcall_wide), 2, 0, 0), 0u);
}

TEST(hook_runtime_variadic) {
    hooks::Target* t = hook("rt_varsum", rt_varsum, mappings::Convention::Cdecl, "int(int n, ...)");
    if (!t) return;
    auto sum = as<int (*)(int, ...)>(rt_varsum);
    CHECK_EQ(sum(3, 1, 2, 3), 6);
    hooks::add(t, handler(TOME_HOOK_BEFORE, varsum_before));
    CHECK_EQ(sum(3, 1, 2, 3), 1005);  // the first variadic argument replaced
    CHECK_EQ(g_extra_seen, 3);
    CHECK_EQ(sum(5, 1, 2, 3, 4, 5), 1014);
    CHECK_EQ(g_extra_seen, 5);
    CHECK_EQ(rt_esp_delta(reinterpret_cast<const void*>(rt_varsum), 3, 0, 0), 12u);
}

TEST(hook_runtime_short_branches_and_tiny_functions) {
    hooks::Target* t = hook("rt_short_jmp", rt_short_jmp, mappings::Convention::Cdecl, "int(int x)");
    if (!t) return;
    hooks::add(t, handler(TOME_HOOK_AFTER, add_one));
    CHECK_EQ(as<CdeclOne>(rt_short_jmp)(0), 101);
    CHECK_EQ(as<CdeclOne>(rt_short_jmp)(7), 8);

    // The loop jumps back into the moved bytes: it has to stay in the copy,
    // or each iteration would run the chain again.
    t = hook("rt_triangle", rt_triangle, mappings::Convention::Fastcall, "int(int n)");
    if (!t) return;
    hooks::add(t, handler(TOME_HOOK_AFTER, add_one));
    CHECK_EQ(as<int(__attribute__((fastcall)) *)(int)>(rt_triangle)(4), 11);

    t = hook("rt_tiny", rt_tiny, mappings::Convention::Cdecl, "int(void)");
    if (!t) return;
    hooks::add(t, handler(TOME_HOOK_AFTER, add_one));
    CHECK_EQ(as<int (*)()>(rt_tiny)(), 2);
}

namespace {

HANDLE g_entered, g_release;
std::vector<std::shared_ptr<hooks::Gate>> g_gates;
bool g_idle_inside = true;

void TOME_CALL blocking_handler(tome_mod*, tome_hook_call*, void* user) {
    if (user) {  // called on the test's own thread
        g_idle_inside = hooks::idle(g_gates, 1000);
        return;
    }
    SetEvent(g_entered);
    WaitForSingleObject(g_release, 5000);
}

DWORD WINAPI call_gated(void*) {
    as<void (*)()>(rt_gated)();
    return 0;
}

} // namespace

TEST(hook_runtime_unload_waits_for_running_handlers) {
    hooks::Target* t = hook("rt_gated", rt_gated, mappings::Convention::Cdecl, "void(void)");
    if (!t) return;
    g_entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    hooks::Handler h = handler(TOME_HOOK_BEFORE, blocking_handler);
    h.gate = std::make_shared<hooks::Gate>();
    g_gates = {h.gate};
    hooks::add(t, h);

    HANDLE thread = CreateThread(nullptr, 0, call_gated, nullptr, 0, nullptr);
    CHECK(WaitForSingleObject(g_entered, 5000) == WAIT_OBJECT_0);
    // Revoked while another thread is inside it: not idle until it returns.
    CHECK_EQ(hooks::remove(t, nullptr, TOME_HOOK_BEFORE, blocking_handler, nullptr), size_t(1));
    CHECK(h.gate->revoked);
    CHECK(!hooks::idle(g_gates, 50));
    SetEvent(g_release);
    CHECK(hooks::idle(g_gates, 5000));
    WaitForSingleObject(thread, 5000);
    CloseHandle(thread);

    // From inside the handler itself (same thread): never idle, and no wait.
    hooks::Handler self = handler(TOME_HOOK_BEFORE, blocking_handler, 0, t);
    self.gate = std::make_shared<hooks::Gate>();
    g_gates = {self.gate};
    hooks::add(t, self);
    const ULONGLONG start = GetTickCount64();
    as<void (*)()>(rt_gated)();
    CHECK(!g_idle_inside);
    CHECK(GetTickCount64() - start < 500);
    CHECK(hooks::idle(g_gates, 0));  // returned: idle now, even though still hooked
    g_gates.clear();
    CloseHandle(g_entered);
    CloseHandle(g_release);
}

// --- crashes in handlers (guard.cpp) ---------------------------------------------

namespace {

int g_crash_after_runs = 0;
int g_crash_replace_results[2];

void crash() {
    *static_cast<volatile int*>(nullptr) = 1;
}

void TOME_CALL crash_before(tome_mod*, tome_hook_call*, void*) {
    crash();
}

void TOME_CALL crash_replace(tome_mod*, tome_hook_call*, void*) {
    crash();
}

void TOME_CALL next_then_crash(tome_mod*, tome_hook_call* call, void*) {
    hooks::call_next(call);
    call->result += 1000;
    crash();
}

void TOME_CALL count_after(tome_mod*, tome_hook_call* call, void*) {
    ++g_crash_after_runs;
    if (g_crash_after_runs <= 2) g_crash_replace_results[g_crash_after_runs - 1] = static_cast<int>(call->result);
}

// A handler of Tome's own (no mod, so no guard) that crashes, with a gate.
void TOME_CALL unguarded_crash(tome_mod*, tome_hook_call*, void*) {
    crash();
}

void TOME_CALL calls_inner(tome_mod*, tome_hook_call*, void*) {
    as<void (*)()>(rt_inner)();
}

} // namespace

TEST(hook_runtime_crashing_handlers) {
    hooks::Target* t = hook("rt_counted", rt_counted, mappings::Convention::Cdecl, "int(int x)");
    if (!t) return;
    auto fn = as<CdeclOne>(rt_counted);
    guard::set_fault_sink(nullptr);
    tome_mod a, b;
    a.id = "a";
    b.id = "b";

    // BEFORE crashes: the call goes on, the other mod's AFTER still runs.
    hooks::Handler before = handler(TOME_HOOK_BEFORE, crash_before);
    before.mod = &a;
    before.gate = std::make_shared<hooks::Gate>();
    hooks::Handler after = handler(TOME_HOOK_AFTER, count_after);
    after.mod = &b;
    hooks::add(t, before);
    hooks::add(t, after);
    g_rt_counted_calls = 0;
    g_crash_after_runs = 0;
    CHECK_EQ(fn(1), 2);
    CHECK_EQ(g_rt_counted_calls, 1);
    CHECK_EQ(g_crash_after_runs, 1);
    CHECK(a.faulted);
    CHECK(!b.faulted);
    CHECK_EQ(before.gate->busy.load(), 0);
    CHECK_EQ(hooks::running_on_this_thread(*before.gate), 0);
    hooks::remove_gate(t, before.gate.get());

    // REPLACE crashes before calling next: the original runs instead.
    tome_mod c;
    hooks::Handler replace = handler(TOME_HOOK_REPLACE, crash_replace);
    replace.mod = &c;
    hooks::add(t, replace);
    g_rt_counted_calls = 0;
    CHECK_EQ(fn(10), 11);
    CHECK_EQ(g_rt_counted_calls, 1);
    CHECK(c.faulted);
    // Once crashed, its handlers are skipped.
    CHECK_EQ(fn(20), 21);
    CHECK_EQ(g_rt_counted_calls, 2);
    hooks::remove(t, &c, TOME_HOOK_REPLACE, crash_replace, nullptr);

    // REPLACE crashes after calling next: the original isn't run again, and
    // the result is what it was at the crash.
    tome_mod d;
    hooks::Handler late = handler(TOME_HOOK_REPLACE, next_then_crash);
    late.mod = &d;
    hooks::add(t, late);
    g_rt_counted_calls = 0;
    CHECK_EQ(fn(30), 1031);
    CHECK_EQ(g_rt_counted_calls, 1);
    CHECK(d.faulted);
    hooks::remove(t, &d, TOME_HOOK_REPLACE, next_then_crash, nullptr);
    hooks::remove(t, &b, TOME_HOOK_AFTER, count_after, nullptr);
}

TEST(hook_runtime_crash_below_a_nested_chain) {
    hooks::Target* outer = hook("rt_counted", rt_counted, mappings::Convention::Cdecl, "int(int x)");
    hooks::Target* inner = hook("rt_inner", rt_inner, mappings::Convention::Cdecl, "void(void)");
    if (!outer || !inner) return;
    guard::set_fault_sink(nullptr);
    // A mod's handler on rt_counted calls rt_inner, whose (unguarded) handler
    // crashes: the mod's guard catches it, and the inner chain's handler
    // doesn't stay marked as running on this thread.
    tome_mod mod;
    hooks::Handler h = handler(TOME_HOOK_BEFORE, calls_inner);
    h.mod = &mod;
    hooks::add(outer, h);
    hooks::Handler own = handler(TOME_HOOK_BEFORE, unguarded_crash);
    own.gate = std::make_shared<hooks::Gate>();
    hooks::add(inner, own);
    const hooks::ThreadState before = hooks::thread_state();
    CHECK_EQ(as<CdeclOne>(rt_counted)(5), 6);
    CHECK(mod.faulted);
    CHECK_EQ(own.gate->busy.load(), 0);
    CHECK_EQ(hooks::running_on_this_thread(*own.gate), 0);
    const hooks::ThreadState after = hooks::thread_state();
    CHECK_EQ(after.running, before.running);
    CHECK_EQ(after.dispatches, before.dispatches);
    hooks::remove_gate(inner, own.gate.get());
    hooks::remove(outer, &mod, TOME_HOOK_BEFORE, calls_inner, nullptr);
}

TEST(hook_runtime_import_slot) {
    auto call = [] { return reinterpret_cast<StdcallImported>(g_rt_import_slot)(21); };
    CHECK_EQ(call(), 42);
    std::string error;
    hooks::Target* t = hooks::install_import("rt_import", reinterpret_cast<uintptr_t>(&g_rt_import_slot),
                                             mappings::Convention::Stdcall, "int(int a)", error);
    CHECK(t != nullptr);
    if (!t) return;
    CHECK(g_rt_import_slot != reinterpret_cast<uintptr_t>(&rt_imported));  // points at the stub now
    CHECK_EQ(call(), 42);  // empty chain: the original runs
    CHECK_EQ(rt_esp_delta(reinterpret_cast<const void*>(g_rt_import_slot), 1, 0, 0), 0u);  // still stdcall

    hooks::add(t, handler(TOME_HOOK_BEFORE, add_100_to_last_arg));
    CHECK_EQ(call(), 242);
    hooks::add(t, handler(TOME_HOOK_BEFORE, skip_with_42, -1));
    CHECK_EQ(call(), 42);
    hooks::remove(t, nullptr, TOME_HOOK_BEFORE, skip_with_42, nullptr);
    hooks::remove(t, nullptr, TOME_HOOK_BEFORE, add_100_to_last_arg, nullptr);
    CHECK_EQ(call(), 42);

    // Hooked once: the same name gives the same target, another name for the slot fails.
    CHECK(hooks::install_import("rt_import", 0, mappings::Convention::Stdcall, "int(int a)", error) == t);
    CHECK(hooks::install_import("rt_import_again", reinterpret_cast<uintptr_t>(&g_rt_import_slot),
                                mappings::Convention::Stdcall, "int(int a)", error) == nullptr);
    CHECK(error.find("already hooked") != std::string::npos);
}
