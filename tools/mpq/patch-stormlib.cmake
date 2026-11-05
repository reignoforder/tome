# SPDX-License-Identifier: GPL-3.0-or-later
# Tome's patches to StormLib 9.30, run in StormLib's source folder: cmake -P
# patch-stormlib.cmake. Each replaces one exact piece of text, and fails if
# it isn't there (a different StormLib); a file already patched is left as
# it is.

function(tome_patch file old new)
    file(READ "${file}" text)
    string(FIND "${text}" "${new}" done)
    if(NOT done EQUAL -1)
        return()
    endif()
    string(FIND "${text}" "${old}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "StormLib patch: ${file} doesn't have the text Tome patches")
    endif()
    string(REPLACE "${old}" "${new}" text "${text}")
    file(WRITE "${file}" "${text}")
endfunction()

# 1. "%u[" is three TCHARs: 6 bytes only in a wide-character build. In
# Tome's (char) build the memcpy read 2 bytes past the 4-byte literal
# (-Wstringop-overread).
tome_patch(src/SMemUtf8.cpp
"memcpy(szBuffer + nOutLength, _T(\"%u[\"), 6);"
"memcpy(szBuffer + nOutLength, _T(\"%u[\"), 3 * sizeof(TCHAR));")

# 2. Tokens after #endif (-Wendif-labels).
tome_patch(src/zlib/inflate.h
"#endif __INFLATE_H_"
"#endif /* __INFLATE_H_ */")
