// SPDX-License-Identifier: GPL-3.0-or-later
#include "crash_handler.h"

#include "guard.h"
#include "log.h"
#include "unwind.h"

#include <dbghelp.h>

#include <algorithm>
#include <cstdio>

namespace tome::crash_handler {
namespace {

constexpr size_t kLogTailBytes = 64 * 1024;
constexpr size_t kLogTailLines = 80;
constexpr size_t kStackWords = 1024;  // scanned for return addresses
constexpr size_t kMaxCallers = 12;

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

std::wstring g_tome_dir;
std::wstring g_dir;  // tome/crashes
DWORD g_main_thread = 0;
std::string g_game = "unknown";
LPTOP_LEVEL_EXCEPTION_FILTER g_next = nullptr;   // the filter Tome's hands over to
LPTOP_LEVEL_EXCEPTION_FILTER g_first = nullptr;  // the one install found (the game's)
// Per thread: calls of Tome's filter handing a crash on that haven't returned.
DWORD g_depth_slot = TLS_OUT_OF_INDEXES;
MiniDumpWriteDumpFn g_write_dump = nullptr;
ReportSink g_sink = nullptr;

SRWLOCK g_mods_lock = SRWLOCK_INIT;
std::vector<crash_report::ModLine> g_mods;
std::vector<std::pair<uintptr_t, std::string>> g_modules;
bool g_mods_known = false;

// One report at a time: the thread writing it, 0 if none.
volatile LONG g_busy = 0;

// The writer thread and its one request.
HANDLE g_writer = nullptr;
DWORD g_writer_id = 0;
HANDLE g_request = nullptr, g_done = nullptr;
EXCEPTION_POINTERS* g_request_pointers = nullptr;
DWORD g_request_thread = 0;
std::string g_request_stem;

std::string narrow_name(const wchar_t* path) {
    std::wstring name = path;
    size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) name = name.substr(slash + 1);
    return narrow(name);
}

crash_report::Location locate(uintptr_t address, const std::vector<std::pair<uintptr_t, std::string>>& modules) {
    crash_report::Location at;
    at.address = static_cast<uint32_t>(address);
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module) &&
        GetModuleFileNameW(module, path, MAX_PATH)) {
        at.module = narrow_name(path);
        at.offset = static_cast<uint32_t>(address - reinterpret_cast<uintptr_t>(module));
        for (const auto& [base, id] : modules) {
            if (base == reinterpret_cast<uintptr_t>(module)) at.mod = id;
        }
    }
    return at;
}

// Likely return addresses on the crashed thread's stack, from esp up, as far
// as the stack's committed memory goes.
std::vector<crash_report::Location> callers(uint32_t esp,
                                            const std::vector<std::pair<uintptr_t, std::string>>& modules) {
    std::vector<crash_report::Location> out;
    MEMORY_BASIC_INFORMATION mbi;
    if (!esp || !VirtualQuery(reinterpret_cast<const void*>(static_cast<uintptr_t>(esp)), &mbi, sizeof(mbi)) ||
        mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY))) {
        return out;
    }
    uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    auto words = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(esp & ~3u));
    size_t count = std::min<size_t>(kStackWords, (end - reinterpret_cast<uintptr_t>(words)) / 4);
    for (size_t i = 0; i < count && out.size() < kMaxCallers; ++i) {
        if (guard::looks_like_return_address(words[i])) out.push_back(locate(words[i], modules));
    }
    return out;
}

std::string read_log_tail() {
    HANDLE file = CreateFileW((g_tome_dir + L"\\tome.log").c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return "";
    std::string text;
    LARGE_INTEGER size;
    if (GetFileSizeEx(file, &size)) {
        LONGLONG start = std::max<LONGLONG>(0, size.QuadPart - static_cast<LONGLONG>(kLogTailBytes));
        LARGE_INTEGER at;
        at.QuadPart = start;
        text.resize(static_cast<size_t>(size.QuadPart - start));
        DWORD read = 0;
        if (!SetFilePointerEx(file, at, nullptr, FILE_BEGIN) ||
            !ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr)) {
            read = 0;
        }
        text.resize(read);
        // Cut at a line start unless the whole file was read.
        if (start > 0) {
            size_t newline = text.find('\n');
            text.erase(0, newline == std::string::npos ? text.size() : newline + 1);
        }
    }
    CloseHandle(file);
    std::string tail = crash_report::tail(text, kLogTailLines);
    std::erase(tail, '\r');
    return tail;
}

