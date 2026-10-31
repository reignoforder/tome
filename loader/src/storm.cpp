#include "storm.h"

#include "log.h"

#include <windows.h>

namespace tome::storm {
namespace {

using SFileOpenArchiveFn = BOOL(WINAPI*)(const char* name, DWORD priority, DWORD flags, HANDLE* archive);
using SFileCloseArchiveFn = BOOL(WINAPI*)(HANDLE archive);
using SFileOpenFileExFn = BOOL(WINAPI*)(HANDLE archive, const char* name, DWORD scope, HANDLE* file);
using SFileGetFileSizeFn = DWORD(WINAPI*)(HANDLE file, DWORD* high);
using SFileReadFileFn = BOOL(WINAPI*)(HANDLE file, void* buffer, DWORD size, DWORD* read, void* overlapped);
using SFileCloseFileFn = BOOL(WINAPI*)(HANDLE file);

SFileOpenArchiveFn g_open_archive = nullptr;
SFileCloseArchiveFn g_close_archive = nullptr;
SFileOpenFileExFn g_open_file_ex = nullptr;
SFileGetFileSizeFn g_get_file_size = nullptr;
SFileReadFileFn g_read_file = nullptr;
SFileCloseFileFn g_close_file = nullptr;

template <typename Fn>
Fn by_ordinal(HMODULE storm, int ordinal) {
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(storm, MAKEINTRESOURCEA(ordinal))));
}

} // namespace

bool init() {
    HMODULE storm = GetModuleHandleW(L"storm.dll");
    if (!storm) return false;
    g_open_archive = by_ordinal<SFileOpenArchiveFn>(storm, 266);
    g_close_archive = by_ordinal<SFileCloseArchiveFn>(storm, 252);
    g_open_file_ex = by_ordinal<SFileOpenFileExFn>(storm, 268);
    g_get_file_size = by_ordinal<SFileGetFileSizeFn>(storm, 265);
    g_read_file = by_ordinal<SFileReadFileFn>(storm, 269);
    g_close_file = by_ordinal<SFileCloseFileFn>(storm, 253);
    return g_open_archive && g_close_archive && g_open_file_ex && g_get_file_size && g_read_file && g_close_file;
}

bool read_file(const char* name, std::string& out, size_t max, bool* too_large) {
    if (too_large) *too_large = false;
    HANDLE file = nullptr;
    if (!g_open_file_ex || !g_open_file_ex(nullptr, name, 0, &file) || !file) return false;
    DWORD size = g_get_file_size(file, nullptr);
    bool ok = size != 0xFFFFFFFF;
    if (ok && size > max) {
        ok = false;
        if (too_large) *too_large = true;
    }
    if (ok) {
        out.resize(size);
        DWORD read = 0;
        ok = size == 0 || (g_read_file(file, out.data(), size, &read, nullptr) && read == size);
    }
    g_close_file(file);
    return ok;
}

void* open_archive(const std::wstring& path, unsigned priority) {
    if (!g_open_archive) return nullptr;
    // Storm takes ANSI paths; the overlay lives in the game folder, which the
    // game itself can open, so the ANSI code page covers it.
    char ansi[MAX_PATH * 2];
    if (!WideCharToMultiByte(CP_ACP, 0, path.c_str(), -1, ansi, sizeof(ansi), nullptr, nullptr)) return nullptr;
    HANDLE archive = nullptr;
    if (!g_open_archive(ansi, priority, 0, &archive) || !archive) {
        TOME_ERROR("SFileOpenArchive(%s) failed (error %lu)", ansi, GetLastError());
        return nullptr;
    }
    return archive;
}

void close_archive(void* archive) {
    if (archive && g_close_archive) g_close_archive(archive);
}

} // namespace tome::storm
