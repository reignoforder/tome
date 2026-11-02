// SPDX-License-Identifier: GPL-3.0-or-later
#include "path_guard.h"

#include "text.h"

namespace tome::path_guard {
namespace {

bool is_sep(char c) { return c == '/' || c == '\\'; }

using text::iequals;

// The stem of a path component is what Windows uses to resolve a device name:
// the text before the first '.', with trailing spaces and dots ignored (the
// filesystem strips them, so "nul." and "nul " are still the NUL device).
bool is_reserved_device(std::string_view comp) {
    // Drop trailing spaces and dots.
    while (!comp.empty() && (comp.back() == ' ' || comp.back() == '.')) comp.remove_suffix(1);
    // Stem: up to the first '.'.
    if (size_t dot = comp.find('.'); dot != std::string_view::npos) comp = comp.substr(0, dot);
    // Spaces before the extension are ignored too: "nul .txt" is NUL.
    while (!comp.empty() && comp.back() == ' ') comp.remove_suffix(1);
    if (iequals(comp, "con") || iequals(comp, "prn") || iequals(comp, "aux") || iequals(comp, "nul") ||
        iequals(comp, "conin$") || iequals(comp, "conout$")) {
        return true;
    }
    // COM1-9 / LPT1-9: three letters plus one digit 1-9, or a superscript
    // 1-3, which Windows also resolves to the port: as one ANSI byte (Latin-1
    // B9/B2/B3) or as its UTF-8 form (C2 then the same byte).
    if (comp.size() < 4) return false;
    std::string_view head = comp.substr(0, 3);
    if (!iequals(head, "com") && !iequals(head, "lpt")) return false;
    std::string_view tail = comp.substr(3);
    if (tail.size() == 2 && static_cast<unsigned char>(tail[0]) == 0xC2) tail.remove_prefix(1);
    if (tail.size() != 1) return false;
    auto d = static_cast<unsigned char>(tail[0]);
    return (d >= '1' && d <= '9') || d == 0xB9 || d == 0xB2 || d == 0xB3;
}

// Every check but the length.
bool escapes(std::string_view path) {
    if (path.empty()) return true;
    // A drive letter ("C:...") or an NTFS alternate data stream ("x:y") is the
    // only legitimate use of ':' in a Windows path, and both escape the
    // sandbox: refuse any ':'.
    if (path.find(':') != std::string_view::npos) return true;
    // Rooted: absolute, drive-relative, or UNC ("\\host\share").
    if (is_sep(path.front())) return true;

    for (size_t i = 0; i < path.size();) {
        size_t j = i;
        while (j < path.size() && !is_sep(path[j])) ++j;
        std::string_view comp = path.substr(i, j - i);
        if (comp == "..") return true;
        if (is_reserved_device(comp)) return true;
        i = (j < path.size()) ? j + 1 : j;
    }
    return false;
}

} // namespace

size_t max_name_after(size_t prefix_len) {
    constexpr size_t kExt = 4;  // ".pld"
    return prefix_len + kExt < kMaxPathLen ? kMaxPathLen - kExt - prefix_len : 0;
}

bool is_unsafe(std::string_view path) { return path.size() > kMaxPathLen || escapes(path); }

bool is_unsafe_any_length(std::string_view path) {
    return escapes(path) || (path.size() > kMaxPathLen && escapes(path.substr(0, kMaxPathLen)));
}

bool toc_is_unsafe(std::string_view toc) {
    while (!toc.empty()) {
        size_t end = toc.find_first_of("\r\n");
        std::string_view line = toc.substr(0, end);
        toc.remove_prefix(end == std::string_view::npos ? toc.size() : end + 1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        if (!line.empty() && is_unsafe(line)) return true;
    }
    return false;
}

} // namespace tome::path_guard
