// SPDX-License-Identifier: GPL-3.0-or-later
// tome-mpq: list, extract and replace files in MPQ archives (maps).
//
//   tome-mpq list    <archive>
//   tome-mpq extract <archive> <name-in-archive> <output-file>
//   tome-mpq put     <archive> <name-in-archive> <input-file>   (modifies the archive)
//
// A Windows program (it runs under Wine on Linux), because the rest of the
// toolchain cross-compiles too.
#include <StormLib.h>

#include <cstdio>
#include <cstring>

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: tome-mpq list <archive>\n"
                 "       tome-mpq extract <archive> <name> <output-file>\n"
                 "       tome-mpq put <archive> <name> <input-file>\n");
    return 2;
}

int fail(const char* what, const char* detail) {
    std::fprintf(stderr, "tome-mpq: %s %s (error %u)\n", what, detail, static_cast<unsigned>(GetLastError()));
    return 1;
}

int list(HANDLE mpq) {
    SFILE_FIND_DATA fd;
    HANDLE find = SFileFindFirstFile(mpq, "*", &fd, nullptr);
    if (!find) return fail("can't enumerate files in", "archive");
    do {
        std::printf("%10u  %s\n", static_cast<unsigned>(fd.dwFileSize), fd.cFileName);
    } while (SFileFindNextFile(find, &fd));
    SFileFindClose(find);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const char* command = argv[1];
    const char* archive = argv[2];

    bool writing = std::strcmp(command, "put") == 0;
    HANDLE mpq = nullptr;
    if (!SFileOpenArchive(archive, 0, writing ? 0 : STREAM_FLAG_READ_ONLY, &mpq)) {
        return fail("can't open archive", archive);
    }

    int rc = 0;
    if (std::strcmp(command, "list") == 0 && argc == 3) {
        rc = list(mpq);
    } else if (std::strcmp(command, "extract") == 0 && argc == 5) {
        if (!SFileExtractFile(mpq, argv[3], argv[4], SFILE_OPEN_FROM_MPQ)) rc = fail("can't extract", argv[3]);
    } else if (writing && argc == 5) {
        // Melee maps have small hash tables; make room if needed.
        DWORD max_files = SFileGetMaxFileCount(mpq);
        SFileSetMaxFileCount(mpq, max_files < 64 ? 64 : max_files);
        if (!SFileAddFileEx(mpq, argv[4], argv[3], MPQ_FILE_COMPRESS | MPQ_FILE_REPLACEEXISTING,
                            MPQ_COMPRESSION_ZLIB, MPQ_COMPRESSION_NEXT_SAME)) {
            rc = fail("can't add", argv[3]);
        } else if (!SFileFlushArchive(mpq)) {
            rc = fail("can't write", archive);
        }
    } else {
        rc = usage();
    }
    SFileCloseArchive(mpq);
    return rc;
}
