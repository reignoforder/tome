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
