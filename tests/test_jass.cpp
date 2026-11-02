// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/jass.h"
#include "test.h"

using namespace tome::jass;

namespace {

std::string sig(const char* text) {
    NativeDecl decl;
    std::string err;
    if (!parse_native(text, decl, err)) {
        test::fail(__FILE__, __LINE__, std::string("parse failed: ") + err);
        return {};
    }
    return decl.signature();
}

bool rejects(const char* text) {
    NativeDecl decl;
    std::string err;
    return !parse_native(text, decl, err) && !err.empty();
}

} // namespace

TEST(jass_code_mask) {
    NativeDecl decl;
    std::string error;
    CHECK(parse_native("native A takes nothing returns nothing", decl, error));
    CHECK_EQ(decl.code_mask(), 0u);
    CHECK(parse_native("native B takes code c returns boolean", decl, error));
    CHECK_EQ(decl.code_mask(), 1u);
    CHECK(parse_native("native C takes integer i, code a, string s, code b returns code", decl, error));
    CHECK_EQ(decl.code_mask(), 0xAu);
}

TEST(jass_signatures_match_common_j) {
    // Pairs taken from 1.27b's own BindNative calls.
    CHECK_EQ(sig("native Deg2Rad takes real degrees returns real"), std::string("(R)R"));
    CHECK_EQ(sig("native KillUnit takes unit whichUnit returns nothing"), std::string("(Hunit;)V"));
    CHECK_EQ(sig("native GetTriggerUnit takes nothing returns unit"), std::string("()Hunit;"));
    CHECK_EQ(sig("native CreateUnit takes player id, integer unitid, real x, real y, real face returns unit"),
             std::string("(Hplayer;IRRR)Hunit;"));
    CHECK_EQ(sig("native TriggerAddAction takes trigger whichTrigger, code actionFunc returns triggeraction"),
             std::string("(Htrigger;C)Htriggeraction;"));
    CHECK_EQ(sig("native S2I takes string s returns integer"), std::string("(S)I"));
    CHECK_EQ(sig("native IsUnitType takes unit whichUnit, unittype whichUnitType returns boolean"),
             std::string("(Hunit;Hunittype;)B"));
}

TEST(jass_parses_constant_and_whitespace) {
    NativeDecl decl;
    std::string err;
    CHECK(parse_native("  constant native   GetPlayerId takes player whichPlayer\treturns integer ", decl, err));
    CHECK(decl.is_constant);
    CHECK_EQ(decl.name, std::string("GetPlayerId"));
    CHECK_EQ(decl.param_types.size(), size_t(1));
    CHECK(parse_native("native A takes integer a,integer b returns nothing", decl, err));
    CHECK_EQ(decl.signature(), std::string("(II)V"));
}

TEST(jass_rejects_malformed_declarations) {
    CHECK(rejects(""));
    CHECK(rejects("function Foo takes nothing returns nothing"));
    CHECK(rejects("native takes nothing returns nothing"));
    CHECK(rejects("native Foo nothing returns nothing"));
    CHECK(rejects("native Foo takes integer returns nothing"));
    CHECK(rejects("native Foo takes integer a, returns nothing"));
    CHECK(rejects("native Foo takes nothing a returns nothing"));
    CHECK(rejects("native Foo takes nothing returns"));
    CHECK(rejects("native Foo takes nothing returns nothing extra"));
    CHECK(rejects("native 9Foo takes nothing returns nothing"));
    CHECK(rejects("native Foo_ takes nothing returns nothing"));
    CHECK(rejects("native Foo takes integer set returns nothing"));
}

TEST(jass_scan_shadow_exploit) {
    // The memhack pattern: a local shadows a global of a different type.
    const char* evil =
        "globals\n"
        "    unit G = null\n"
        "    integer K\n"
        "endglobals\n"
        "function f takes nothing returns nothing\n"
        "    local integer G\n"          // shadows unit global G -> finding
        "    set G = 5\n"
        "endfunction\n";
    std::vector<ShadowFinding> f;
    CHECK(scan_shadow_exploit(evil, f));
    CHECK_EQ(f.size(), (size_t)1);
    CHECK_EQ(f[0].name, std::string("G"));
    CHECK_EQ(f[0].global_type, std::string("unit"));
    CHECK_EQ(f[0].local_type, std::string("integer"));
    CHECK_EQ(f[0].line, 6);

    // Benign: a local shadowing a global of the SAME type is not flagged,
    // nor is a local whose name is not a global, nor comments/strings.
    const char* ok =
        "globals\n"
        "    unit G\n"
        "    integer N\n"
        "endglobals\n"
        "function g takes nothing returns nothing\n"
        "    local unit G\n"             // same type: fine
        "    local real tmp\n"           // not a global: fine
        "    local string s = \"local integer G\"\n"  // string literal: ignored
        "    // local integer N\n"       // comment: ignored
        "endfunction\n";
    std::vector<ShadowFinding> f2;
    CHECK(!scan_shadow_exploit(ok, f2));
    CHECK_EQ(f2.size(), (size_t)0);

    // constant and array globals are parsed; a mismatched array local is caught.
    const char* arr =
        "globals\n"
        "    constant integer A = 1\n"
        "    real array B\n"
        "endglobals\n"
        "function h takes nothing returns nothing\n"
        "    local unit B\n"             // shadows real-array global B
        "endfunction\n";
    std::vector<ShadowFinding> f3;
    CHECK(scan_shadow_exploit(arr, f3));
    CHECK_EQ(f3.size(), (size_t)1);
    CHECK_EQ(f3[0].name, std::string("B"));

    // Globals from the prelude scripts (Blizzard.j) count; their own locals
    // aren't reported, and lines are the map script's.
    const char* blizzard =
        "globals\n"
        "    unit bj_lastCreatedUnit = null\n"
        "endglobals\n"
        "function Prelude takes nothing returns nothing\n"
        "    local integer udg_x\n"
        "endfunction\n";
    const char* map =
        "globals\n"
        "    unit udg_x = null\n"
        "endglobals\n"
        "function m takes nothing returns nothing\n"
        "    local integer bj_lastCreatedUnit\n"
        "endfunction\n";
    std::vector<ShadowFinding> f4;
    CHECK(!scan_shadow_exploit(map, f4));
    CHECK(scan_shadow_exploit(map, f4, {blizzard}));
    CHECK_EQ(f4.size(), (size_t)1);
    CHECK_EQ(f4[0].name, std::string("bj_lastCreatedUnit"));
    CHECK_EQ(f4[0].global_type, std::string("unit"));
    CHECK_EQ(f4[0].line, 5);
}
