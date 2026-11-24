// SPDX-License-Identifier: GPL-3.0-or-later
#include "path_guard.h"

#include "text.h"

#include <cstring>

namespace tome::path_guard {
namespace {

bool is_sep(char c) { return c == '/' || c == '\\'; }

using text::iequals;

// What Windows keeps of a name: it drops trailing dots and spaces.
std::string_view drop_trailing_dots_and_spaces(std::string_view name) {
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.remove_suffix(1);
    return name;
}

// The stem of a path component is what Windows uses to resolve a device name:
// the text before the first '.', with trailing spaces and dots ignored (the
// filesystem strips them, so "nul." and "nul " are still the NUL device).
bool is_reserved_device(std::string_view comp) {
    comp = drop_trailing_dots_and_spaces(comp);
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

// "..", or a component Windows may trim to it: only dots and spaces, with
// at least two dots (".. ", ". .", "..."). Windows drops trailing dots and
// spaces from a name, and how it treats such a component in the middle of a
// path isn't something to rely on; no map needs one.
bool is_parent(std::string_view comp) {
    size_t dots = 0;
    for (char c : comp) {
        if (c == '.') {
            ++dots;
        } else if (c != ' ') {
            return false;
        }
    }
    return dots >= 2;
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
        if (is_parent(comp)) return true;
        if (is_reserved_device(comp)) return true;
        i = (j < path.size()) ? j + 1 : j;
    }
    return false;
}

// Extensions is_executable_name refuses, lowercase, without the dot.
constexpr std::string_view kExecutable[] = {
    // Programs and libraries.
    "exe", "com", "scr", "pif", "dll", "cpl", "ocx", "sys", "drv", "msi", "msp", "msc", "jar",
    // Scripts and things Windows runs or applies when opened.
    "bat", "cmd", "vbs", "vbe", "js", "jse", "wsf", "wsh", "wsc", "ps1", "psm1", "psd1", "hta", "reg",
    "inf", "scf", "lnk", "url", "appref-ms", "settingcontent-ms", "vb", "ws", "chm", "hlp", "mst", "ps1xml",
    "psc1", "application", "gadget", "appx", "msix", "library-ms", "search-ms", "diagcab", "xll",
    // The Miles sound library's plugins (it loads these from the game's folders),
    // and a DirectShow filter (the game loads its blizzard.ax as code).
    "mix", "asi", "flt", "m3d", "ax",
};

} // namespace

bool is_executable_name(std::string_view path) {
    std::string_view name;
    for (;;) {
        while (!path.empty() && is_sep(path.back())) path.remove_suffix(1);
        size_t start = path.size();
        while (start > 0 && !is_sep(path[start - 1])) --start;
        name = drop_trailing_dots_and_spaces(path.substr(start));
        // A last component of only dots and spaces names no file of its own:
        // Windows drops a "." component, so "x.exe\\." is x.exe. The one
        // before it counts.
        if (!name.empty() || start == 0) break;
        path = path.substr(0, start);
    }
    const size_t dot = name.rfind('.');
    if (dot == std::string_view::npos) return false;
    const std::string_view ext = name.substr(dot + 1);
    for (std::string_view e : kExecutable) {
        if (iequals(ext, e)) return true;
    }
    // The 8.3 short name Windows gives a name that isn't one (on a drive that
    // makes them) has the first three characters of its extension, spaces
    // dropped and characters 8.3 doesn't allow made '_': "x.exe1" is also
    // X~1.EXE, "x.mixer" X~1.MIX (and "*.mix" finds it, short names too).
    char short_ext[3];
    size_t n = 0;
    for (size_t i = 0; i < ext.size() && n < sizeof short_ext; ++i) {
        const unsigned char c = static_cast<unsigned char>(ext[i]);
        if (c == ' ') continue;
        const bool allowed = c > 0x20 && c < 0x80 && !std::strchr("\"*+,/:;<=>?[]|\\", c);
        short_ext[n++] = allowed ? static_cast<char>(c) : '_';
    }
    const std::string_view short_view(short_ext, n);
    if (iequals(short_view, ext)) return false;  // already 8.3: checked above
    for (std::string_view e : kExecutable) {
        if (iequals(short_view, e)) return true;
    }
    return false;
}

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
