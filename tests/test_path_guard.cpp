// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/path_guard.h"
#include "test.h"

#include <string>

using tome::path_guard::is_unsafe;

TEST(path_guard_allows_plain_relative_names) {
    // These are what PreloadGenEnd / InitGameCache are meant to write.
    CHECK(!is_unsafe("run.txt"));
    CHECK(!is_unsafe("MyCampaign.w3v"));
    CHECK(!is_unsafe("Logs\\run.txt"));
    CHECK(!is_unsafe("Logs/run.txt"));
    CHECK(!is_unsafe("a\\b\\c\\deep.txt"));
    CHECK(!is_unsafe("war3map.preload"));
    // A dot in the middle, or a leading dot, isn't traversal.
    CHECK(!is_unsafe("my.file.name.txt"));
    CHECK(!is_unsafe(".hidden"));
    CHECK(!is_unsafe("..foo"));   // not a ".." component
    CHECK(!is_unsafe("foo.."));   // trailing dots on a real stem
}

TEST(path_guard_rejects_empty) {
    CHECK(is_unsafe(""));
}

TEST(path_guard_rejects_overlong_names) {
    using tome::path_guard::kMaxPathLen;
    // MAX_PATH less its NUL: the longest name the game's MAX_PATH buffers keep
    // whole. A name up to it is fine; a longer one, which the game would
    // truncate, is refused.
    CHECK(kMaxPathLen == 259);
    CHECK(!is_unsafe(std::string(kMaxPathLen, 'a')));
    CHECK(!is_unsafe(std::string(kMaxPathLen - 4, 'a') + ".txt"));
    CHECK(is_unsafe(std::string(kMaxPathLen + 1, 'a')));
    CHECK(is_unsafe(std::string(5000, 'a') + ".txt"));
    // A long name built only of otherwise-safe components is still refused,
    // e.g. one whose truncation would end in a ".." component.
    CHECK(is_unsafe("Logs\\" + std::string(kMaxPathLen, 'b')));
    CHECK(is_unsafe(std::string(kMaxPathLen - 3, 'a') + "\\..x"));
}

TEST(path_guard_max_name_after_a_prefix) {
    using tome::path_guard::kMaxPathLen;
    using tome::path_guard::max_name_after;
    // PreloadGenEnd: "<game folder>\\CustomMapData\\" + name + maybe ".pld"
    // must stay under 260, or the game leaves its buffers unterminated.
    CHECK(max_name_after(41) == 214);  // the e2e game: Z:\tmp\tmp.XXXXXXXXXX\game\CustomMapData\ (41)
    CHECK(41 + max_name_after(41) + 4 == kMaxPathLen);
    CHECK(max_name_after(0) == kMaxPathLen - 4);
    CHECK(max_name_after(254) == 1);
    CHECK(max_name_after(255) == 0);  // no name fits
    CHECK(max_name_after(1000) == 0);
}

TEST(path_guard_any_length_allows_long_data) {
    using tome::path_guard::is_unsafe_any_length;
    using tome::path_guard::kMaxPathLen;
    // Preload's argument: a long save/load payload is fine...
    CHECK(!is_unsafe_any_length(std::string(5000, 'a')));
    CHECK(!is_unsafe_any_length("x"));
    // ...but everything else is still checked, in the whole string...
    CHECK(is_unsafe_any_length(""));
    CHECK(is_unsafe_any_length(std::string(5000, 'a') + "\\..\\x"));
    CHECK(is_unsafe_any_length("C" + std::string(5000, 'a') + ":"));
    CHECK(is_unsafe_any_length("\\" + std::string(5000, 'a')));
    // ...and in what a truncating copy would keep: "..x" cut to "..",
    // "CONSOLE" cut to "CON".
    CHECK(is_unsafe_any_length(std::string(kMaxPathLen - 3, 'a') + "\\..x" + std::string(100, 'b')));
    CHECK(is_unsafe_any_length(std::string(kMaxPathLen - 4, 'a') + "\\CONSOLE"));
    CHECK(!is_unsafe_any_length(std::string(kMaxPathLen - 5, 'a') + "\\CONSOLE"));
}

