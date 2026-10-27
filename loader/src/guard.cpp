#include "guard.h"

#include "core/hooks.h"
#include "log.h"
#include "paths.h"

#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <string>

namespace {

constexpr size_t kStackWords = 64;  // copied from the faulting stack, for the log

// An SEH registration record (next, handler) followed by what
// tome_guard_enter needs to resume, then what the handler saw.
struct GuardFrame {
    GuardFrame* next;
    void* handler;
    uint32_t esp;     // after tome_guard_enter's pushes
    uint32_t ebp;
    uint32_t resume;  // tome_guard_enter's resume label
    uint16_t fpu_control;
    uint16_t unused;
    uint32_t mxcsr;
    // Filled in by the handler.
    uint32_t code;
    uint32_t address;
    uint32_t info[2];
    uint32_t info_count;
    uint32_t stack[kStackWords];
    uint32_t stack_count;
};
static_assert(offsetof(GuardFrame, esp) == 8 && offsetof(GuardFrame, resume) == 16 &&
              offsetof(GuardFrame, fpu_control) == 20 && offsetof(GuardFrame, mxcsr) == 24,
              "tome_guard_enter depends on this layout");

// ExceptionFlags while handlers are called to unwind, not to handle.
constexpr DWORD kUnwinding = 0x2 | 0x4;  // EXCEPTION_UNWINDING, EXCEPTION_EXIT_UNWIND
constexpr DWORD kCppException = 0xE06D7363;  // MSVC's throw

} // namespace

extern "C" {
// Registers `frame` as the innermost SEH frame and calls fn(context). Returns
// 0 when fn returns, 1 when tome_guard_resume came back to it after a fault.
int tome_guard_enter(GuardFrame* frame, void (*fn)(void*), void* context);
// Lets the frames above `frame` unwind, then continues in tome_guard_enter
// with its stack, as it was before the call. Doesn't return.
[[noreturn]] void tome_guard_resume(GuardFrame* frame, EXCEPTION_RECORD* record);
EXCEPTION_DISPOSITION __cdecl tome_guard_handler(EXCEPTION_RECORD* record, void* frame, CONTEXT* context,
                                                  void* dispatcher);
}

// tome_guard_enter saves ebx/esi/edi (the resume path skips whatever the
// callee did with them), the FPU control word and MXCSR, and links the frame
// into fs:[0]. After a fault, the x87 stack may hold the mod's half-done
// values: fninit empties it before the control words are restored.
//
// tome_guard_resume: RtlUnwind calls every handler above the frame with
// the unwinding flag (MSVC's __finally blocks and C++ destructors in the
// game's frames) and leaves fs:[0] at the frame; then the saved esp and ebp
// come back and execution continues at the resume label.
asm(R"(
    .intel_syntax noprefix
    .text
    .globl _tome_guard_enter
_tome_guard_enter:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi
    mov eax, [ebp + 8]
    mov [eax + 8], esp
    mov [eax + 12], ebp
    mov dword ptr [eax + 16], offset .Ltome_guard_resumed
    fnstcw word ptr [eax + 20]
    stmxcsr dword ptr [eax + 24]
    mov ecx, dword ptr fs:[0]
    mov [eax], ecx
    mov dword ptr [eax + 4], offset _tome_guard_handler
    mov dword ptr fs:[0], eax
    push dword ptr [ebp + 16]
    call dword ptr [ebp + 12]
    mov eax, [ebp + 8]
    mov ecx, [eax]
    mov dword ptr fs:[0], ecx
    xor eax, eax
    jmp .Ltome_guard_done
.Ltome_guard_resumed:
    mov ecx, [eax]
    mov dword ptr fs:[0], ecx
    cld
    fninit
    fldcw word ptr [eax + 20]
    ldmxcsr dword ptr [eax + 24]
    mov eax, 1
.Ltome_guard_done:
    lea esp, [ebp - 12]
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

    .globl _tome_guard_resume
_tome_guard_resume:
    mov eax, [esp + 4]
    mov ecx, [esp + 8]
    push 0
    push ecx
    push 0
    push eax
    call _RtlUnwind@16
    mov eax, [esp + 4]
    mov esp, [eax + 8]
    mov ebp, [eax + 12]
    jmp dword ptr [eax + 16]
    .att_syntax prefix
)");

