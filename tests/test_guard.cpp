// SPDX-License-Identifier: GPL-3.0-or-later
// The crash guard (loader/src/guard.cpp) on real faults in this process.

#include "guard.h"
#include "test.h"
#include "unwind.h"

#include <windows.h>

#include <memory>

using namespace tome;

extern "C" {
// Sets ebx, esi, edi and ebp to garbage, then writes to address 0.
void rt_clobber_and_fault(void*);
// Executes ud2.
void rt_illegal(void*);
// Pushes three values on the x87 stack, then writes to address 0.
void rt_fpu_fault(void*);
}

asm(R"(
    .intel_syntax noprefix
    .text
    .globl _rt_clobber_and_fault
_rt_clobber_and_fault:
    mov ebx, 0x11111111
    mov esi, 0x22222222
    mov edi, 0x33333333
    mov ebp, 0x44444444
    xor eax, eax
    mov dword ptr [eax], 1
    ret

    .globl _rt_illegal
_rt_illegal:
    ud2
    ret

    .globl _rt_fpu_fault
_rt_fpu_fault:
    fld1
    fldz
    fld1
    xor eax, eax
    mov dword ptr [eax], 1
    ret
    .att_syntax prefix
)");

namespace {

int g_sink_calls = 0;
tome_mod* g_sink_mod = nullptr;

void sink(tome_mod* mod) {
    ++g_sink_calls;
    g_sink_mod = mod;
}

struct Setup {
    Setup() {
        guard::set_fault_sink(sink);
        g_sink_calls = 0;
        g_sink_mod = nullptr;
    }
};

bool contains(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

void* fs0() {
    void* value;
    asm volatile("mov %%fs:0, %0" : "=r"(value));
    return value;
}

} // namespace

TEST(guard_normal_call) {
    Setup setup;
    tome_mod mod;
    mod.id = "ok";
    int calls = 0;
    void* before = fs0();
    CHECK(guard::run(&mod, "test", nullptr, [&] { ++calls; }));
    CHECK_EQ(calls, 1);
    CHECK(!mod.faulted);
    CHECK_EQ(g_sink_calls, 0);
    CHECK(fs0() == before);
}

TEST(guard_access_violation) {
    Setup setup;
    tome_mod mod;
    mod.id = "crashy";
    void* before = fs0();
    CHECK(!guard::run(&mod, "event handler", "(tick)", [] { *static_cast<volatile int*>(nullptr) = 1; }));
    CHECK(fs0() == before);
    CHECK(mod.faulted);
    CHECK_EQ(g_sink_calls, 1);
    CHECK(g_sink_mod == &mod);
    std::string text = mod.fault_text();
    CHECK(contains(text, "crashed in event handler (tick): access violation writing 0x00000000 at "));
    CHECK(contains(text, ".exe+0x"));

    // Skipped from now on, unless the call is meant for a crashed mod.
    int calls = 0;
    CHECK(!guard::run(&mod, "test", nullptr, [&] { ++calls; }));
    CHECK_EQ(calls, 0);
    CHECK(guard::run(&mod, "test", nullptr, [&] { ++calls; }, guard::kEvenIfFaulted));
    CHECK_EQ(calls, 1);

    // A second crash is logged, but the first one stays the mod's record.
    CHECK(!guard::run(&mod, "native", "X", [] { (void)*static_cast<volatile int*>(nullptr); },
                      guard::kEvenIfFaulted));
    CHECK_EQ(g_sink_calls, 2);
    CHECK(contains(mod.fault_text(), "event handler"));

    mod.clear_fault();
    CHECK(!mod.faulted);
    CHECK(mod.fault_text().empty());
}

TEST(guard_other_faults) {
    Setup setup;
    tome_mod a, b, c, d;
    CHECK(!guard::call(&a, "test", nullptr, rt_illegal, nullptr));
    CHECK(contains(a.fault_text(), "illegal instruction"));

    volatile int zero = 0;
    CHECK(!guard::run(&b, "test", nullptr, [&] { volatile int x = 10 / zero; (void)x; }));
    CHECK(contains(b.fault_text(), "integer division by zero"));

    CHECK(!guard::run(&c, "test", nullptr, [] { asm volatile("int3"); }));
    CHECK(contains(c.fault_text(), "breakpoint"));

    // What MSVC's throw raises, when nothing catches it.
    CHECK(!guard::run(&d, "test", nullptr, [] { RaiseException(0xE06D7363, EXCEPTION_NONCONTINUABLE, 0, nullptr); }));
    CHECK(contains(d.fault_text(), "uncaught C++ exception"));
    CHECK_EQ(g_sink_calls, 4);
}

TEST(guard_restores_registers_and_fpu) {
    Setup setup;
    tome_mod mod;
    // Values the compiler keeps in callee-saved registers across the call.
    volatile int seed = 7;
    int a = seed * 3, b = seed * 5, c = seed * 11, d = seed * 13;
    CHECK(!guard::call(&mod, "test", nullptr, rt_clobber_and_fault, nullptr));
    CHECK_EQ(a + b + c + d, 7 * 32);
    CHECK_EQ(a * b, 21 * 35);

    // A non-default x87 control word (single precision) and MXCSR survive,
    // and the values the mod left on the x87 stack are gone.
    uint16_t cw_saved, cw = 0x007F, cw_after;
    uint32_t mxcsr_saved, mxcsr = 0x1F80 | 0x6000, mxcsr_after;  // round toward zero
    asm volatile("fnstcw %0" : "=m"(cw_saved));
    asm volatile("stmxcsr %0" : "=m"(mxcsr_saved));
    asm volatile("fldcw %0" ::"m"(cw));
    asm volatile("ldmxcsr %0" ::"m"(mxcsr));
    tome_mod fpu;
    CHECK(!guard::call(&fpu, "test", nullptr, rt_fpu_fault, nullptr));
    asm volatile("fnstcw %0" : "=m"(cw_after));
    asm volatile("stmxcsr %0" : "=m"(mxcsr_after));
    struct {
        uint32_t control, status, tag, rest[4];
    } env;
    asm volatile("fnstenv %0" : "=m"(env));
    asm volatile("fldcw %0" ::"m"(cw_saved));
    asm volatile("ldmxcsr %0" ::"m"(mxcsr_saved));
    CHECK_EQ(cw_after, cw);
    CHECK_EQ(mxcsr_after & 0xFFC0, mxcsr & 0xFFC0);  // the low bits are sticky flags
    CHECK_EQ(env.tag & 0xFFFF, 0xFFFFu);  // every x87 register empty
    // Floating-point math still works afterwards.
    volatile double x = 1.5;
    CHECK(x * 4 == 6.0);
}

namespace {

struct Registration {
    Registration* next;
    void* handler;
};

int g_unwinds = 0;
int g_handled = 0;

extern "C" EXCEPTION_DISPOSITION __cdecl count_unwind(EXCEPTION_RECORD* record, void*, CONTEXT*, void*) {
    if (record->ExceptionFlags & 0x2) {
        ++g_unwinds;
    } else {
        ++g_handled;
    }
    return ExceptionContinueSearch;
}

// Faults with an SEH frame of its own that doesn't handle the fault: it's
// asked first, then unwound.
void fault_inside_a_frame(void*) {
    Registration reg;
    reg.handler = reinterpret_cast<void*>(&count_unwind);
    asm volatile("mov %%fs:0, %0" : "=r"(reg.next));
    asm volatile("mov %0, %%fs:0" ::"r"(&reg) : "memory");
    *static_cast<volatile int*>(nullptr) = 1;
    asm volatile("mov %0, %%fs:0" ::"r"(reg.next) : "memory");
}

} // namespace

TEST(guard_unwinds_inner_frames) {
    Setup setup;
    tome_mod mod;
    g_unwinds = g_handled = 0;
    void* before = fs0();
    CHECK(!guard::call(&mod, "test", nullptr, fault_inside_a_frame, nullptr));
    CHECK(fs0() == before);
    CHECK_EQ(g_handled, 1);
    CHECK_EQ(g_unwinds, 1);
}

TEST(guard_nested) {
    Setup setup;
    tome_mod outer, inner;
    bool inner_ok = true;
    int after = 0;
    CHECK(guard::run(&outer, "outer", nullptr, [&] {
        inner_ok = guard::run(&inner, "inner", nullptr, [] { *static_cast<volatile int*>(nullptr) = 1; });
        ++after;
    }));
    CHECK(!inner_ok);
    CHECK_EQ(after, 1);
    CHECK(inner.faulted);
    CHECK(!outer.faulted);

    // A crash inside an unguarded call made by a guarded one: the outer guard catches it.
    tome_mod caller;
    CHECK(!guard::run(&caller, "outer", nullptr, [&] { rt_clobber_and_fault(nullptr); }));
    CHECK(caller.faulted);
}

namespace {

volatile bool g_forever = true;

int recurse(int n) {
    volatile char pad[512];
    pad[0] = static_cast<char>(n);
    return g_forever ? recurse(n + 1) + pad[0] : n;
}

bool g_overflows[2];

DWORD WINAPI overflow_twice(void*) {
    for (bool& caught : g_overflows) {
        tome_mod mod;
        caught = !guard::run(&mod, "test", nullptr, [] { recurse(0); }) &&
                 mod.fault_text().find("stack overflow") != std::string::npos;
    }
    return 0;
}

} // namespace

TEST(guard_stack_overflow) {
    Setup setup;
    g_overflows[0] = g_overflows[1] = false;
    // A small stack keeps it quick. The second overflow needs the guard page back.
    HANDLE thread = CreateThread(nullptr, 256 * 1024, overflow_twice, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    CHECK(thread);
    if (!thread) return;
    CHECK(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);
    DWORD code = 0;
    GetExitCodeThread(thread, &code);
    CloseHandle(thread);
    CHECK_EQ(code, 0ul);
    CHECK(g_overflows[0]);
    CHECK(g_overflows[1]);
}

// --- cleanups on unwind (unwind.h) ------------------------------------------------

namespace {

void crash_now() {
    *static_cast<volatile int*>(nullptr) = 1;
}

// Code between the guard and the fault that holds a lock and owns memory.
void hold_then_crash(SRWLOCK& lock, std::shared_ptr<int>& owned) {
    ExclusiveLock hold(lock);
    std::shared_ptr<int> mine = owned;
    OnUnwind release([&] { mine.reset(); });
    crash_now();
}

} // namespace

TEST(unwind_cleanups_run_when_a_guard_abandons_the_call) {
    Setup setup;
    SRWLOCK lock = SRWLOCK_INIT;
    auto owned = std::make_shared<int>(1);
    tome_mod mod;
    void* before = fs0();
    CHECK(!guard::run(&mod, "test", nullptr, [&] { hold_then_crash(lock, owned); }));
    CHECK(fs0() == before);
    CHECK(TryAcquireSRWLockExclusive(&lock));  // released on the way out
    ReleaseSRWLockExclusive(&lock);
    CHECK_EQ(owned.use_count(), 1l);           // the copy was dropped

    // A lock released early isn't released again; shared locks too.
    tome_mod other;
    CHECK(!guard::run(&other, "test", nullptr, [&] {
        ExclusiveLock early(lock);
        early.unlock();
        SharedLock shared(lock);
        crash_now();
    }));
    CHECK(TryAcquireSRWLockExclusive(&lock));
    ReleaseSRWLockExclusive(&lock);
}

TEST(unwind_cleanups_stay_quiet_on_normal_returns) {
    int runs = 0;
    void* before = fs0();
    {
        OnUnwind cleanup([&] { ++runs; });
        CHECK(fs0() != before);
    }
    CHECK(fs0() == before);
    CHECK_EQ(runs, 0);
    SRWLOCK lock = SRWLOCK_INIT;
    {
        ExclusiveLock hold(lock);
        CHECK(!TryAcquireSRWLockShared(&lock));
    }
    CHECK(TryAcquireSRWLockExclusive(&lock));
    ReleaseSRWLockExclusive(&lock);
    CHECK(fs0() == before);
}
