#include "overlay.h"

#include "core/assets.h"
#include "core/hash.h"
#include "log.h"
#include "storm.h"
#include "unwind.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <map>

namespace tome::overlay {
namespace {

constexpr unsigned kGeneratedPriority = 15;
constexpr unsigned kAssetsPriority = 12;
constexpr uint32_t kMaxFileSize = 0x7FFFFFFF;

struct Slot {
    const wchar_t* prefix;
    unsigned priority;
    void* archive = nullptr;
    std::wstring path;
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::wstring g_cache;
unsigned g_counter = 0;
Slot g_generated{L"overlay", kGeneratedPriority, nullptr, {}};
Slot g_assets{L"assets", kAssetsPriority, nullptr, {}};
std::vector<std::wstring> g_stale;  // replaced archives Storm hadn't released yet

std::map<std::string, std::string> g_generated_files;  // name -> contents

// Where each mounted mod's files are in the current assets archive.
struct Entry {
    std::string name;
    uint32_t offset;
    uint32_t size;
};
std::map<std::string, std::vector<Entry>> g_mounted_entries;  // by mod id

// Sequential output to a new archive file; the header goes in last.
class Output {
public:
    explicit Output(HANDLE file) : file_(file) {}

    void write(const void* data, size_t size) {
        DWORD written = 0;
        if (ok_ && size && !(WriteFile(file_, data, static_cast<DWORD>(size), &written, nullptr) && written == size)) {
            ok_ = false;
        }
        pos_ += size;
    }
    void write_header(const std::string& header) {
        LARGE_INTEGER zero{};
        if (!SetFilePointerEx(file_, zero, nullptr, FILE_BEGIN)) ok_ = false;
        write(header.data(), header.size());
    }
    // Copies `size` bytes from `in` at its current position; pads with zeros
    // if the file turns out shorter, so the archive stays consistent.
    void copy(HANDLE in, uint32_t size, std::vector<char>& buffer) {
        while (size > 0) {
            DWORD want = std::min<DWORD>(size, static_cast<DWORD>(buffer.size())), got = 0;
            if (!ReadFile(in, buffer.data(), want, &got, nullptr) || got == 0) {
                std::fill(buffer.begin(), buffer.begin() + want, 0);
                got = want;
            }
            write(buffer.data(), got);
            size -= got;
        }
    }
    uint64_t pos() const { return pos_; }
    bool ok() const { return ok_; }

private:
    HANDLE file_;
    uint64_t pos_ = 0;
    bool ok_ = true;
};

HANDLE create_archive_file(const Slot& slot, std::wstring& path) {
    CreateDirectoryW(g_cache.c_str(), nullptr);
    // A name still in use (another game instance) is skipped.
    for (int attempt = 0; attempt < 100; ++attempt) {
        path = g_cache + L"\\" + slot.prefix + L"-" + std::to_wstring(++g_counter) + L".mpq";
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) return file;
    }
    TOME_ERROR("Couldn't create an archive in %s", narrow(g_cache).c_str());
    return INVALID_HANDLE_VALUE;
}

void delete_stale() {
    g_stale.erase(std::remove_if(g_stale.begin(), g_stale.end(),
                                 [](const std::wstring& path) { return DeleteFileW(path.c_str()) != 0; }),
                  g_stale.end());
}

void unmount(Slot& slot) {
    if (!slot.archive) return;
    storm::close_archive(slot.archive);
    g_stale.push_back(slot.path);
    slot.archive = nullptr;
    slot.path.clear();
    delete_stale();
}

// Opens the new archive, then closes the slot's old one.
bool swap_in(Slot& slot, const std::wstring& path) {
    void* archive = storm::open_archive(path, slot.priority);
    if (!archive) {
        DeleteFileW(path.c_str());
        return false;
    }
    unmount(slot);
    slot.archive = archive;
    slot.path = path;
    return true;
}

HANDLE open_for_reading(const std::wstring& path) {
    return CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

bool ignored_name(const wchar_t* name) {
    return name[0] == L'.' || _wcsicmp(name, L"Thumbs.db") == 0 || _wcsicmp(name, L"desktop.ini") == 0;
}

void scan_dir(const std::wstring& dir, const std::string& prefix, int depth, std::vector<AssetFile>& files,
              uint64_t& hash, std::vector<std::string>& warnings) {
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    auto mix = [&hash](const void* data, size_t len) {
        auto bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < len; ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
    };
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0 || ignored_name(fd.cFileName)) {
            continue;
        }
        std::wstring path = dir + L"\\" + fd.cFileName;
        std::string relative = prefix + narrow(fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth > 0) scan_dir(path, relative + "/", depth - 1, files, hash, warnings);
            continue;
        }
        std::string name = assets::archive_path(relative);
        if (name.empty()) {
            warnings.push_back("asset \"" + relative + "\" skipped: not a usable archive path");
            continue;
        }
        if (fd.nFileSizeHigh != 0 || fd.nFileSizeLow > kMaxFileSize) {
            warnings.push_back("asset \"" + relative + "\" skipped: larger than 2 GiB");
            continue;
        }
        mix(name.data(), name.size() + 1);
        mix(&fd.nFileSizeLow, sizeof(fd.nFileSizeLow));
        mix(&fd.ftLastWriteTime, sizeof(fd.ftLastWriteTime));
        files.push_back({std::move(name), std::move(path), fd.nFileSizeLow});
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

} // namespace

void init(const Paths& paths) {
    g_cache = paths.tome_dir + L"\\cache";
    CreateDirectoryW(g_cache.c_str(), nullptr);
    // Archives in use by another game instance fail to delete and stay.
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((g_cache + L"\\*.mpq").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            DeleteFileW((g_cache + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
}

bool put_generated(const std::vector<mpq::File>& files) {
    ExclusiveLock hold(g_lock);
    std::map<std::string, std::string> updated = g_generated_files;
    for (const mpq::File& f : files) updated[f.name] = f.data;
    std::vector<mpq::File> all;
    for (const auto& [name, data] : updated) all.push_back({name, data});
    std::string archive = mpq::build_archive(all);

    bool ok = storm::init();
    if (!ok) TOME_ERROR("Overlay: Storm functions not found");
    std::wstring path;
    HANDLE file = ok ? create_archive_file(g_generated, path) : INVALID_HANDLE_VALUE;
    if (file != INVALID_HANDLE_VALUE) {
        Output out(file);
        out.write(archive.data(), archive.size());
        CloseHandle(file);
        ok = out.ok();
        if (!ok) {
            TOME_ERROR("Overlay: couldn't write %s", narrow(path).c_str());
            DeleteFileW(path.c_str());
        }
    } else {
        ok = false;
    }
    if (ok) ok = swap_in(g_generated, path);
    if (ok) g_generated_files = std::move(updated);
    return ok;
}

void scan_assets(const std::wstring& dir, std::vector<AssetFile>& files, uint64_t& fingerprint,
                 std::vector<std::string>& warnings) {
    files.clear();
    uint64_t hash = 14695981039346656037ull;
    scan_dir(dir, "", 32, files, hash, warnings);
    fingerprint = files.empty() ? 0 : (hash ? hash : 1);
}

bool mount_assets(const std::vector<ModSource>& mods, std::string& summary) {
    ExclusiveLock hold(g_lock);
    if (mods.empty()) {
        unmount(g_assets);
        g_mounted_entries.clear();
        summary = "no mod assets mounted";
        return true;
    }
    if (!storm::init()) {
        summary = "Storm functions not found";
        return false;
    }
    std::wstring path;
    HANDLE file = create_archive_file(g_assets, path);
    if (file == INVALID_HANDLE_VALUE) {
        summary = "couldn't create the archive";
        return false;
    }
    HANDLE previous = g_assets.path.empty() ? INVALID_HANDLE_VALUE : open_for_reading(g_assets.path);

    DWORD started = GetTickCount();
    Output out(file);
    std::vector<char> buffer(1 << 20);
    out.write(buffer.data(), mpq::ArchiveWriter::header_size());
    mpq::ArchiveWriter writer;
    std::map<std::string, std::vector<Entry>> entries;
    size_t overridden = 0;
    std::vector<std::string> problems;

    // Highest precedence first: the first file with a name wins.
    for (auto mod = mods.rbegin(); mod != mods.rend(); ++mod) {
        std::vector<Entry>& placed = entries[mod->id];
        if (mod->keep_previous) {
            auto old = g_mounted_entries.find(mod->id);
            if (old == g_mounted_entries.end() || previous == INVALID_HANDLE_VALUE) {
                problems.push_back(mod->id + ": previous files unavailable");
                continue;
            }
            for (const Entry& e : old->second) {
                if (writer.contains(e.name)) {
                    ++overridden;
                    continue;
                }
                LARGE_INTEGER at;
                at.QuadPart = e.offset;
                uint32_t offset = static_cast<uint32_t>(out.pos());
                if (!SetFilePointerEx(previous, at, nullptr, FILE_BEGIN) || !writer.add(e.name, e.size)) {
                    problems.push_back(mod->id + ": couldn't copy " + e.name);
                    continue;
                }
                out.copy(previous, e.size, buffer);
                placed.push_back({e.name, offset, e.size});
            }
            continue;
        }
        for (const AssetFile& f : mod->files) {
            if (writer.contains(f.name)) {
                ++overridden;
                continue;
            }
            HANDLE in = open_for_reading(f.path);
            LARGE_INTEGER size{};
            if (in == INVALID_HANDLE_VALUE || !GetFileSizeEx(in, &size) || size.QuadPart > kMaxFileSize) {
                if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
                problems.push_back(mod->id + ": can't read " + narrow(f.path));
                continue;
            }
            uint32_t offset = static_cast<uint32_t>(out.pos());
            uint32_t bytes = static_cast<uint32_t>(size.QuadPart);
            if (!writer.add(f.name, bytes)) {
                CloseHandle(in);
                problems.push_back(mod->id + ": " + f.name + " doesn't fit (archives are limited to 4 GiB)");
                continue;
            }
            out.copy(in, bytes, buffer);
            CloseHandle(in);
            placed.push_back({f.name, offset, bytes});
        }
    }
    if (previous != INVALID_HANDLE_VALUE) CloseHandle(previous);

    std::string tail = writer.finish();
    out.write(tail.data(), tail.size());
    out.write_header(writer.header());
    CloseHandle(file);
    bool ok = out.ok() && swap_in(g_assets, path);
    if (!out.ok()) DeleteFileW(path.c_str());
    if (ok) {
        g_mounted_entries = std::move(entries);
        char text[160];
        snprintf(text, sizeof(text), "%zu file%s from %zu mod%s (%zu overridden by a later mod), %.1f MB in %lu ms",
                 writer.file_count() - 1, writer.file_count() == 2 ? "" : "s", mods.size(), mods.size() == 1 ? "" : "s",
                 overridden, static_cast<double>(out.pos()) / (1024.0 * 1024.0), GetTickCount() - started);
        summary = text;
    } else {
        summary = out.ok() ? "the game couldn't open the archive" : "couldn't write " + narrow(path);
    }
    // In the summary rather than logged here: at startup the log isn't open yet.
    for (const std::string& p : problems) summary += "; " + p;
    return ok;
}

void log_probe() {
    char name[MAX_PATH];
    DWORD len = GetEnvironmentVariableA("TOME_DEBUG_ASSET", name, sizeof(name));
    if (len == 0 || len >= sizeof(name)) return;
    std::string data;
    if (!storm::init() || !storm::read_file(name, data)) {
        TOME_INFO("Asset probe %s: not found", name);
        return;
    }
    TOME_INFO("Asset probe %s: %zu bytes, fnv1a %08lx", name, data.size(),
              static_cast<unsigned long>(fnv1a32(data)));
}

} // namespace tome::overlay