namespace {

bool catchable(DWORD code) {
    // Errors (severity bits 11, which includes C++ exceptions), plus the
    // warnings that would otherwise end the process.
    return (code & 0xC0000000) == 0xC0000000 || code == EXCEPTION_BREAKPOINT ||
           code == EXCEPTION_DATATYPE_MISALIGNMENT;
}

} // namespace

extern "C" EXCEPTION_DISPOSITION __cdecl tome_guard_handler(EXCEPTION_RECORD* record, void* establisher,
                                                             CONTEXT* context, void*) {
    if ((record->ExceptionFlags & kUnwinding) || !catchable(record->ExceptionCode)) return ExceptionContinueSearch;
    auto* frame = static_cast<GuardFrame*>(establisher);
    frame->code = record->ExceptionCode;
    frame->address = reinterpret_cast<uintptr_t>(record->ExceptionAddress);
    frame->info_count = record->NumberParameters < 2 ? record->NumberParameters : 2;
    for (uint32_t i = 0; i < frame->info_count; ++i) frame->info[i] = record->ExceptionInformation[i];
    // The stack is still there below the frame; after the resume it's reused.
    frame->stack_count = 0;
    const uintptr_t esp = context->Esp, top = reinterpret_cast<uintptr_t>(frame);
    if (esp && esp < top && !(esp & 3)) {
        auto words = reinterpret_cast<const uint32_t*>(esp);
        while (frame->stack_count < kStackWords && esp + 4 * (frame->stack_count + 1) <= top) {
            frame->stack[frame->stack_count] = words[frame->stack_count];
            ++frame->stack_count;
        }
    }
    tome_guard_resume(frame, record);
}

namespace tome::guard {
namespace {

FaultSink g_sink = nullptr;

const char* exception_name(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer division by zero";
    case EXCEPTION_INT_OVERFLOW: return "integer overflow";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_BREAKPOINT: return "breakpoint";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned data";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "floating-point division by zero";
    case EXCEPTION_FLT_INVALID_OPERATION: return "invalid floating-point operation";
    case EXCEPTION_FLT_OVERFLOW: return "floating-point overflow";
    case EXCEPTION_FLT_UNDERFLOW: return "floating-point underflow";
    case EXCEPTION_FLT_STACK_CHECK: return "floating-point stack check";
    case kCppException: return "uncaught C++ exception";
    default: return nullptr;
    }
}

// "hello.3.dll+0x1a2b", or the bare address outside any module.
std::string where(uintptr_t address) {
    char buf[300];
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module) &&
        GetModuleFileNameW(module, path, MAX_PATH)) {
        std::wstring name = path;
        name = name.substr(name.find_last_of(L"\\/") + 1);
        snprintf(buf, sizeof(buf), "%s+0x%lx", narrow(name).c_str(),
                 static_cast<unsigned long>(address - reinterpret_cast<uintptr_t>(module)));
    } else {
        snprintf(buf, sizeof(buf), "0x%08lx", static_cast<unsigned long>(address));
    }
    return buf;
}

// Whether `value` looks like a return address: inside a module, right
// after a call instruction (E8 rel32, FF /2 with a 2-, 3- or 6-byte form).
bool return_address(uint32_t value) {
    HMODULE module = nullptr;
    if (value < 0x10000 || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                               reinterpret_cast<LPCWSTR>(static_cast<uintptr_t>(value)), &module)) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi;
    const uintptr_t start = value - 6;
    if (!VirtualQuery(reinterpret_cast<const void*>(start), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
        start + 6 > reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
        return false;
    }
    auto code = reinterpret_cast<const uint8_t*>(start);
    return code[1] == 0xE8 || (code[4] == 0xFF && (code[5] & 0x38) == 0x10) ||
           (code[3] == 0xFF && (code[4] & 0x38) == 0x10) || (code[0] == 0xFF && (code[1] & 0x38) == 0x10);
}

