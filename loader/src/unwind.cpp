#include "unwind.h"

namespace {

constexpr DWORD kUnwinding = 0x2 | 0x4;  // EXCEPTION_UNWINDING, EXCEPTION_EXIT_UNWIND

// Only acts while unwinding; a fault being dispatched is someone else's to handle.
extern "C" EXCEPTION_DISPOSITION __cdecl tome_unwind_handler(EXCEPTION_RECORD* exception, void* frame, CONTEXT*,
                                                              void*) {
    if (exception->ExceptionFlags & kUnwinding) {
        auto* record = static_cast<tome::unwind::Record*>(frame);
        record->fn(record->context);
    }
    return ExceptionContinueSearch;
}

} // namespace

namespace tome::unwind {

void push(Record& r) {
    r.handler = reinterpret_cast<void*>(&tome_unwind_handler);
    Record* head;
    asm volatile("mov %%fs:0, %0" : "=r"(head));
    r.next = head;
    asm volatile("mov %0, %%fs:0" ::"r"(&r) : "memory");
}

void pop(Record& r) {
    asm volatile("mov %0, %%fs:0" ::"r"(r.next) : "memory");
}

} // namespace tome::unwind
