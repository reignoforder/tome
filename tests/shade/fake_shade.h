// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Loads the fake shade.dll (fake_shade.cpp, its path in TOME_TEST_FAKE_SHADE)
// into this process: from then on Tome runs as under Shade. Load it before
// the first shade::active() call, which looks once.

#include <windows.h>

#include <cstdint>

namespace fake_shade {

struct Api {
    void (*fail)(const char* native) = nullptr;  // that native fails once
    void (*during_hook)(void (*fn)()) = nullptr;  // the next ShadeHookPushJump runs fn on another thread
    int (*hooked)(uint32_t target) = nullptr;    // 1 hooked, 0 unhooked, -1 never
    uint32_t (*call_target)(uint32_t site) = nullptr;
};

template <typename Fn>
inline void find(HMODULE m, const char* name, Fn& fn) {
    fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, name)));
}

// Null if it isn't there.
inline const Api* load() {
    static Api api;
    static bool loaded = [] {
        HMODULE m = LoadLibraryA(TOME_TEST_FAKE_SHADE);
        if (!m) return false;
        find(m, "FakeShadeFail", api.fail);
        find(m, "FakeShadeDuringHook", api.during_hook);
        find(m, "FakeShadeHooked", api.hooked);
        find(m, "FakeShadeCallTarget", api.call_target);
        return api.fail && api.during_hook && api.hooked && api.call_target;
    }();
    return loaded ? &api : nullptr;
}

} // namespace fake_shade
