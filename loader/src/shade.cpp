// SPDX-License-Identifier: GPL-3.0-or-later
#include "shade.h"

#include <windows.h>

namespace tome::shade {

namespace {

struct Natives {
    bool active = false;
    uint32_t(__stdcall* push_jump)(uint32_t target, uint32_t dest, uint32_t value) = nullptr;
    uint32_t(__stdcall* make_stub)(uint32_t dest, uint32_t value) = nullptr;
    uint32_t(__stdcall* hook_call)(uint32_t site, uint32_t dest) = nullptr;
};

template <typename Fn>
bool find(HMODULE m, const char* name, Fn& fn) {
    fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, name)));
    return fn != nullptr;
}

const Natives& natives() {
    static const Natives n = [] {
        Natives r;
        HMODULE m = GetModuleHandleW(L"shade.dll");
        if (!m) return r;
        uint32_t(__stdcall * version)() = nullptr;
        r.active = find(m, "ShadeVersion", version) && version() >= 1 && find(m, "ShadeHookPushJump", r.push_jump) &&
                   find(m, "ShadeMakeStub", r.make_stub) && find(m, "ShadeHookCall", r.hook_call);
        return r;
    }();
    return n;
}

} // namespace

bool active() { return natives().active; }

uintptr_t hook_function(uintptr_t target, uintptr_t dest, uintptr_t value) {
    if (!active()) return 0;
    return natives().push_jump(static_cast<uint32_t>(target), static_cast<uint32_t>(dest),
                               static_cast<uint32_t>(value));
}

uintptr_t make_stub(uintptr_t dest, uintptr_t value) {
    if (!active()) return 0;
    return natives().make_stub(static_cast<uint32_t>(dest), static_cast<uint32_t>(value));
}

bool hook_call(uintptr_t site, uintptr_t dest) {
    if (!active()) return false;
    return natives().hook_call(static_cast<uint32_t>(site), static_cast<uint32_t>(dest)) != 0;
}

} // namespace tome::shade
