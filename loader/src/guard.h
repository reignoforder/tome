#pragma once

// Crash isolation for calls into mods (docs/research.md §5.7).
//
// GCC has no __try/__except on 32-bit Windows, so Tome registers an SEH
// frame of its own around each call (assembly in guard.cpp). If the mod
// faults, the frame's handler lets the handlers of the frames above it
// unwind (RtlUnwind, as MSVC's __except does), then resumes right after the
// call with the stack, registers and FPU state of before it. The call is
// abandoned: whatever the mod was doing is left half done, and the mod is
// marked as faulted, so Tome stops calling it and the mod manager disables
// it (see ModManager).
//
// Only faults (access violations, illegal instructions, divisions by zero,
// stack overflows, breakpoints, uncaught C++ exceptions, ...) that nothing
// inside the call handled reach the guard. Threads a mod starts itself
// aren't guarded.

#include "mod_handle.h"

#include <type_traits>

namespace tome::guard {

// Called on the faulting thread after a guarded call crashed and was
// abandoned, once the crash is logged and recorded in the mod.
using FaultSink = void (*)(tome_mod* mod);
void set_fault_sink(FaultSink sink);

enum Flags : unsigned {
    kNone = 0,
    kEvenIfFaulted = 1,  // call even if the mod crashed before (disable, unload)
};

// Calls fn(context) on this thread under a guard. `what` and `detail` say
// what the call was for the log ("event handler", "tick"). True if it
// returned normally; false if it crashed, or if the mod had crashed before
// (then fn isn't called, unless kEvenIfFaulted).
bool call(tome_mod* mod, const char* what, const char* detail, void (*fn)(void*), void* context,
          unsigned flags = kNone);

template <typename F>
bool run(tome_mod* mod, const char* what, const char* detail, F&& f, unsigned flags = kNone) {
    using Fn = std::remove_reference_t<F>;
    return call(mod, what, detail, [](void* c) { (*static_cast<Fn*>(c))(); }, &f, flags);
}

} // namespace tome::guard
