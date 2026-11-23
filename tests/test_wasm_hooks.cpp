// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/embedded_mappings.h"
#include "core/mappings.h"
#include "core/wasm_hooks.h"
#include "test.h"

#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace tome;
using namespace tome::wasm;

namespace {

bool ok(const char* signature, HookPoint& point, const char* name = "CUnit::SetHeroStr") {
    std::string why;
    return hook_point(name, signature, point, why);
}

std::string why_not(const char* signature, const char* name = "CUnit::SetHeroStr") {
    HookPoint point;
    std::string why;
    return hook_point(name, signature, point, why) ? std::string() : why;
}

} // namespace

TEST(wasm_hooks_numbers_and_objects_qualify) {
    HookPoint p;
    CHECK(ok("void(CUnit* this, float amount)", p));
    CHECK_EQ(p.args.size(), size_t(2));
    CHECK(p.args[0] == HookArg::Unit);
    CHECK(p.args[1] == HookArg::Float);
    CHECK(p.result == HookResult::None);
    CHECK_EQ(hook_block_size(p), 16u);

    CHECK(ok("int(CItem* this, CPlayerWar3* owner, uint32_t id, unsigned int flags, int32_t n, unsigned u)", p));
    CHECK(p.args[0] == HookArg::Item);
    CHECK(p.args[1] == HookArg::Player);
    for (size_t i = 2; i < 6; ++i) CHECK(p.args[i] == HookArg::Int);  // CUnit::SetHeroStr: a writable one
    CHECK(p.result == HookResult::Int);

    CHECK(ok("float(CUnit *this)", p));
    CHECK(p.result == HookResult::Float);
    CHECK(ok("uint32_t(void)", p));
    CHECK(p.args.empty());
    CHECK(ok("void()", p));
    CHECK(ok("void(const CUnit* unit)", p));
    CHECK(p.args[0] == HookArg::Unit);
    CHECK(ok("void(int, float)", p));  // unnamed
    CHECK_EQ(p.args.size(), size_t(2));
}

TEST(wasm_hooks_everything_else_is_refused_with_a_reason) {
    CHECK(why_not("void(CUnit* this, const char* text)").find("const char*") != std::string::npos);
    CHECK(why_not("void(void* this)").find("void*") != std::string::npos);
    CHECK(why_not("void(CFrame* this, int x)").find("CFrame*") != std::string::npos);
    CHECK(why_not("CUnit*(CUnit* this)").find("result") != std::string::npos);
    CHECK(why_not("double(int x)").find("result") != std::string::npos);
    CHECK(why_not("void(double x)").find("double") != std::string::npos);
    CHECK(why_not("void(int64_t x)").find("int64_t") != std::string::npos);
    CHECK(why_not("void(const char* format, ...)").find("const char*") != std::string::npos);
    CHECK(why_not("void(int x, ...)").find("variadic") != std::string::npos);
    CHECK(why_not("no parentheses").find("result(params)") != std::string::npos);
    CHECK(why_not("void(int, , int)") != "");
    CHECK(why_not("") != "");
    std::string many = "void(";
    for (int i = 0; i < 17; ++i) many += (i ? ", int" : "int");
    many += ")";
    CHECK(why_not(many.c_str()).find("17") != std::string::npos || why_not(many.c_str()).find("more than") != std::string::npos);
}

