#include "packages.h"

#include "core/hash.h"
#include "core/package.h"
#include "core/zip.h"
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace tome::packages {
namespace {

bool read_whole(const std::wstring& path, std::string& out, std::string& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "can't open the package (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(file, &size);
    if (ok && static_cast<uint64_t>(size.QuadPart) > package::kMaxFileSize) {
        error = "the package is larger than " + std::to_string(package::kMaxFileSize >> 20) + " MiB";
        CloseHandle(file);
        return false;
    }
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        size_t done = 0;
        while (ok && done < out.size()) {
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(out.size() - done, 1u << 24)), got = 0;
            ok = ReadFile(file, out.data() + done, chunk, &got, nullptr) && got == chunk;
            done += got;
        }
    }
    CloseHandle(file);
    if (!ok) error = "can't read the package (error " + std::to_string(GetLastError()) + ")";
    return ok;
}

bool is_dir(const std::wstring& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// Creates `dir` and the folders above it, up to `root` (which exists).
bool make_dirs(const std::wstring& root, const std::wstring& dir) {
    if (dir.size() <= root.size() || is_dir(dir)) return true;
    size_t slash = dir.find_last_of(L'\\');
    if (slash != std::wstring::npos && slash > root.size() && !make_dirs(root, dir.substr(0, slash))) return false;
    return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

// Deletes a folder and everything in it; false if anything is left.
bool remove_tree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            std::wstring path = dir + L"\\" + name;
            // Never follow a link out of the folder: remove the link itself.
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                remove_tree(path);
            } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveDirectoryW(path.c_str());
            } else {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) {
                    SetFileAttributesW(path.c_str(), fd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
                }
                DeleteFileW(path.c_str());
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    return RemoveDirectoryW(dir.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
}

bool write_new(const std::wstring& path, const std::string& data, DWORD& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return false;
    }
    size_t done = 0;
    bool ok = true;
    while (ok && done < data.size()) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - done, 1u << 24)), wrote = 0;
        ok = WriteFile(file, data.data() + done, chunk, &wrote, nullptr) && wrote == chunk;
        done += wrote;
    }
    if (!ok) error = GetLastError();
    CloseHandle(file);
    return ok;
}

} // namespace

Unpacked unpack(const Paths& paths, const std::wstring& file) {
    Unpacked result;
    const std::wstring file_name = file.substr(file.find_last_of(L'\\') + 1);
    const std::string stem = package::stem(narrow(file_name));
    std::string archive;
    if (!read_whole(file, archive, result.error)) return result;

    std::vector<zip::Entry> entries;
    if (!zip::read_directory(archive, entries, result.error) || !package::check_entries(entries, result.error)) {
        return result;
    }
    Fnv1a64 hash;
    hash.mix(archive.data(), archive.size());
    const std::wstring final_dir = paths.packages_dir + L"\\" + widen(package::unpacked_name(stem, hash.value));
    if (is_dir(final_dir)) {  // unpacked before: only a complete unpack is ever renamed to this name
        result.dir = final_dir;
        return result;
    }

    CreateDirectoryW(paths.packages_dir.c_str(), nullptr);
    const std::wstring partial =
        paths.packages_dir + L"\\" +
        widen(package::partial_name(narrow(final_dir.substr(final_dir.find_last_of(L'\\') + 1)), GetCurrentProcessId()));
    remove_tree(partial);
    if (!CreateDirectoryW(partial.c_str(), nullptr)) {
        result.error = "can't create " + narrow(partial) + " (error " + std::to_string(GetLastError()) + ")";
        return result;
    }
    const DWORD started = GetTickCount();
    std::string data;
    for (const zip::Entry& entry : entries) {
        std::wstring path = partial + L"\\" + widen(entry.name);
        for (wchar_t& c : path) {
            if (c == L'/') c = L'\\';
        }
        if (entry.directory()) path.pop_back();
        const std::wstring parent = entry.directory() ? path : path.substr(0, path.find_last_of(L'\\'));
        if (!make_dirs(partial, parent)) {
            result.error = "can't create the folder for \"" + entry.name + "\" (error " +
                           std::to_string(GetLastError()) + ")";
            break;
        }
        if (entry.directory()) continue;
        if (!zip::extract(archive, entry, data, result.error)) break;
        DWORD error = 0;
        if (!write_new(path, data, error)) {
            result.error = error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS
                               ? "\"" + entry.name + "\" is the same file as another entry on this system"
                               : "can't write \"" + entry.name + "\" (error " + std::to_string(error) + ")";
            break;
        }
    }
    if (result.error.empty() && !MoveFileW(partial.c_str(), final_dir.c_str())) {
        // Another game instance may have finished the same unpack first.
        if (!is_dir(final_dir)) {
            result.error = "can't rename the unpacked folder (error " + std::to_string(GetLastError()) + ")";
        }
    }
    if (!result.error.empty() || is_dir(partial)) remove_tree(partial);
    if (!result.error.empty()) return result;
    result.dir = final_dir;
    TOME_INFO("Unpacked %s (%zu entries, %lu ms)", narrow(file_name).c_str(), entries.size(),
              GetTickCount() - started);
    return result;
}

void remove_unused(const Paths& paths, const std::set<std::wstring>& keep) {
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((paths.packages_dir + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> unused;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring dir = paths.packages_dir + L"\\" + fd.cFileName;
        if (!package::is_unpacked_name(narrow(fd.cFileName)) || keep.count(dir)) continue;
        // Another instance's unpack in progress has its process id in the name.
        std::string name = narrow(fd.cFileName);
        if (name.size() > 8 && name.substr(name.size() - 8) == ".partial") {
            std::string id = name.substr(0, name.size() - 8);
            id = id.substr(id.find_last_of('.') + 1);
            HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(std::strtoul(id.c_str(), nullptr, 10)));
            if (process) {
                bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
                CloseHandle(process);
                if (running && std::strtoul(id.c_str(), nullptr, 10) != GetCurrentProcessId()) continue;
            }
        }
        unused.push_back(dir);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    for (const std::wstring& dir : unused) {
        if (remove_tree(dir)) TOME_INFO("Removed the unused unpacked package %s", narrow(dir.substr(dir.find_last_of(L'\\') + 1)).c_str());
    }
}

} // namespace tome::packages
