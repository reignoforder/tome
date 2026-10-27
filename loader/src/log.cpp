#include "log.h"
#include "unwind.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace tome::log {
namespace {

HANDLE g_file = INVALID_HANDLE_VALUE;
SRWLOCK g_lock = SRWLOCK_INIT;

const char* level_name(Level level) {
    switch (level) {
    case Level::Debug: return "DEBUG";
    case Level::Info:  return "INFO ";
    case Level::Warn:  return "WARN ";
    case Level::Error: return "ERROR";
    }
    return "?????";
}

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
    return true;
}

void write(Level level, const char* fmt, ...) {
    char message[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    SYSTEMTIME now;
    GetLocalTime(&now);

    char line[2200];
    int len = snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u] [%5lu] [%s] %s\r\n",
                       now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
                       GetCurrentThreadId(), level_name(level), message);
    if (len < 0) {
        return;
    }
    if (len >= static_cast<int>(sizeof(line))) {
        len = sizeof(line) - 1;
    }

    OutputDebugStringA(line);

    ExclusiveLock hold(g_lock);
    if (g_file != INVALID_HANDLE_VALUE) {
        // Unbuffered on our side: if the game crashes, everything up to the
        // crash is already in the OS file cache.
        DWORD written;
        WriteFile(g_file, line, static_cast<DWORD>(len), &written, nullptr);
    }
}

} // namespace tome::log
