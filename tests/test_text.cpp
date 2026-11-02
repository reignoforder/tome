// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/text.h"
#include "test.h"

using tome::text::iequals;

TEST(text_iequals_ignores_ascii_case) {
    CHECK(iequals("Hotkey", "HOTKEY"));
    CHECK(iequals("", ""));
    CHECK(iequals(L"WorldEdit.exe", L"worldedit.exe"));
    CHECK(!iequals("con", "conin$"));
    CHECK(!iequals("abc", "abd"));
    CHECK(!iequals(L"a", L"ab"));
    // Only ASCII letters fold: '[' (0x5B) is not 'z' + 1 lowered, and
    // non-ASCII bytes compare as they are.
    CHECK(!iequals("[", "{"));
    CHECK(!iequals("\xC9", "\xE9"));
}
