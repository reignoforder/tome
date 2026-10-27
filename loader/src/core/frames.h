#pragma once

// Portable parts of the frame API for mods (loader/src/frames.cpp): the table
// that gives JASS scripts integer frame ids, and the paths a mod's FDF files
// get inside the archive Tome builds for them.

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tome::ui {

// 1.31's FRAMEPOINT numbering, which CLayoutFrame::SetPoint uses too.
constexpr int32_t kFramePointCount = 9;  // TOPLEFT 0 ... CENTER 4 ... BOTTOMRIGHT 8

// Every frame Tome has handed out, with the name and create context it's
// registered under. A frame is only used while the game still finds that
// same frame under that name, so a stale id (its screen was destroyed) or a
// made-up one does nothing instead of crashing.
struct FrameRecord {
    uintptr_t frame = 0;
    std::string name;
    int32_t context = 0;
};

class FrameTable {
public:
    // The frame's id (1, 2, ...), added if it's new. 0 for a null frame.
    // A known frame address with a different name (memory reused by a new
    // frame) keeps its id and gets the new name.
    int32_t remember(uintptr_t frame, std::string_view name, int32_t context);

    const FrameRecord* by_id(int32_t id) const;
    const FrameRecord* by_frame(uintptr_t frame) const;
    size_t size() const { return records_.size(); }

private:
    std::vector<FrameRecord> records_;
    std::unordered_map<uintptr_t, int32_t> ids_;
};

// A path a mod passes to load_fdf: relative to the mod's folder, with no
// "..", drive letter or empty component, and ending in ".fdf".
bool is_valid_mod_fdf_path(std::string_view path);

// Where a mod's FDF goes in its archive, e.g. ("hello", "ui/hello.fdf") ->
// "UI\FrameDef\Tome\hello\ui\hello.fdf". The mod id keeps mods apart.
std::string mod_fdf_archive_path(std::string_view mod_id, std::string_view path);

// A TOC listing one file, in the game's format (CRLF, blank line at the end).
std::string toc_for(std::string_view archive_path);

} // namespace tome::ui
