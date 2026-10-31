#include "hash_cache.h"

#include "json.h"

#include <cinttypes>
#include <cstdio>

namespace tome::hash_cache {

namespace {

constexpr double kVersion = 1;

bool is_sha256(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// "size-written-created-fileid-volume", each in hex.
std::string format(const Stamp& s) {
    char text[96];
    std::snprintf(text, sizeof(text), "%" PRIx64 "-%" PRIx64 "-%" PRIx64 "-%" PRIx64 "-%" PRIx32, s.size, s.written,
                  s.created, s.file_id, s.volume);
    return text;
}

bool hex_field(std::string_view& text, uint64_t max, uint64_t& out) {
    size_t n = 0;
    out = 0;
    while (n < text.size() && text[n] != '-') {
        const char c = text[n];
        unsigned digit = 0;
        if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
        else return false;
        if (out > (max >> 4)) return false;
        out = (out << 4) | digit;
        ++n;
    }
    if (n == 0 || n > 16) return false;
    text.remove_prefix(n);
    return true;
}

bool parse_stamp(std::string_view text, Stamp& s) {
    uint64_t* fields[] = {&s.size, &s.written, &s.created, &s.file_id};
    for (uint64_t* f : fields) {
        if (!hex_field(text, UINT64_MAX, *f) || text.empty() || text[0] != '-') return false;
        text.remove_prefix(1);
    }
    uint64_t volume = 0;
    if (!hex_field(text, UINT32_MAX, volume) || !text.empty()) return false;
    s.volume = static_cast<uint32_t>(volume);
    return true;
}

} // namespace

bool Cache::parse(std::string_view text) {
    entries_.clear();
    folders_.clear();
    dirty_ = false;
    json::Value root;
    if (!json::parse(text, root).ok || !root.is_object()) return false;
    const json::Value* version = root.find("version");
    const json::Value* files = root.find("files");
    if (!version || !version->is_number() || version->as_number() != kVersion || !files || !files->is_object())
        return false;
    for (const auto& [path, entry] : files->members()) {
        if (entries_.size() >= kMaxEntries) break;
        const json::Value* stamp = entry.find("stamp");
        const json::Value* sha = entry.find("sha256");
        Entry e;
        if (path.empty() || !stamp || !stamp->is_string() || !sha || !sha->is_string() ||
            !parse_stamp(stamp->as_string(), e.stamp) || !is_sha256(sha->as_string())) {
            continue;
        }
        e.sha256 = sha->as_string();
        entries_[path] = std::move(e);
    }
    const json::Value* folders = root.find("folders");
    if (folders && folders->is_object()) {
        for (const auto& [dir, entry] : folders->members()) {
            if (folders_.size() >= kMaxFolders) break;
            const json::Value* print = entry.find("print");
            const json::Value* sha = entry.find("sha256");
            Folder f;
            std::string_view print_text = print && print->is_string() ? std::string_view(print->as_string()) : "";
            if (dir.empty() || print_text.empty() || !hex_field(print_text, UINT64_MAX, f.print) ||
                !print_text.empty() || !sha || !sha->is_string() || !is_sha256(sha->as_string())) {
                continue;
            }
            f.sha256 = sha->as_string();
            folders_[dir] = std::move(f);
        }
    }
    return true;
}

std::string Cache::serialize() const {
    json::Value files = json::Value::object();
    for (const auto& [path, e] : entries_) {
        if (!e.used) continue;
        json::Value entry = json::Value::object();
        entry.set("stamp", json::Value::string(format(e.stamp)));
        entry.set("sha256", json::Value::string(e.sha256));
        files.set(path, std::move(entry));
    }
    json::Value folders = json::Value::object();
    for (const auto& [dir, f] : folders_) {
        if (!f.used) continue;
        char print[17];
        std::snprintf(print, sizeof(print), "%" PRIx64, f.print);
        json::Value entry = json::Value::object();
        entry.set("print", json::Value::string(print));
        entry.set("sha256", json::Value::string(f.sha256));
        folders.set(dir, std::move(entry));
    }
    json::Value root = json::Value::object();
    root.set("version", json::Value::number(kVersion));
    root.set("files", std::move(files));
    root.set("folders", std::move(folders));
    return json::serialize(root);
}

const std::string* Cache::find(const std::string& path, const Stamp& stamp) {
    auto it = entries_.find(path);
    if (it == entries_.end() || !(it->second.stamp == stamp)) return nullptr;
    it->second.used = true;
    return &it->second.sha256;
}

void Cache::put(const std::string& path, const Stamp& stamp, std::string sha256) {
    if (!is_sha256(sha256)) return;
    auto it = entries_.find(path);
    if (it == entries_.end() && entries_.size() >= kMaxEntries) {
        std::erase_if(entries_, [](const auto& entry) { return !entry.second.used; });
        if (entries_.size() >= kMaxEntries) return;
    }
    Entry& e = entries_[path];
    e.stamp = stamp;
    e.sha256 = std::move(sha256);
    e.used = true;
    dirty_ = true;
}

const std::string* Cache::find_folder(const std::string& dir, uint64_t print) {
    auto it = folders_.find(dir);
    if (it == folders_.end() || it->second.print != print) return nullptr;
    it->second.used = true;
    const std::string prefix = dir + "\\";
    for (auto e = entries_.lower_bound(prefix); e != entries_.end() && e->first.starts_with(prefix); ++e) {
        e->second.used = true;
    }
    return &it->second.sha256;
}

void Cache::put_folder(const std::string& dir, uint64_t print, std::string sha256) {
    if (!is_sha256(sha256)) return;
    auto it = folders_.find(dir);
    if (it == folders_.end() && folders_.size() >= kMaxFolders) {
        std::erase_if(folders_, [](const auto& f) { return !f.second.used; });
        if (folders_.size() >= kMaxFolders) return;
    }
    Folder& f = folders_[dir];
    f.print = print;
    f.sha256 = std::move(sha256);
    f.used = true;
    dirty_ = true;
}

bool Cache::dirty() const {
    if (dirty_) return true;
    for (const auto& [path, e] : entries_) {
        if (!e.used) return true;  // dropped from the next file
    }
    for (const auto& [dir, f] : folders_) {
        if (!f.used) return true;
    }
    return false;
}

void Cache::mark_saved() {
    std::erase_if(entries_, [](const auto& entry) { return !entry.second.used; });
    std::erase_if(folders_, [](const auto& f) { return !f.second.used; });
    dirty_ = false;
}

} // namespace tome::hash_cache