TEST(path_guard_rejects_parent_traversal) {
    CHECK(is_unsafe(".."));
    CHECK(is_unsafe("../x"));
    CHECK(is_unsafe("..\\x"));
    CHECK(is_unsafe("a/../b"));
    CHECK(is_unsafe("a\\..\\..\\b"));
    CHECK(is_unsafe("Logs/../../startup.bat"));
    CHECK(is_unsafe("a/b/.."));
}

TEST(path_guard_rejects_rooted_paths) {
    CHECK(is_unsafe("/etc/passwd"));
    CHECK(is_unsafe("\\Windows\\System32\\x"));
    CHECK(is_unsafe("\\\\host\\share\\x"));  // UNC
    CHECK(is_unsafe("/"));
    CHECK(is_unsafe("\\"));
}

TEST(path_guard_rejects_drive_and_streams) {
    CHECK(is_unsafe("C:\\Windows\\x"));
    CHECK(is_unsafe("c:relative"));        // drive-relative
    CHECK(is_unsafe("file.txt:stream"));   // NTFS alternate data stream
    CHECK(is_unsafe("::$DATA"));
}

TEST(path_guard_rejects_reserved_devices) {
    CHECK(is_unsafe("NUL"));
    CHECK(is_unsafe("nul"));
    CHECK(is_unsafe("CON"));
    CHECK(is_unsafe("aux"));
    CHECK(is_unsafe("prn"));
    CHECK(is_unsafe("COM1"));
    CHECK(is_unsafe("lpt9"));
    // A reserved stem keeps its meaning with an extension or trailing junk.
    CHECK(is_unsafe("nul.txt"));
    CHECK(is_unsafe("CON.log"));
    CHECK(is_unsafe("nul "));
    CHECK(is_unsafe("nul."));
    CHECK(is_unsafe("nul .txt"));
    // A reserved name as one component of a path still resolves to the device.
    CHECK(is_unsafe("Logs\\nul"));
    CHECK(is_unsafe("CONIN$"));
    CHECK(is_unsafe("conout$.txt"));
    // Superscript digits name the port too: ANSI (Latin-1) and UTF-8 forms.
    CHECK(is_unsafe("COM\xB9"));
    CHECK(is_unsafe("lpt\xC2\xB2.txt"));
    CHECK(!is_unsafe("com\xC2\xB4"));  // acute accent, not a superscript
    // Not reserved: longer names, or a digit out of range.
    CHECK(!is_unsafe("console.txt"));
    CHECK(!is_unsafe("com0"));
    CHECK(!is_unsafe("com10"));
    CHECK(!is_unsafe("lpt"));
    CHECK(!is_unsafe("nullable.dat"));
}

TEST(path_guard_checks_every_toc_line) {
    using tome::path_guard::toc_is_unsafe;
    CHECK(!toc_is_unsafe(""));
    CHECK(!toc_is_unsafe("UI\\FrameDef\\Tome.fdf\r\nUI\\FrameDef\\Other.fdf\r\n\r\n"));
    CHECK(!toc_is_unsafe("  ui/a.fdf  \n\n"));
    CHECK(toc_is_unsafe("UI\\a.fdf\r\n..\\..\\secret.fdf\r\n"));
    CHECK(toc_is_unsafe("C:\\x.fdf"));
    CHECK(toc_is_unsafe("a.fdf\nnul"));
}

// lan-map-guard (patches/lan_map_guard.cpp) applies is_unsafe to the map
// path a game's host sends: a host's real paths pass, a lookup outside Maps
// or of a device doesn't.
TEST(path_guard_host_map_paths) {
    CHECK(!is_unsafe("Maps\\FrozenThrone\\(2)EchoIsles.w3x"));
    CHECK(!is_unsafe("Maps\\Download\\DotA v6.83d.w3x"));
    CHECK(!is_unsafe("Maps/Scenario/(4)Map.w3m"));
    CHECK(is_unsafe("..\\..\\Users\\me\\x.w3x"));
    CHECK(is_unsafe("Maps\\..\\..\\x.w3x"));
    CHECK(is_unsafe("Maps\\com1.x.w3x"));  // the game compares only up to the last dot
    CHECK(is_unsafe("Maps\\conin$.w3x"));
    CHECK(is_unsafe("\\\\host\\share\\x.w3x"));
}
