// SPDX-License-Identifier: GPL-3.0-or-later
#include "paths.h"

#include "core/json.h"
#include "core/metadata.h"

namespace tome {
namespace {

bool is_dir(const std::wstring& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool dir_is_empty(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return true;
    bool empty = true;
    do {
        if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) empty = false;
    } while (empty && FindNextFileW(find, &fd));
    FindClose(find);
    return empty;
}

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

    // TOME_HOME relocates tome/ (tests, or separate mod setups).
    std::wstring home = out.game_dir;
    wchar_t env[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"TOME_HOME", env, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        home.assign(env, len);
        while (!home.empty() && (home.back() == L'\\' || home.back() == L'/')) home.pop_back();
        CreateDirectoryW(home.c_str(), nullptr);
    }
    out.home_dir = home;
    out.tome_dir = home + L"\\tome";
    out.mods_dir = out.tome_dir + L"\\mods";
    out.mod_config_dir = out.mods_dir + L"\\config";
    out.packages_dir = out.tome_dir + L"\\packages";

    if (!CreateDirectoryW(out.tome_dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    return true;
}

std::vector<std::string> migrate_layout(const Paths& paths) {
    std::vector<std::string> notes;
    const std::wstring old_mods = paths.home_dir + L"\\mods";
    bool moved = false;
    if (is_dir(old_mods)) {
        if (exists(paths.mods_dir)) {
            notes.push_back("the old mods folder " + narrow(old_mods) + " isn't read any more; move its mods to " +
                            narrow(paths.mods_dir));
        } else if (MoveFileW(old_mods.c_str(), paths.mods_dir.c_str())) {
            notes.push_back("moved " + narrow(old_mods) + " to " + narrow(paths.mods_dir));
            moved = true;
        } else {
            notes.push_back("couldn't move " + narrow(old_mods) + " to " + narrow(paths.mods_dir) + " (error " +
                            std::to_string(GetLastError()) + ")");
        }
    }
    CreateDirectoryW(paths.mods_dir.c_str(), nullptr);
    CreateDirectoryW(paths.mod_config_dir.c_str(), nullptr);

    // Settings from mods/<folder>/config/, the old layout's, only when the old
    // folder was just moved: in tome/mods a mod's config/ is its own files.
    if (!moved) return notes;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((paths.mods_dir + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return notes;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.' ||
            _wcsicmp(fd.cFileName, L"config") == 0) {
            continue;
        }
        const std::wstring dir = paths.mods_dir + L"\\" + fd.cFileName;
        const std::wstring old_config = dir + L"\\config";
        if (!is_dir(old_config)) continue;
        std::string text, id;
        json::Value root;
        if (read_file(dir + L"\\tome.mod.json", text) && json::parse(text, root).ok) {
            if (const json::Value* value = root.find("id"); value && value->is_string()) id = value->as_string();
        }
        const std::string where = narrow(fd.cFileName) + "\\config";
        if (!is_valid_mod_id(id)) {
            notes.push_back(where + ": no mod id in tome.mod.json; left where it is");
            continue;
        }
        const std::wstring old_settings = old_config + L"\\settings.json";
        const std::wstring new_settings = paths.mod_config_dir + L"\\" + widen(id) + L".json";
        if (exists(old_settings)) {
            if (exists(new_settings)) {
                notes.push_back(where + "\\settings.json: config\\" + id + ".json exists already; not moved");
            } else if (MoveFileW(old_settings.c_str(), new_settings.c_str())) {
                notes.push_back("moved " + where + "\\settings.json to config\\" + id + ".json");
            } else {
                notes.push_back("couldn't move " + where + "\\settings.json (error " + std::to_string(GetLastError()) +
                                ")");
            }
        }
        if (dir_is_empty(old_config)) {
            RemoveDirectoryW(old_config.c_str());
            continue;
        }
        const std::wstring new_dir = paths.mod_config_dir + L"\\" + widen(id);
        if (exists(new_dir)) {
            notes.push_back(where + ": config\\" + id + " exists already; not moved");
        } else if (MoveFileW(old_config.c_str(), new_dir.c_str())) {
            notes.push_back("moved " + where + " to config\\" + id);
        } else {
            notes.push_back("couldn't move " + where + " (error " + std::to_string(GetLastError()) + ")");
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return notes;
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