void remove_old_reports() {
    std::vector<std::string> names;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW((g_dir + L"\\*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.push_back(narrow(data.cFileName));
    } while (FindNextFileW(find, &data));
    FindClose(find);
    // Room for the one being written.
    for (const std::string& name : crash_report::to_remove(names, crash_report::kKeepReports - 1)) {
        DeleteFileW((g_dir + L"\\" + widen(name)).c_str());
    }
}

bool write_text(const std::wstring& path, const std::string& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
              written == text.size();
    CloseHandle(file);
    return ok;
}

std::string write_now(EXCEPTION_POINTERS* pointers, DWORD thread) {
    if (g_dir.empty() || !pointers || !pointers->ExceptionRecord || !pointers->ContextRecord) return "";
    CreateDirectoryW(g_dir.c_str(), nullptr);
    remove_old_reports();

    SYSTEMTIME now;
    GetLocalTime(&now);
    const std::string stem =
        crash_report::file_stem(now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    char time[32];
    snprintf(time, sizeof(time), "%04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour,
             now.wMinute, now.wSecond);

    crash_report::Report report;
    // The crashed thread may hold the lock (or be stopped while setting the
    // list): then the report says the mods are unknown.
    std::vector<std::pair<uintptr_t, std::string>> modules;
    if (TryAcquireSRWLockShared(&g_mods_lock)) {
        report.mods = g_mods;
        report.mods_known = g_mods_known;
        modules = g_modules;
        ReleaseSRWLockShared(&g_mods_lock);
    }

    const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;
    const CONTEXT& context = *pointers->ContextRecord;
    report.time = time;
    report.tome_version = TOME_VERSION;
    report.game = g_game;
    report.code = record.ExceptionCode;
    for (DWORD i = 0; i < record.NumberParameters && i < EXCEPTION_MAXIMUM_PARAMETERS; ++i) {
        report.info.push_back(static_cast<uint32_t>(record.ExceptionInformation[i]));
    }
    report.at = locate(reinterpret_cast<uintptr_t>(record.ExceptionAddress), modules);
    report.thread = thread;
    report.main_thread = thread == g_main_thread;
    const DWORD registers[10] = {context.Eax, context.Ebx, context.Ecx, context.Edx, context.Esi,
                                 context.Edi, context.Ebp, context.Esp, context.Eip, context.EFlags};
    std::copy(std::begin(registers), std::end(registers), report.registers);
    report.callers = callers(context.Esp, modules);

    if (!g_write_dump) {
        report.dump_error = "dbghelp.dll isn't loaded";
    } else {
        const std::wstring dump_path = g_dir + L"\\" + widen(stem) + L".dmp";
        HANDLE file = CreateFileW(dump_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            report.dump_error = "error " + std::to_string(GetLastError()) + " creating it";
        } else {
            MINIDUMP_EXCEPTION_INFORMATION info;
            info.ThreadId = thread;
            info.ExceptionPointers = pointers;
            info.ClientPointers = FALSE;
            const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory |
                                                         MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData);
            bool ok = g_write_dump(GetCurrentProcess(), GetCurrentProcessId(), file, type, &info, nullptr, nullptr);
            DWORD error = GetLastError();
            CloseHandle(file);
            if (ok) {
                report.dump = stem + ".dmp";
            } else {
                report.dump_error = "error " + std::to_string(error);
                DeleteFileW(dump_path.c_str());
            }
        }
    }
    report.log_tail = read_log_tail();

    if (!write_text(g_dir + L"\\" + widen(stem) + L".txt", crash_report::format(report))) return "";
    std::string line = "Crash outside a mod call: " + crash_report::summary(report) + " (thread " +
                       std::to_string(thread) + "); report in tome\\crashes\\" + stem + ".txt";
    log::try_write(log::Level::Error, line.c_str());
    if (g_sink) g_sink(stem);
    return stem;
}

DWORD WINAPI writer_thread(LPVOID) {
    for (;;) {
        WaitForSingleObject(g_request, INFINITE);
        g_request_stem = write_now(g_request_pointers, g_request_thread);
        SetEvent(g_done);
    }
}

uintptr_t depth() {
    return g_depth_slot == TLS_OUT_OF_INDEXES ? 0 : reinterpret_cast<uintptr_t>(TlsGetValue(g_depth_slot));
}

// Calls `next` with this thread's depth one more (a TLS slot: no heap).
LONG hand_on(LPTOP_LEVEL_EXCEPTION_FILTER next, EXCEPTION_POINTERS* pointers, uintptr_t depth) {
    if (!next) return EXCEPTION_CONTINUE_SEARCH;
    if (g_depth_slot != TLS_OUT_OF_INDEXES) TlsSetValue(g_depth_slot, reinterpret_cast<void*>(depth + 1));
    LONG result = next(pointers);
    if (g_depth_slot != TLS_OUT_OF_INDEXES) TlsSetValue(g_depth_slot, reinterpret_cast<void*>(depth));
    return result;
}

LONG WINAPI filter(EXCEPTION_POINTERS* pointers) {
    const DWORD thread = GetCurrentThreadId();
    const LONG busy = InterlockedCompareExchange(&g_busy, 0, 0);
    if (busy == static_cast<LONG>(thread) || (g_writer && thread == g_writer_id)) {
        // A fault while writing a report: leave it to the game's filter.
        return g_next ? g_next(pointers) : EXCEPTION_CONTINUE_SEARCH;
    }
    if (const uintptr_t d = depth()) {
        // Back here while handing this thread's crash on: a filter after
        // Tome's chained to the one it replaced (Tome's, which check_filter
        // put in front of it again), or one faulted. Reported already; the
        // game's filter once, unless it's the one that came back.
        if (d == 1 && g_first != g_next) return hand_on(g_first, pointers, d);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (busy) {
        // Another thread's crash is being reported, one at a time: this one
        // waits for it, then goes to the game's filter unreported.
        for (DWORD waited = 0; InterlockedCompareExchange(&g_busy, 0, 0) != 0 && waited < kWriteTimeoutMs;
             waited += 50) {
            Sleep(50);
        }
        return hand_on(g_next, pointers, 0);
    }
    write_report(pointers, thread);
    return hand_on(g_next, pointers, 0);
}

} // namespace

void install(const Paths& paths, DWORD main_thread) {
    g_tome_dir = paths.tome_dir;
    g_dir = paths.tome_dir + L"\\crashes";
    g_main_thread = main_thread;
    if (g_depth_slot == TLS_OUT_OF_INDEXES) g_depth_slot = TlsAlloc();
    g_next = SetUnhandledExceptionFilter(filter);
    g_first = g_next;
}

void start(const GameBuild& build) {
    if (g_dir.empty()) return;  // not installed
    if (build.known) {
        g_game = build.known->data->name + " (game.dll " + build.file_version + ")";
    } else if (!build.file_version.empty()) {
        g_game = "game.dll " + build.file_version;
    }
    wchar_t system[MAX_PATH];
    UINT length = GetSystemDirectoryW(system, MAX_PATH);
    if (length && length < MAX_PATH) {
        if (HMODULE dbghelp = LoadLibraryW((std::wstring(system) + L"\\dbghelp.dll").c_str())) {
            g_write_dump = reinterpret_cast<MiniDumpWriteDumpFn>(
                reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
        }
    }
    if (!g_write_dump) TOME_WARN("Crash reports: no MiniDumpWriteDump in dbghelp.dll; reports come without a minidump");
    g_request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_request && g_done) g_writer = CreateThread(nullptr, 0, writer_thread, nullptr, 0, &g_writer_id);
    if (!g_writer) TOME_WARN("Crash reports: no writer thread; a crashed thread writes its own report");
    // game.dll's filter, CrashHandler::Filter (reverse-engineering.md, "Crash handler").
    std::string next = g_next ? crash_report::where(locate(reinterpret_cast<uintptr_t>(g_next), {})) : "";
    TOME_INFO("Crash reports: tome\\crashes, in front of the filter %s", g_next ? next.c_str() : "of Windows");
}

void check_filter() {
    if (g_dir.empty()) return;  // not installed
    LPTOP_LEVEL_EXCEPTION_FILTER previous = SetUnhandledExceptionFilter(filter);
    if (previous == filter) return;
    TOME_WARN("Crash reports: another exception filter replaced Tome's (%p); Tome's goes in front of it again",
              reinterpret_cast<void*>(previous));
    g_next = previous;
}

void add_module(uintptr_t base, const std::string& id) {
    ExclusiveLock hold(g_mods_lock);
    g_modules.emplace_back(base, id);
}

void set_mods(std::vector<crash_report::ModLine> mods, std::vector<std::pair<uintptr_t, std::string>> modules) {
    ExclusiveLock hold(g_mods_lock);
    g_mods = std::move(mods);
    g_modules = std::move(modules);
    g_mods_known = true;
}

std::string write_report(EXCEPTION_POINTERS* pointers, DWORD thread) {
    const DWORD self = GetCurrentThreadId();
    if (InterlockedCompareExchange(&g_busy, static_cast<LONG>(self), 0) != 0) return "";
    std::string stem;
    if (g_writer && self != g_writer_id) {
        g_request_pointers = pointers;
        g_request_thread = thread;
        g_request_stem.clear();
        SetEvent(g_request);
        if (WaitForSingleObject(g_done, kWriteTimeoutMs) == WAIT_OBJECT_0) stem = g_request_stem;
        // On a timeout the writer is stuck (a lock the crash left held); it
        // stays busy, so no later report waits for it again.
        else return "";
    } else {
        stem = write_now(pointers, thread);
    }
    InterlockedExchange(&g_busy, 0);
    return stem;
}

void set_report_sink(ReportSink sink) {
    g_sink = sink;
}

} // namespace tome::crash_handler
