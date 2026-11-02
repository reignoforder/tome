// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Path sanitizer for file-writing JASS natives (docs/known-bugs.md,
// "PreloadGenEnd file writes"). A downloaded map calls natives like
// PreloadGenEnd(string filename) and InitGameCache(string campaignFile) that
// create a file from a map-controlled name. On 1.27b the write is rooted in
// the game folder, so a traversing or absolute name lets the map plant a file
// anywhere the process can write (a startup script, a config) -- a persistence
// / code-execution primitive. This decides which names to refuse.
//
// Portable and pure so it can be unit-tested (tests/test_path_guard.cpp); the
// patch that calls it lives in loader/src/patches/path_sanitizer.cpp.

#include <string_view>

namespace tome::path_guard {

// True if `path`, a filename argument to a file-writing native, tries to leave
// the game's data sandbox and must be refused:
//   - empty (no target),
//   - contains ':' anywhere (a drive letter like "C:" or an NTFS alternate
//     data stream like "file:stream"),
//   - rooted: a leading '/' or '\' (absolute or drive-relative, incl. UNC),
//   - any ".." path component (parent-directory traversal),
//   - a reserved DOS device-name component (CON, PRN, AUX, NUL, CONIN$,
//     CONOUT$, COM1-9, LPT1-9, and COM/LPT with a superscript 1-3), which
//     names a device rather than a file under the sandbox.
// Both '/' and '\' count as separators. A plain relative name, with or without
// subdirectories (e.g. "Logs\\run.txt"), is safe: that is these natives'
// intended use and stays inside the data folder.
bool is_unsafe(std::string_view path);

// True if a TOC file's text lists a file that is_unsafe refuses. A TOC names
// one FDF file per line (blank lines and surrounding spaces ignored), and the
// game loads each, so a map's TOC is checked like the path it was loaded by.
bool toc_is_unsafe(std::string_view toc);

} // namespace tome::path_guard
