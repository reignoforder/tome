#include "paths.h"

namespace tome {
namespace {

std::wstring module_path(HMODULE module) {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        DWORD len = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (len == 0) {
            return {};
        }
        if (len < path.size()) {
            path.resize(len);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::wstring dir_of(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring file_of(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

} // namespace

bool init_paths(HMODULE self, Paths& out) {
    std::wstring self_path = module_path(self);
    std::wstring host_path = module_path(nullptr);
    if (self_path.empty() || host_path.empty()) {
        return false;
    }

    out.game_dir = dir_of(self_path);
    out.host_exe = file_of(host_path);

    // TOME_HOME relocates mods/ and tome/ (tests, or separate mod setups).
    std::wstring home = out.game_dir;
    wchar_t env[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"TOME_HOME", env, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        home.assign(env, len);
        while (!home.empty() && (home.back() == L'\\' || home.back() == L'/')) home.pop_back();
        CreateDirectoryW(home.c_str(), nullptr);
    }
    out.tome_dir = home + L"\\tome";
    out.mods_dir = home + L"\\mods";

    if (!CreateDirectoryW(out.tome_dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    return true;
}

std::string narrow(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                  nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), len);
    return out;
}

bool file_stamp(const std::wstring& path, FileStamp& out) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    out.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    out.write_time = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                     data.ftLastWriteTime.dwLowDateTime;
    return true;
}

bool read_file(const std::wstring& path, std::string& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart < (16 << 20);
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = out.empty() || (ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) &&
                             read == out.size());
    }
    CloseHandle(file);
    return ok;
}

bool write_file_atomic(const std::wstring& path, const std::string& data) {
    std::wstring tmp = path + L".tmp";
    HANDLE file = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
              written == data.size();
    CloseHandle(file);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace tome
