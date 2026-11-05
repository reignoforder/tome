// SPDX-License-Identifier: GPL-3.0-or-later
// Runs every test in a process of its own: the loader keeps its state in
// globals (the mod manager, native slots, hooks), so a fresh process is the
// only clean slate. Without arguments this program starts itself once per
// test with `--run <name>`; with `<text>` it only runs tests whose name
// contains it.
#include "test.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr DWORD kTimeoutMs = 60000;  // a hang is a failure, not a wait

// A test that crashes ends its process with the exception code (and
// without starting a debugger); the parent reports it.
LONG WINAPI report_crash(EXCEPTION_POINTERS* info) {
    std::printf("  unhandled exception 0x%08lx at %p\n", info->ExceptionRecord->ExceptionCode,
                info->ExceptionRecord->ExceptionAddress);
    std::fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

int run_one(const char* name) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(report_crash);
    for (const test::Case& c : test::registry()) {
        if (std::strcmp(c.name, name) == 0) {
            c.fn();
            std::fflush(stdout);
            return test::failures() == 0 ? 0 : 1;
        }
    }
    std::printf("no test named %s\n", name);
    return 2;
}

// 0 passed, 1 failed, 2 crashed or hung.
int spawn(const char* name) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(self) + L"\" --run ";
    for (const char* c = name; *c; ++c) command += static_cast<wchar_t>(*c);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi = {};
    std::fflush(stdout);
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        std::printf("  couldn't start a process for %s (error %lu)\n", name, GetLastError());
        return 2;
    }
    CloseHandle(pi.hThread);
    DWORD exit_code = 2;
    if (WaitForSingleObject(pi.hProcess, kTimeoutMs) == WAIT_TIMEOUT) {
        std::printf("  %s still running after %lu s; stopped\n", name, kTimeoutMs / 1000);
        TerminateProcess(pi.hProcess, 3);
    } else {
        GetExitCodeProcess(pi.hProcess, &exit_code);
    }
    CloseHandle(pi.hProcess);
    if (exit_code > 1) std::printf("  %s crashed (exit code 0x%08lx)\n", name, exit_code);
    return exit_code == 0 ? 0 : exit_code == 1 ? 1 : 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--run") == 0) return run_one(argv[2]);

    const char* filter = argc == 2 ? argv[1] : nullptr;
    int ran = 0, failed = 0;
    for (const test::Case& c : test::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        int result = spawn(c.name);
        std::printf("%s %s\n", result == 0 ? "ok  " : "FAIL", c.name);
        ++ran;
        if (result != 0) ++failed;
    }
    std::printf("\n%d tests, %d failed\n", ran, failed);
    return failed == 0 ? 0 : 1;
}