// After a stack overflow the guard page is gone (it was committed to run the
// handler); without it the next overflow on this thread would end the
// process. Puts it back at the bottom of the committed stack, like the CRT's
// _resetstkoflw.
void rearm_stack_guard() {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(&mbi, &mbi, sizeof(mbi))) return;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    auto here = reinterpret_cast<uint8_t*>(&mbi);
    for (auto p = static_cast<uint8_t*>(mbi.AllocationBase); p <= static_cast<uint8_t*>(mbi.BaseAddress);) {
        MEMORY_BASIC_INFORMATION region;
        if (!VirtualQuery(p, &region, sizeof(region))) return;
        if (region.State == MEM_COMMIT) {
            // The lowest committed page, unless it's already the guard or too close to this frame.
            if ((region.Protect & PAGE_GUARD) || p + 2 * si.dwPageSize > here) return;
            DWORD old;
            if (!VirtualProtect(p, si.dwPageSize, PAGE_READWRITE | PAGE_GUARD, &old)) {
                TOME_WARN("Couldn't restore the stack guard page; another stack overflow will end the game");
            }
            return;
        }
        p = static_cast<uint8_t*>(region.BaseAddress) + region.RegionSize;
    }
}

void report(tome_mod* mod, const char* what, const char* detail, const GuardFrame& frame) {
    char buf[200];
    const char* name = exception_name(frame.code);
    if (frame.code == EXCEPTION_ACCESS_VIOLATION && frame.info_count == 2) {
        const char* access = frame.info[0] == 1 ? "writing" : frame.info[0] == 8 ? "executing" : "reading";
        snprintf(buf, sizeof(buf), "access violation %s 0x%08lx", access, static_cast<unsigned long>(frame.info[1]));
    } else if (name) {
        snprintf(buf, sizeof(buf), "%s", name);
    } else {
        snprintf(buf, sizeof(buf), "exception 0x%08lx", static_cast<unsigned long>(frame.code));
    }
    std::string text = std::string("crashed in ") + what + (detail && *detail ? " " : "") + (detail ? detail : "") +
                       ": " + buf + " at " + where(frame.address);
    const char* id = mod ? mod->id.c_str() : "?";
    TOME_ERROR("%s %s; Tome stopped the call and disables the mod", id, text.c_str());

    std::string calls;
    int shown = 0;
    for (uint32_t i = 0; i < frame.stack_count && shown < 8; ++i) {
        if (!return_address(frame.stack[i])) continue;
        calls += (calls.empty() ? "" : " <- ") + where(frame.stack[i]);
        ++shown;
    }
    if (!calls.empty()) TOME_ERROR("  called from (likely): %s", calls.c_str());

    if (mod) mod->set_fault(text, calls);
    if (mod && g_sink) g_sink(mod);
}

} // namespace

void set_fault_sink(FaultSink sink) {
    g_sink = sink;
}

bool call(tome_mod* mod, const char* what, const char* detail, void (*fn)(void*), void* context, unsigned flags) {
    if (mod && mod->faulted && !(flags & kEvenIfFaulted)) return false;
    GuardFrame frame;
    const hooks::ThreadState state = hooks::thread_state();
    if (!tome_guard_enter(&frame, fn, context)) return true;
    // Hook calls the crash skipped over still count as running on this thread.
    hooks::restore_thread_state(state);
    if (frame.code == EXCEPTION_STACK_OVERFLOW) rearm_stack_guard();
    report(mod, what, detail, frame);
    return false;
}

} // namespace tome::guard
