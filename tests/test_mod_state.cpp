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

Entry approved(const std::string& hash, const std::string& folder, const std::string& key = {}) {
    Entry e;
    e.approved = {hash, folder, key};
    return e;
}

// Two real Ed25519 public keys (not of small order).
const std::string kKeyA = "11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo=";
const std::string kKeyB = "A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg=";

bool contains(const char* text, const char* part) {
    return std::string(text).find(part) != std::string::npos;
}

} // namespace

TEST(mod_state_schema_1_is_legacy) {
    int schema = -1;
    State state = parse(parsed(R"({"schema": 1, "mods": {"hello": {"enabled": false}}})"), schema);
    CHECK_EQ(schema, 1);
    CHECK_EQ(state.size(), 1u);
    CHECK(!state["hello"].enabled);
    CHECK(state["hello"].approved.hash.empty());

    parse(parsed(R"({"mods": {}})"), schema);
    CHECK_EQ(schema, 0);
    parse(parsed(R"({"schema": "3", "mods": {}})"), schema);
    CHECK_EQ(schema, 0);
    parse(parsed(R"({"schema": 1e300, "mods": {}})"), schema);
    CHECK_EQ(schema, kSchema);
    parse(parsed(R"({"schema": -5, "mods": {}})"), schema);
    CHECK_EQ(schema, 0);
}

TEST(mod_state_round_trip) {
    State state;
    state["hello"] = approved(kHashA, "hello.tome");
    state["off"].enabled = false;
    int schema = 0;
    State back = parse(serialize(state), schema);
    CHECK_EQ(schema, kSchema);
    CHECK_EQ(back.size(), 2u);
    CHECK(back["hello"].enabled);
    CHECK_EQ(back["hello"].approved.hash, kHashA);
    CHECK_EQ(back["hello"].approved.folder, std::string("hello.tome"));
    CHECK(!back["off"].enabled);
    CHECK(back["off"].approved.hash.empty());
    CHECK(!serialize(state).find("mods")->find("off")->find("approved"));
}

TEST(mod_state_malformed_hash_allows_nothing) {
    int schema = 0;
    State state = parse(parsed(R"({"schema": 2, "mods": {
        "short": {"enabled": true, "approved": "abc", "approved_from": "short"},
        "upper": {"enabled": true, "approved": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"},
        "number": {"enabled": true, "approved": 5},
        "junk": 3}})"),
                        schema);
    CHECK_EQ(schema, 2);
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

TEST(mod_state_signed_updates) {
    Entry e = approved(kHashA, "hello.tome", kKeyA);
    // The same key: an update from the same author runs.
    CHECK(need(&e, true, true, "hello.tome", kHashB, kKeyA) == Need::None);
    // Another key, or none: asks, louder than for unsigned code.
    CHECK(need(&e, true, true, "hello.tome", kHashB, kKeyB) == Need::SignerChanged);
    CHECK(need(&e, true, true, "hello.tome", kHashB, "") == Need::SignerChanged);
    CHECK(need(&e, true, true, "hello.tome", "", kKeyA) == Need::Changed);  // unreadable: never
    // The very code that was allowed runs whoever signed it.
    CHECK(need(&e, true, true, "hello.tome", kHashA, "") == Need::None);
    // Allowed unsigned: a signature changes nothing.
    Entry plain = approved(kHashA, "hello.tome");
    CHECK(need(&plain, true, true, "hello.tome", kHashB, kKeyA) == Need::Changed);
    // Never allowed: a signature allows nothing.
    CHECK(need(nullptr, true, true, "hello.tome", kHashA, kKeyA) == Need::New);
}

TEST(mod_state_keys_round_trip) {
    State state;
    state["signed"] = approved(kHashA, "signed.tome", kKeyA);
    state["plain"] = approved(kHashB, "plain");
    int schema = 0;
    State back = parse(serialize(state), schema);
    CHECK_EQ(back["signed"].approved.key, kKeyA);
    CHECK(back["plain"].approved.key.empty());
    CHECK(!serialize(state).find("mods")->find("plain")->find("approved_key"));

    // A malformed or weak key: as if it wasn't signed.
    State weak = parse(parsed(R"({"schema": 3, "mods": {
        "short": {"approved": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "approved_key": "AAAA"},
        "zero": {"approved": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                 "approved_key": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="}}})"),
                       schema);
    CHECK(weak["short"].approved.key.empty());
    CHECK(weak["zero"].approved.key.empty());
    CHECK_EQ(weak["zero"].approved.hash, kHashA);
}

TEST(mod_state_describe) {
    CHECK_EQ(std::string(describe(Need::None, CodeKind::Native)), std::string(""));
    CHECK(contains(describe(Need::New, CodeKind::Native), "native code"));
    CHECK(contains(describe(Need::New, CodeKind::Scripts), "game scripts"));
    CHECK(contains(describe(Need::New, CodeKind::Assets), "game files"));
    CHECK(contains(describe(Need::New, CodeKind::Wasm), "sandboxed code"));
    CHECK(contains(describe(Need::Changed, CodeKind::Wasm), "code changed"));
    CHECK_EQ(std::string(kind_name(CodeKind::Wasm)), std::string("wasm"));
    CHECK(contains(describe(Need::Changed, CodeKind::Native), "code changed"));
    CHECK(contains(describe(Need::Changed, CodeKind::Scripts), "code changed"));
    CHECK(contains(describe(Need::Changed, CodeKind::Assets), "files changed"));
    CHECK(contains(describe(Need::SignerChanged, CodeKind::Native), "no longer signed"));
    CHECK_EQ(std::string(kind_name(CodeKind::Native)), std::string("native"));
    CHECK_EQ(std::string(kind_name(CodeKind::Scripts)), std::string("scripts"));
    CHECK_EQ(std::string(kind_name(CodeKind::Assets)), std::string("assets"));
    CHECK_EQ(std::string(kind_name(CodeKind::None)), std::string(""));
}

TEST(mod_state_code_files) {
    // Schema 3: every file but the top tome.mod.json.
    CHECK(is_code_file("hello.dll"));
    CHECK(is_code_file("assets/UI/x.blp"));
    CHECK(is_code_file("icon.tga"));
    CHECK(is_code_file("config.schema.json"));
    CHECK(!is_code_file("tome.mod.json"));
    CHECK(!is_code_file("Tome.Mod.Json"));
    CHECK(!is_code_file("tome.sig"));  // it signs the hash
    CHECK(is_code_file("sub/tome.sig"));
    CHECK(is_code_file("sub/tome.mod.json"));
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
