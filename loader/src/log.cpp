// SPDX-License-Identifier: GPL-3.0-or-later
#include "log.h"
#include "unwind.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace tome::log {
namespace {

HANDLE g_file = INVALID_HANDLE_VALUE;
std::wstring g_path;  // of g_file
SRWLOCK g_lock = SRWLOCK_INIT;
bool g_keep = false;
std::string g_kept;  // every line since keep_lines()

} // namespace

bool open(const std::wstring& path) {
    // FILE_SHARE_READ so the log can be tailed while the game runs.
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    ExclusiveLock hold(g_lock);
    g_file = file;
    g_path = path;
    return true;
}

void close() {
    ExclusiveLock hold(g_lock);
    if (g_file != INVALID_HANDLE_VALUE) CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;
    g_path.clear();
}

std::string read_tail(size_t max_bytes) {
    std::wstring path;
    {
        SharedLock hold(g_lock);
        path = g_path;
    }
    if (path.empty()) return "";
    // Our own handle writes: the share mode has to allow that.
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return "";
    std::string text;
    LARGE_INTEGER size;
    if (GetFileSizeEx(file, &size)) {
        LONGLONG start = size.QuadPart - static_cast<LONGLONG>(max_bytes);
        if (start < 0) start = 0;
        LARGE_INTEGER at;
        at.QuadPart = start;
        text.resize(static_cast<size_t>(size.QuadPart - start));
        DWORD read = 0;
        if (!SetFilePointerEx(file, at, nullptr, FILE_BEGIN) ||
            !ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr)) {
            read = 0;
        }
        text.resize(read);
    }
    CloseHandle(file);
    return text;
}

void keep_lines(bool keep) {
    ExclusiveLock hold(g_lock);
    g_keep = keep;
    if (!keep) std::string().swap(g_kept);
}

bool find_kept(size_t& from, std::string_view text) {
    SharedLock hold(g_lock);
    if (from > g_kept.size()) return false;
    size_t at = g_kept.find(text, from);
    if (at == std::string::npos) return false;
    size_t end = g_kept.find('\n', at);
    from = end == std::string::npos ? g_kept.size() : end + 1;
    return true;
}

size_t kept_size() {
    SharedLock hold(g_lock);
    return g_kept.size();
}

namespace {

// One line as it goes into the file (core/log_line.h), stamped now.
int stamped_line(char (&line)[2200], Level level, const char* message) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    return log::format_line(line, sizeof(line), now.wHour, now.wMinute, now.wSecond,
                            now.wMilliseconds, GetCurrentThreadId(), level, message);
}

// With g_lock held.
void emit(const char* message, const char* line, int len) {
    if (g_keep && std::strncmp(message, "Debug ", 6) != 0) g_kept.append(line, static_cast<size_t>(len));
    if (g_file != INVALID_HANDLE_VALUE) {
        // Unbuffered on our side: if the game crashes, everything up to the
        // crash is already in the OS file cache.
        DWORD written;
        WriteFile(g_file, line, static_cast<DWORD>(len), &written, nullptr);
    }
}

} // namespace

void write(Level level, const char* fmt, ...) {
    char message[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    char line[2200];
    int len = stamped_line(line, level, message);
    if (len < 0) return;
    OutputDebugStringA(line);

    ExclusiveLock hold(g_lock);
    emit(message, line, len);
}

bool try_write(Level level, const char* message) {
    char line[2200];
    int len = stamped_line(line, level, message);
    if (len < 0) return false;
    OutputDebugStringA(line);
    // Another thread's write takes microseconds; a crashed thread's never ends.
    // Sleep(1) can take a whole timer tick (15.6 ms), so count the time, not the tries.
    const DWORD start = GetTickCount();
    while (!TryAcquireSRWLockExclusive(&g_lock)) {
        if (GetTickCount() - start >= 100) return false;
        Sleep(1);
    }
    emit(message, line, len);
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

} // namespace tome::log
