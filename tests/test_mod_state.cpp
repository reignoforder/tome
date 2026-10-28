#include "core/mod_state.h"
#include "test.h"

using namespace tome;
using namespace tome::mod_state;

namespace {

const std::string kHashA(64, 'a');
const std::string kHashB(64, 'b');

json::Value parsed(const char* text) {
    json::Value root;
    json::ParseResult result = json::parse(text, root);
    CHECK(result.ok);
    return root;
}

Entry approved(const std::string& hash, const std::string& folder) {
    Entry e;
    e.approved = {hash, folder};
    return e;
}

bool contains(const char* text, const char* part) {
    return std::string(text).find(part) != std::string::npos;
}

} // namespace

TEST(mod_state_schema_1_is_legacy) {
    bool legacy = false;
    State state = parse(parsed(R"({"schema": 1, "mods": {"hello": {"enabled": false}}})"), legacy);
    CHECK(legacy);
    CHECK_EQ(state.size(), 1u);
    CHECK(!state["hello"].enabled);
    CHECK(state["hello"].approved.hash.empty());

    parse(parsed(R"({"mods": {}})"), legacy);
    CHECK(legacy);
}

TEST(mod_state_round_trip) {
    State state;
    state["hello"] = approved(kHashA, "hello.tome");
    state["off"].enabled = false;
    bool legacy = true;
    State back = parse(serialize(state), legacy);
    CHECK(!legacy);
    CHECK_EQ(back.size(), 2u);
    CHECK(back["hello"].enabled);
    CHECK_EQ(back["hello"].approved.hash, kHashA);
    CHECK_EQ(back["hello"].approved.folder, std::string("hello.tome"));
    CHECK(!back["off"].enabled);
    CHECK(back["off"].approved.hash.empty());
    CHECK(!serialize(state).find("mods")->find("off")->find("approved"));
}

TEST(mod_state_malformed_hash_allows_nothing) {
    bool legacy = false;
    State state = parse(parsed(R"({"schema": 2, "mods": {
        "short": {"enabled": true, "approved": "abc", "approved_from": "short"},
        "upper": {"enabled": true, "approved": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"},
        "number": {"enabled": true, "approved": 5},
        "junk": 3}})"),
                        legacy);
    CHECK(!legacy);
    CHECK(state["short"].approved.hash.empty());
    CHECK(state["short"].approved.folder.empty());
    CHECK(state["upper"].approved.hash.empty());
    CHECK(state["number"].approved.hash.empty());
    CHECK(!state.count("junk"));
}

TEST(mod_state_mods_without_code_need_nothing) {
    CHECK(need(nullptr, false, false, "assets-only", "") == Need::None);
    CHECK(need(nullptr, false, true, "assets.tome", "") == Need::None);
}

TEST(mod_state_new_code_needs_approval) {
    CHECK(need(nullptr, true, false, "hello", kHashA) == Need::New);
    CHECK(need(nullptr, true, true, "hello.tome", kHashA) == Need::New);
    Entry on;  // enabled, but never allowed (a mod installed with this feature)
    CHECK(need(&on, true, false, "hello", kHashA) == Need::New);
}

TEST(mod_state_folder_mod_hot_reloads_in_its_folder) {
    Entry e = approved(kHashA, "hello");
    CHECK(need(&e, true, false, "hello", kHashA) == Need::None);
    CHECK(need(&e, true, false, "hello", kHashB) == Need::None);  // rebuilt
    CHECK(need(&e, true, false, "hello", "") == Need::None);
    // Another folder with the same id: only the same code.
    CHECK(need(&e, true, false, "hello-copy", kHashA) == Need::None);
    CHECK(need(&e, true, false, "hello-copy", kHashB) == Need::Changed);
}

TEST(mod_state_package_needs_the_same_code) {
    Entry e = approved(kHashA, "hello.tome");
    CHECK(need(&e, true, true, "hello.tome", kHashA) == Need::None);
    CHECK(need(&e, true, true, "hello.tome", kHashB) == Need::Changed);
    CHECK(need(&e, true, true, "hello.tome", "") == Need::Changed);  // unreadable
    // Another package file with the same id: only the same code.
    CHECK(need(&e, true, true, "other.tome", kHashA) == Need::None);
    CHECK(need(&e, true, true, "other.tome", kHashB) == Need::Changed);
    // A folder replacing an allowed package: only the same code.
    CHECK(need(&e, true, false, "hello", kHashB) == Need::Changed);
    CHECK(need(&e, true, false, "hello", kHashA) == Need::None);
}

TEST(mod_state_describe) {
    CHECK_EQ(std::string(describe(Need::None, true)), std::string(""));
    CHECK(contains(describe(Need::New, true), "native code"));
    CHECK(contains(describe(Need::New, false), "game scripts"));
    CHECK(contains(describe(Need::Changed, true), "changed"));
    CHECK(contains(describe(Need::Changed, false), "changed"));
}

TEST(mod_state_code_files) {
    CHECK(is_code_file("hello.dll", "assets"));
    CHECK(is_code_file("lib/helper.dll", "assets"));
    CHECK(is_code_file("config.json", "assets"));
    CHECK(is_code_file("payload.bin", "assets"));
    CHECK(!is_code_file("tome.mod.json", "assets"));
    CHECK(!is_code_file("TOME.MOD.JSON", "assets"));
    CHECK(is_code_file("sub/tome.mod.json", "assets"));  // only the top one is metadata
    CHECK(!is_code_file("assets/UI/x.blp", "assets"));
    CHECK(is_code_file("assets/Scripts/Blizzard.j", "assets"));  // scripts among the assets are code
    CHECK(is_code_file("Assets/AI/human.AI", "assets"));
    CHECK(!is_code_file("Assets/ui/x.blp", "assets"));
    CHECK(is_code_file("assets2/x.dll", "assets"));  // not inside the assets folder
    CHECK(is_code_file("assets", "assets"));         // a file, not the folder
    CHECK(!is_code_file("data/art/x.blp", "data\\art\\"));
    CHECK(is_code_file("data/x.dll", "data/art"));
}

TEST(mod_state_scripts) {
    CHECK(is_script("Scripts\\Blizzard.j"));
    CHECK(is_script("Scripts/common.J"));
    CHECK(is_script("war3map.j"));
    CHECK(is_script("Scripts\\human.ai"));
    CHECK(is_script("x.pld"));
    CHECK(is_script(".j"));
    CHECK(!is_script("Scripts\\Blizzard.j.bak"));
    CHECK(!is_script("UI\\x.blp"));
    CHECK(!is_script("Units\\UnitData.slk"));
    CHECK(!is_script("noextension"));
    CHECK(!is_script("dir.j\\file"));
}

TEST(mod_state_manifest_sorted_like_sha256sum) {
    std::vector<CodeFile> files{{"lib/b.dll", kHashB}, {"hello.dll", kHashA}};
    CHECK_EQ(manifest(files), kHashA + "  hello.dll\n" + kHashB + "  lib/b.dll\n");
    CHECK_EQ(manifest({}), std::string());
}

TEST(mod_state_debug_approves) {
    CHECK(debug_approves("*", "hello"));
    CHECK(debug_approves("hello", "hello"));
    CHECK(debug_approves("a, hello ,b", "hello"));
    CHECK(!debug_approves("", "hello"));
    CHECK(!debug_approves("hell,hello2", "hello"));
    CHECK(!debug_approves(",,", "hello"));
    CHECK(!debug_approves("hello", ""));
}
