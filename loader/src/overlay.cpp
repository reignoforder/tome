#include "overlay.h"

#include "core/assets.h"
#include "core/hash.h"
#include "core/json.h"
#include "log.h"
#include "storm.h"
#include "unwind.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
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

// The last assets archive Tome built and what's in it (tome/cache/assets.json),
// so the next start can mount it again instead of copying every file anew.
constexpr wchar_t kManifest[] = L"assets.json";
constexpr int kManifestSchema = 1;
struct Manifest {
    std::wstring file;  // name in tome/cache
    std::vector<std::pair<std::string, uint64_t>> mods;  // id, assets fingerprint; lowest precedence first
    std::map<std::string, std::vector<Entry>> entries;
};
Manifest g_kept;  // from the last session, until the first mount_assets uses or deletes it

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
    // Writes `header` over the placeholder at the start; pos() stays the
    // archive's size (the manifest records it, and the next start checks it).
    void write_header(const std::string& header) {
        LARGE_INTEGER zero{};
        if (!SetFilePointerEx(file_, zero, nullptr, FILE_BEGIN)) ok_ = false;
        const uint64_t end = pos_;
        write(header.data(), header.size());
        pos_ = end;
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

std::string to_hex(uint64_t value) {
    char text[17];
    snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}

// The manifest, if it's valid and its archive is still there with the size it had.
bool read_manifest(Manifest& out) {
    std::string text;
    if (!read_file(g_cache + L"\\" + kManifest, text)) return false;
    json::Value root;
    if (!json::parse(text, root).ok) return false;
    const json::Value* schema = root.find("schema");
    const json::Value* file = root.find("archive");
    const json::Value* size = root.find("size");
    const json::Value* mods = root.find("mods");
    if (!schema || !schema->is_number() || schema->as_number() != kManifestSchema || !file || !file->is_string() ||
        !size || !size->is_number() || !mods || !mods->is_array()) {
        return false;
    }
    Manifest m;
    m.file = widen(file->as_string());
    if (m.file.empty() || m.file.find_first_of(L"\\/:") != std::wstring::npos) return false;
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    if (!GetFileAttributesExW((g_cache + L"\\" + m.file).c_str(), GetFileExInfoStandard, &attributes) ||
        attributes.nFileSizeHigh != 0 || attributes.nFileSizeLow != size->as_number()) {
        return false;
    }
    for (const json::Value& mod : mods->items()) {
        const json::Value* id = mod.find("id");
        const json::Value* fingerprint = mod.find("fingerprint");
        const json::Value* files = mod.find("files");
        if (!id || !id->is_string() || !fingerprint || !fingerprint->is_string() || !files || !files->is_array()) {
            return false;
        }
        m.mods.emplace_back(id->as_string(), std::strtoull(fingerprint->as_string().c_str(), nullptr, 16));
        std::vector<Entry>& entries = m.entries[id->as_string()];
        for (const json::Value& f : files->items()) {
            if (!f.is_array() || f.items().size() != 3 || !f.items()[0].is_string() || !f.items()[1].is_number() ||
                !f.items()[2].is_number()) {
                return false;
            }
            entries.push_back({f.items()[0].as_string(), static_cast<uint32_t>(f.items()[1].as_number()),
                               static_cast<uint32_t>(f.items()[2].as_number())});
        }
    }
    out = std::move(m);
    return true;
}

void write_manifest(const std::wstring& path, uint64_t size, const std::vector<ModSource>& mods) {
    json::Value root = json::Value::object();
    root.set("schema", json::Value::number(kManifestSchema));
    root.set("archive", json::Value::string(narrow(path.substr(path.rfind(L'\\') + 1))));
    root.set("size", json::Value::number(static_cast<double>(size)));
    json::Value list = json::Value::array();
    for (const ModSource& mod : mods) {
        json::Value entry = json::Value::object();
        entry.set("id", json::Value::string(mod.id));
        entry.set("fingerprint", json::Value::string(to_hex(mod.fingerprint)));
        json::Value files = json::Value::array();
        auto placed = g_mounted_entries.find(mod.id);
        if (placed != g_mounted_entries.end()) {
            for (const Entry& e : placed->second) {
                json::Value f = json::Value::array();
                f.push(json::Value::string(e.name));
                f.push(json::Value::number(e.offset));
                f.push(json::Value::number(e.size));
                files.push(std::move(f));
            }
        }
        entry.set("files", std::move(files));
        list.push(std::move(entry));
    }
    root.set("mods", std::move(list));
    if (!write_file_atomic(g_cache + L"\\" + kManifest, json::serialize(root))) {
        DeleteFileW((g_cache + L"\\" + kManifest).c_str());
    }
}

// The archive kept from the last session isn't the one mounted: delete it.
void drop_kept() {
    if (g_kept.file.empty()) return;
    DeleteFileW((g_cache + L"\\" + g_kept.file).c_str());
    g_kept = {};
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
    if (!read_manifest(g_kept)) {
        g_kept = {};
        DeleteFileW((g_cache + L"\\" + kManifest).c_str());
    }
    // New archives get higher numbers than the one kept.
    unsigned kept_number = 0;
    if (swscanf(g_kept.file.c_str(), L"assets-%u.mpq", &kept_number) == 1) g_counter = kept_number;
    // Archives in use by another game instance fail to delete and stay.
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((g_cache + L"\\*.mpq").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (_wcsicmp(fd.cFileName, g_kept.file.c_str()) == 0) continue;
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
        drop_kept();
        DeleteFileW((g_cache + L"\\" + kManifest).c_str());
        g_mounted_entries.clear();
        summary = "no mod assets mounted";
        return true;
    }
    if (!storm::init()) {
        summary = "Storm functions not found";
        return false;
    }

    // The first mount of the session, with the same mods and files as the
    // archive kept from the last one: mount that one again.
    if (!g_kept.file.empty()) {
        bool same = !g_assets.archive && g_kept.mods.size() == mods.size();
        for (size_t i = 0; same && i < mods.size(); ++i) {
            same = !mods[i].keep_previous && mods[i].fingerprint && g_kept.mods[i].first == mods[i].id &&
                   g_kept.mods[i].second == mods[i].fingerprint;
        }
        if (same && swap_in(g_assets, g_cache + L"\\" + g_kept.file)) {
            size_t files = 0;
            for (const auto& [id, entries] : g_kept.entries) files += entries.size();
            g_mounted_entries = std::move(g_kept.entries);
            g_kept = {};
            char text[120];
            snprintf(text, sizeof(text), "%zu file%s from %zu mod%s, unchanged since the last start", files,
                     files == 1 ? "" : "s", mods.size(), mods.size() == 1 ? "" : "s");
            summary = text;
            return true;
        }
        drop_kept();  // swap_in deletes it as well if the game couldn't open it
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
        // Only an archive built from the mods' folders with every file in it
        // is worth mounting again: files copied from the previous archive
        // (keep_previous) may lack some, under a fingerprint that still
        // matches the folder's (the previous build may have skipped a file).
        const bool from_folders =
            std::none_of(mods.begin(), mods.end(), [](const ModSource& m) { return m.keep_previous; });
        if (problems.empty() && from_folders) {
            write_manifest(path, out.pos(), mods);
        } else {
            DeleteFileW((g_cache + L"\\" + kManifest).c_str());
        }
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
    if (!TOME_DEV) return;
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
