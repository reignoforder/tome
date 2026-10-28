#include "core/path_guard.h"
#include "test.h"

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
