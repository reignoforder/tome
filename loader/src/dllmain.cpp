// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>

#include "bootstrap.h"
#include "restart.h"

// MinGW's CRT calls _fpreset (fninit) from a TLS callback in every thread
// started after this DLL loads, which would switch the game's new threads to
// 64-bit x87 precision; Windows starts them at 53-bit, as they'd run without
// Tome. Defining it here keeps the CRT's version out, so new threads keep the
// state Windows gave them.
extern "C" void _fpreset(void) {}

// tome.mix is loaded by Miles Sound System as if it were an audio provider.
// Pin the module so Miles can never unload it, then hand off to the bootstrap thread.
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        // rundll32 runs TomeRestartW only (restart.h).
        if (tome::restart::is_helper_process()) return TRUE;

        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&DllMain), &pinned);

        tome::start_bootstrap(instance);
    }
    return TRUE;
}
