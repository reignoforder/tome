// SPDX-License-Identifier: GPL-3.0-or-later
#include "frames.h"

namespace tome::ui {

int32_t FrameTable::remember(uintptr_t frame, std::string_view name, int32_t context) {
    if (!frame) return 0;
    auto it = ids_.find(frame);
    if (it != ids_.end()) {
        FrameRecord& record = records_[static_cast<size_t>(it->second - 1)];
        record.name = std::string(name);
        record.context = context;
        return it->second;
    }
    records_.push_back({frame, std::string(name), context});
    int32_t id = static_cast<int32_t>(records_.size());
    ids_[frame] = id;
    return id;
}

const FrameRecord* FrameTable::by_id(int32_t id) const {
    if (id <= 0 || static_cast<size_t>(id) > records_.size()) return nullptr;
    return &records_[static_cast<size_t>(id - 1)];
}

const FrameRecord* FrameTable::by_frame(uintptr_t frame) const {
    auto it = ids_.find(frame);
    return it == ids_.end() ? nullptr : by_id(it->second);
}

bool is_valid_mod_fdf_path(std::string_view path) {
    constexpr std::string_view kExtension = ".fdf";
    if (path.size() <= kExtension.size()) return false;
    std::string_view ext = path.substr(path.size() - kExtension.size());
    for (size_t i = 0; i < ext.size(); ++i) {
        char c = ext[i] >= 'A' && ext[i] <= 'Z' ? static_cast<char>(ext[i] - 'A' + 'a') : ext[i];
        if (c != kExtension[i]) return false;
    }
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find_first_of("/\\", start);
        if (end == std::string_view::npos) end = path.size();
        std::string_view part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        for (char c : part) {
            if (c == ':' || static_cast<unsigned char>(c) < 0x20) return false;
        }
        start = end + 1;
    }
    return true;
}

std::string mod_fdf_archive_path(std::string_view mod_id, std::string_view path) {
    std::string out = "UI\\FrameDef\\Tome\\";
    out += mod_id;
    out += '\\';
    for (char c : path) out += c == '/' ? '\\' : c;
    return out;
}

std::string toc_for(std::string_view archive_path) {
    return std::string(archive_path) + "\r\n\r\n";
}

} // namespace tome::ui
