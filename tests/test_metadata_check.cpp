// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/metadata_check.h"
#include "test.h"

#include <cstring>
#include <string>

using namespace tome;

namespace {

ResolveEnvironment env_1_27b(Edition edition = Edition::FrozenThrone) {
    ResolveEnvironment env;
    parse_semver("0.4.0", env.tome_version);
    parse_game_version("1.27b", env.game_version);
    env.edition = edition;
    return env;
}

// A build that maps one function, CUnit::Foo.
mappings::Build test_build() {
    json::Value root;
    const char* text = R"j({"schema": 2, "name": "test", "file_version": "1.27.1.7085",
        "pe_timestamp": "0x584a499e", "size_of_image": "0x1000",
        "sha256": "d51e5680243fc90e19c9d6074f7fac433c466d3cf5f46e2364291725574d8236",
        "symbols": {"CUnit::Foo": {"kind": "function", "rva": "0x100", "conv": "thiscall", "bytes": "55 8b ec 83 ec",
                                   "sig": "void(CUnit* this)"}}})j";
    if (!json::parse(text, root).ok) test::fail(__FILE__, __LINE__, "bad JSON in test");
    mappings::Build build;
    auto r = mappings::parse_build(root, build);
    if (!r.ok) test::fail(__FILE__, __LINE__, "bad mapping in test: " + (r.errors.empty() ? "" : r.errors[0]));
    return build;
}

std::string meta(const std::string& extra) {
    return R"({"schema": 1, "id": "candidate", "version": "1.2.0", "sync": "client")" +
           (extra.empty() ? "" : ", " + extra) + "}";
}

} // namespace

TEST(metadata_check_runs_here) {
    mappings::Build build = test_build();
    MetadataCheck c = check_metadata_text(meta(R"("game": ">=1.26a <1.28", "tome": "^0.4", "requires": ["CUnit::Foo"])"),
                                          env_1_27b(), &build);
    CHECK_EQ(c.result, 1);
    CHECK_EQ(c.reason, std::string());
    // A mod's own names ("<id>/<name>") wait for that mod's mapping file; dependencies aren't checked.
    c = check_metadata_text(meta(R"("game": "*", "tome": "*", "requires": ["lib/CUnit::Bar"],
                                    "depends": {"not-installed": "^9"})"),
                            env_1_27b(), &build);
    CHECK_EQ(c.result, 1);
}

TEST(metadata_check_not_here) {
    mappings::Build build = test_build();
    MetadataCheck c = check_metadata_text(meta(R"("game": "1.26a", "tome": "*")"), env_1_27b(), &build);
    CHECK_EQ(c.result, 0);
    CHECK_EQ(c.reason, std::string("supports game 1.26a, this is 1.27b"));
    c = check_metadata_text(meta(R"("game": "*", "tome": ">=0.5")"), env_1_27b(), &build);
    CHECK_EQ(c.result, 0);
    CHECK_EQ(c.reason, std::string("needs Tome >=0.5, this is 0.4.0"));
    c = check_metadata_text(meta(R"("game": "*", "tome": "*", "edition": "roc")"), env_1_27b(), &build);
    CHECK_EQ(c.result, 0);
    CHECK(c.reason.find("is for ") == 0);
    // Every problem, not just the first.
    c = check_metadata_text(meta(R"("game": "1.26a", "tome": "*", "requires": ["CUnit::Missing", "CUnit::Foo"])"),
                            env_1_27b(), &build);
    CHECK_EQ(c.result, 0);
    CHECK_EQ(c.reason, std::string("supports game 1.26a, this is 1.27b; needs \"CUnit::Missing\", which isn't "
                                   "mapped for test"));
    // An unknown build checks no "requires" names, as discovery (which runs such mods anyway).
    c = check_metadata_text(meta(R"("game": "*", "tome": "*", "requires": ["CUnit::Foo"])"), env_1_27b(), nullptr);
    CHECK_EQ(c.result, 1);
    CHECK_EQ(c.reason, std::string(""));
    // Every game, edition and Tome problem, not only the first.
    c = check_metadata_text(meta(R"("game": "1.26a", "tome": ">=99.0", "edition": "roc")"), env_1_27b(), &build);
    CHECK_EQ(c.result, 0);
    CHECK(c.reason.find("supports game 1.26a") == 0);
    CHECK(c.reason.find("; is for ") != std::string::npos);
    CHECK(c.reason.find("; needs Tome >=99.0") != std::string::npos);
    // A DLL where mods' DLLs don't run (a Shade without them), as discovery refuses it; a Wasm module runs.
    c = check_metadata_text(meta(R"("game": "*", "tome": "*", "entry": "mod.dll")"), env_1_27b(), &build, false);
    CHECK_EQ(c.result, 0);
    CHECK_EQ(c.reason, std::string(kShadeRefusesDll));
    c = check_metadata_text(meta(R"("game": "*", "tome": "*", "entry": {"1.26a": "old.dll"})"), env_1_27b(), &build,
                            false);
    CHECK_EQ(c.result, 1);  // no DLL for this game: assets only
    c = check_metadata_text(meta(R"("game": "*", "tome": "*", "wasm": "mod.wasm")"), env_1_27b(), &build, false);
    CHECK_EQ(c.result, 1);
    c = check_metadata_text(meta(R"("game": "*", "tome": "*", "entry": "mod.dll")"), env_1_27b(), &build);
    CHECK_EQ(c.result, 1);
}

TEST(metadata_check_invalid) {
    MetadataCheck c = check_metadata_text("{not json", env_1_27b(), nullptr);
    CHECK_EQ(c.result, -1);
    CHECK(c.reason.find("tome.mod.json: ") == 0);
    c = check_metadata_text(R"({"schema": 1, "id": "candidate", "version": "1.2.0"})", env_1_27b(), nullptr);
    CHECK_EQ(c.result, -1);
    CHECK(c.reason.find("missing required field") != std::string::npos);
    c = check_metadata_text("", env_1_27b(), nullptr);
    CHECK_EQ(c.result, -1);
    CHECK(!c.reason.empty());
}

TEST(metadata_check_copy_text) {
    char out[8];
    copy_text("short", out, sizeof(out));
    CHECK_EQ(std::string(out), std::string("short"));
    copy_text("exactly", out, sizeof(out));  // 7 bytes and the NUL
    CHECK_EQ(std::string(out), std::string("exactly"));
    copy_text("too long here", out, sizeof(out));
    CHECK_EQ(std::string(out), std::string("too lon"));
    // "abcde" then "é" (2 bytes): the cut falls inside it, so it goes.
    copy_text("abcdef\xC3\xA9", out, 8);
    CHECK_EQ(std::string(out), std::string("abcdef"));
    copy_text("abcde\xC3\xA9x", out, 8);
    CHECK_EQ(std::string(out), std::string("abcde\xC3\xA9"));
    char one[1] = {'x'};
    copy_text("anything", one, 1);
    CHECK_EQ(one[0], '\0');
    copy_text("ignored", nullptr, 0);
    copy_text("ignored", out, 0);
}