// An int argument may be an index or a table key: only the functions on the
// list can have theirs changed; the rest are read-only (Index), floats always
// writable. The game's scripting, CRT, crash handling and start/end are refused.
TEST(wasm_hooks_ints_are_read_only_unless_listed_and_risky_functions_are_refused) {
    HookPoint p;
    CHECK(ok("void(CPlayerWar3* this, int state, int value)", p, "CPlayerWar3::SetState"));
    CHECK(p.args[0] == HookArg::Player);
    CHECK(p.args[1] == HookArg::Index);
    CHECK(p.args[2] == HookArg::Index);
    CHECK(ok("void(int screen, int force)", p, "CGlueMgr::SetScreen"));
    CHECK(p.args[0] == HookArg::Index);
    CHECK(ok("void(float x, int n)", p, "DDCToNDCWidth"));
    CHECK(p.args[0] == HookArg::Float);
    CHECK(p.args[1] == HookArg::Index);
    CHECK(ok("void(CUnit* this, int value, int permanent)", p, "CUnit::SetHeroAgi"));
    CHECK(p.args[1] == HookArg::Int);
    CHECK(p.args[2] == HookArg::Int);

    for (const char* name : {"Native.TriggerAddAction", "Native.Condition", "__report_gsfailure",
                             "CrashHandler::Remove", "GameShutdown", "InitNatives"}) {
        CHECK(why_not("void(void)", name) != "");
        CHECK(why_not("uint32_t(uint32_t trigger, uint32_t code)", name) != "");
    }
    CHECK(why_not("void(void)", "Natives.Other") == "");  // only the "Native." prefix
}

// tome_wasm_game.h (tools/gen-game-header.py, in Python) lists the functions
// this rule (in C++) accepts: the ones every mapping file that has them gives
// the same signature, and hook_point accepts.
TEST(wasm_hooks_the_generated_header_lists_what_the_loader_accepts) {
    std::set<std::string> expected;
    std::map<std::string, HookPoint> points;
    std::map<std::string, std::set<std::string>> signatures;
    for (size_t i = 0; i < mappings::kEmbeddedFileCount; ++i) {
        json::Value root;
        mappings::Build build;
        if (!json::parse(mappings::kEmbeddedFiles[i].text, root).ok || !mappings::parse_build(root, build).ok) continue;
        for (const mappings::Symbol& s : build.symbols) {
            if (s.kind == mappings::Kind::Function) signatures[s.name].insert(s.signature);
        }
    }
    for (const auto& [name, sigs] : signatures) {
        HookPoint point;
        std::string why;
        if (sigs.size() == 1 && hook_point(name, *sigs.begin(), point, why)) {
            expected.insert(name);
            points[name] = point;
        }
    }
    std::ifstream in(TOME_TEST_WASM_GAME_HEADER);
    std::stringstream text;
    text << in.rdbuf();
    std::set<std::string> listed;
    std::string line;
    std::map<std::string, std::string> name_of;  // TOME_FN_<mangled> -> name
    std::string current;
    size_t arg = 0;
    while (std::getline(text, line)) {
        if (line.rfind("#define TOME_FN_", 0) == 0) {
            const size_t quote = line.find('"');
            const std::string name = line.substr(quote + 1, line.rfind('"') - quote - 1);
            listed.insert(name);
            name_of[line.substr(8, line.find(' ', 8) - 8)] = name;
        } else if (line.rfind("typedef struct tome_wasm_hook_", 0) == 0) {
            current = line.substr(30, line.find(' ', 30) - 30);
            arg = 0;
        } else if (!current.empty() && line.rfind("    ", 0) == 0 && line.find("result;") == std::string::npos &&
                   line.find("flags;") == std::string::npos) {
            // An argument: `const` marks the read-only ints, the objects and floats never carry it.
            const std::string name = name_of["TOME_FN_" + current];
            const auto it = points.find(name);
            if (it != points.end() && arg < it->second.args.size()) {
                const bool readonly = line.rfind("    const ", 0) == 0;
                if (readonly != (it->second.args[arg] == HookArg::Index))
                    test::fail(__FILE__, __LINE__, name + ": argument " + std::to_string(arg) + " is not as read-only in tome_wasm_game.h as the loader makes it");
            }
            ++arg;
        } else if (line.rfind("}", 0) == 0) {
            current.clear();
        }
    }
    CHECK(!listed.empty());
    for (const std::string& name : expected) {
        if (!listed.count(name)) test::fail(__FILE__, __LINE__, name + " qualifies but tome_wasm_game.h lacks it");
    }
    for (const std::string& name : listed) {
        if (!expected.count(name)) test::fail(__FILE__, __LINE__, name + " is in tome_wasm_game.h but doesn't qualify");
    }
}
