#include "log.h"
#include "unwind.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>

namespace tome::log {
namespace {

HANDLE g_file = INVALID_HANDLE_VALUE;
SRWLOCK g_lock = SRWLOCK_INIT;
bool g_keep = false;
std::string g_kept;  // every line since keep_lines()

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
        // Cut short: still one line, for find_kept and readers of the file.
        len = sizeof(line) - 1;
        line[len - 2] = '\r';
        line[len - 1] = '\n';
    }

    OutputDebugStringA(line);

    ExclusiveLock hold(g_lock);
    if (g_keep) g_kept.append(line, static_cast<size_t>(len));
    if (g_file != INVALID_HANDLE_VALUE) {
        // Unbuffered on our side: if the game crashes, everything up to the
        // crash is already in the OS file cache.
        DWORD written;
        WriteFile(g_file, line, static_cast<DWORD>(len), &written, nullptr);
    }
}

} // namespace tome::log
