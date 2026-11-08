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
    uint32_t(__stdcall* unhook)(uint32_t target) = nullptr;
    uint32_t(__stdcall* original)(uint32_t target) = nullptr;
    uint32_t(__stdcall* mod_code_hash)(const char* dll, char* out) = nullptr;
    uint32_t version = 0;  // ShadeVersion's answer
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
        if (find(m, "ShadeVersion", version)) r.version = version();
        r.active = r.version >= 1 && find(m, "ShadeHookPushJump", r.push_jump) &&
                   find(m, "ShadeMakeStub", r.make_stub) && find(m, "ShadeHookCall", r.hook_call);
        // Only for a hook that failed half way (unhook): without it Tome is
        // still under Shade, and must not take its Windows paths there.
        find(m, "ShadeUnhook", r.unhook);
        // Version 2: without it, Tome is under Shade but can't hook functions.
        find(m, "ShadeOriginal", r.original);
        // Version 4: without it, mods' DLLs don't load (approval needs it).
        find(m, "ShadeModCodeHash", r.mod_code_hash);
        return r;
    }();
    return n;
}

} // namespace

bool active() { return natives().active; }

bool can_hook_functions() { return active() && natives().original; }

bool can_load_dlls() { return active() && natives().version >= 4 && natives().mod_code_hash; }

std::string mod_code_hash(const std::wstring& dll) {
    if (!can_load_dlls()) return {};
    // Shade's paths are UTF-8, as its LoadLibraryW gets them.
    std::string path(WideCharToMultiByte(CP_UTF8, 0, dll.c_str(), -1, nullptr, 0, nullptr, nullptr), '\0');
    if (path.empty() || !WideCharToMultiByte(CP_UTF8, 0, dll.c_str(), -1, path.data(), int(path.size()), nullptr,
                                             nullptr))
        return {};
    char out[65] = {};
    if (!natives().mod_code_hash(path.c_str(), out)) return {};
    out[64] = 0;
    std::string hash = out;
    for (char c : hash) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return {};
    }
    return hash.size() == 64 ? hash : std::string();
}

const char* load_error_hint(unsigned long error) {
    if (!active()) return "";
    if (error == ERROR_MOD_NOT_FOUND)
        return " (under Shade, a DLL's code must be built first, with shade-recomp build-mod: Shade's log says how)";
    if (error == ERROR_DLL_INIT_FAILED)
        return " (under Shade, its built code may be for another Shade, or its address taken by another mod's: "
               "Shade's log says which)";
    return "";
}

uintptr_t original(uintptr_t target) {
    if (!can_hook_functions()) return 0;
    return natives().original(static_cast<uint32_t>(target));
}

bool hook_function(uintptr_t target, uintptr_t dest, uintptr_t value) {
    if (!active()) return false;
    return natives().push_jump(static_cast<uint32_t>(target), static_cast<uint32_t>(dest),
                               static_cast<uint32_t>(value)) != 0;
}

uintptr_t make_stub(uintptr_t dest, uintptr_t value) {
    if (!active()) return 0;
    return natives().make_stub(static_cast<uint32_t>(dest), static_cast<uint32_t>(value));
}

bool hook_call(uintptr_t site, uintptr_t dest) {
    if (!active()) return false;
    return natives().hook_call(static_cast<uint32_t>(site), static_cast<uint32_t>(dest)) != 0;
}

bool unhook(uintptr_t target) {
    if (!active() || !natives().unhook) return false;
    return natives().unhook(static_cast<uint32_t>(target)) != 0;
}

} // namespace tome::shade
