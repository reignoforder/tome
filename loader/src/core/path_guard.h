// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Path sanitizer for file-writing JASS natives (docs/known-bugs.md,
// "PreloadGenEnd file writes"). A downloaded map calls natives like
// PreloadGenEnd(string filename) and InitGameCache(string campaignFile) that
// create a file from a map-controlled name. On 1.27b the write lands inside
// the game folder (PreloadGenEnd: CustomMapData\, saves: save\<profile>\),
// not the user-data folder, so a traversing or absolute name lets the map
// plant a file anywhere the process can write (a startup script, a config) --
// a persistence / code-execution primitive. This decides which names to
// refuse.
//
// Portable and pure so it can be unit-tested (tests/test_path_guard.cpp); the
// patch that calls it lives in loader/src/patches/path_sanitizer.cpp.

#include <cstddef>
#include <string_view>

namespace tome::path_guard {

// The longest path a native argument may be before is_unsafe refuses it:
// MAX_PATH (260) less its terminating NUL. The game copies a name into
// MAX_PATH buffers with truncating copies (PreloadGenEnd: SStrCopy(buf, name,
// 260)), so a longer one would be checked whole but used cut short, and a cut
// can turn a safe component into a refused one ("..x" into "..", "CONSOLE"
// into "CON"). Refusing it keeps what is checked what the game uses. No
// legitimate name is this long: the ANSI file APIs can't open one.
// PreloadGenEnd needs a tighter limit (max_name_after).
constexpr size_t kMaxPathLen = 259;

// The longest name that fits whole after `prefix_len` characters in a
// MAX_PATH buffer that may also get a 4-character extension (".pld"), 0 if
// none does: PreloadGenEnd builds "<game folder>\\CustomMapData\\<name>" with
// strncpy into 260-byte stack buffers, which it leaves unterminated when that
// reaches 260, and then scans them as strings past their end; a name of ~4000
// characters has crashed the game that way (docs/known-bugs.md, "Over-long
// path"). Never more than kMaxPathLen.
size_t max_name_after(size_t prefix_len);

// True if `path`, a filename argument to a file-writing native, tries to leave
// the game's data sandbox and must be refused:
//   - empty (no target),
//   - longer than kMaxPathLen (the game would truncate it),
//   - contains ':' anywhere (a drive letter like "C:" or an NTFS alternate
//     data stream like "file:stream"),
//   - rooted: a leading '/' or '\' (absolute or drive-relative, incl. UNC),
//   - any ".." path component (parent-directory traversal), or one of only
//     dots and spaces with two dots or more (".. ", "...": what Windows'
//     trimming of trailing dots and spaces could turn into ".."),
//   - a reserved DOS device-name component (CON, PRN, AUX, NUL, CONIN$,
//     CONOUT$, COM1-9, LPT1-9, and COM/LPT with a superscript 1-3), which
//     names a device rather than a file under the sandbox.
// Both '/' and '\' count as separators. A plain relative name, with or without
// subdirectories (e.g. "Logs\\run.txt"), is safe: that is these natives'
// intended use and stays inside the data folder.
bool is_unsafe(std::string_view path);

// is_unsafe without the length limit, for an argument that is more than a
// path: Preload(filename) stores its string as is (no fixed buffer), and maps
// use it as a data channel, since PreloadGenEnd writes it out word for word
// (File I/O, codeless save/load), so a long one is legitimate. Refuses what
// is_unsafe refuses in the whole string or in its first kMaxPathLen bytes
// (what a truncating copy of it would keep).
bool is_unsafe_any_length(std::string_view path);

// True if `path`'s last component names a file Windows would run or open as
// a program or script, or one the game or its sound library loads as code:
// .exe, .dll, .bat, .cmd, .com, .scr, .pif, .cpl, .msi, .vbs, .js, .ps1,
// .hta, .lnk, .reg, ..., Miles' plugins (.mix, .asi, .flt, .m3d; tome.mix
// is one) and DirectShow filters (.ax, as blizzard.ax). Its extension is
// taken as Windows resolves the name: trailing dots and spaces dropped
// ("x.bat. ." is x.bat), case ignored, and so is the extension of the 8.3
// short name Windows may give it ("x.exe1" is also X~1.EXE). For the natives that write a file: a
// map has no reason to make one of these, and the game's own files (saves,
// caches, preload files) never are. Separators at the end are ignored
// ("x.exe\\" names x.exe), and so are components of only dots and spaces
// after the name ("x.exe\\." is x.exe: Windows drops a "." component). Game
// scripts (.j, .ai, .pld) aren't refused: Preloader runs a file of any name,
// and maps use those.
bool is_executable_name(std::string_view path);

// True if a TOC file's text lists a file that is_unsafe refuses. A TOC names
// one FDF file per line (blank lines and surrounding spaces ignored), and the
// game loads each, so a map's TOC is checked like the path it was loaded by.
bool toc_is_unsafe(std::string_view toc);

} // namespace tome::path_guard
