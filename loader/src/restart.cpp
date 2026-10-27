#include "restart.h"

#include "frames.h"
#include "log.h"
#include "main_thread.h"
#include "paths.h"

#include <cwchar>
#include <string>

namespace tome::restart {
namespace {

constexpr DWORD kWaitMs = 60000;
// A close the game didn't act on stops counting as a restart after this
// long, so a later exit doesn't restart. The check runs on the main thread,
// from the game's message loop: once the game has left its main loop to shut
// down, it never runs, however long the shutdown takes.
constexpr unsigned kCloseTimeoutMs = 60000;

bool g_pending = false;  // main thread
unsigned g_sequence = 0;

std::wstring module_path(HMODULE module) {
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(module, path, MAX_PATH);
    return len == 0 || len >= MAX_PATH ? std::wstring() : std::wstring(path, len);
}

HMODULE self_module() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&self_module), &self);
    return self;
}

} // namespace

bool request(const char* who) {
    HWND window = main_thread::window();
    if (!window) {
        TOME_WARN("Restart asked for by %s, but there's no game window to close", who);
        return false;
    }
    g_pending = true;
    unsigned sequence = ++g_sequence;
    // In a game, closing the window asks Confirm Exit first. The player has
    // said yes already, so Tome presses that dialog's Exit button instead.
    void* exit = frames::game_showing() ? frames::get("ConfirmQuitQuitButton") : nullptr;
    if (exit && frames::click(exit)) {
        TOME_INFO("Restart asked for by %s: leaving the game", who);
    } else {
        TOME_INFO("Restart asked for by %s: closing the game", who);
        PostMessageW(window, WM_CLOSE, 0, 0);
    }
    main_thread::post_delayed(kCloseTimeoutMs, [sequence] {
        if (!g_pending || sequence != g_sequence) return;
        g_pending = false;
        TOME_WARN("The game didn't close for the restart; not restarting");
    });
    return true;
}

void on_shutdown() {
    if (!g_pending) return;
    g_pending = false;
    std::wstring exe = module_path(nullptr);
    std::wstring mix = module_path(self_module());
    wchar_t system[MAX_PATH];
    UINT system_len = GetSystemDirectoryW(system, MAX_PATH);
    if (exe.empty() || mix.empty() || system_len == 0 || system_len >= MAX_PATH) {
        TOME_ERROR("Restart: can't find the game's executable or tome.mix (error %lu)", GetLastError());
        return;
    }
    std::wstring rundll32 = std::wstring(system) + L"\\rundll32.exe";
    std::wstring command = L"\"" + rundll32 + L"\" \"" + mix + L"\",TomeRestart " +
                           std::to_wstring(GetCurrentProcessId()) + L"|" + exe + L"|" + GetCommandLineW();
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(rundll32.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        TOME_ERROR("Restart: couldn't start %s (error %lu)", narrow(rundll32).c_str(), GetLastError());
        return;
    }
    TOME_INFO("Restart: helper started (pid %lu); it starts %s once this process has exited", pi.dwProcessId,
              narrow(exe).c_str());
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

bool is_helper_process() {
    std::wstring exe = module_path(nullptr);
    size_t slash = exe.find_last_of(L"\\/");
    std::wstring name = exe.substr(slash == std::wstring::npos ? 0 : slash + 1);
    return _wcsicmp(name.c_str(), L"rundll32.exe") == 0;
}

} // namespace tome::restart

extern "C" void CALLBACK TomeRestartW(HWND, HINSTANCE, LPWSTR args, int) {
    if (!args) return;
    std::wstring text = args;
    size_t first = text.find(L'|');
    size_t second = first == std::wstring::npos ? first : text.find(L'|', first + 1);
    if (second == std::wstring::npos) return;
    DWORD pid = static_cast<DWORD>(std::wcstoul(text.c_str(), nullptr, 10));
    std::wstring exe = text.substr(first + 1, second - first - 1);
    std::wstring command_line = text.substr(second + 1);
    // OpenProcess fails once it's gone; then there's nothing to wait for.
    if (HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
        WaitForSingleObject(old, tome::restart::kWaitMs);
        CloseHandle(old);
    }
    // The game's folder, which war3.exe expects as its working directory.
    std::wstring dir = exe.substr(0, exe.find_last_of(L"\\/"));
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(exe.c_str(), command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si,
                       &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}
