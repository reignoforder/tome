// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/metadata.h"
#include "test.h"

using namespace tome;

namespace {

MetadataResult parse_text(const std::string& text, ModMetadata& out) {
    json::Value root;
    auto r = json::parse(text, root);
    if (!r.ok) {
        test::fail(__FILE__, __LINE__, "bad JSON in test: " + r.error);
        return {};
    }
    return parse_metadata(root, out);
}

const char* kMinimal = R"({
  "schema": 1, "id": "hp-bars", "version": "1.2.0",
  "game": ">=1.26a <1.29", "tome": ">=0.1", "sync": "client"
})";

bool has_error_containing(const MetadataResult& r, const char* text) {
    for (const std::string& e : r.errors) {
        if (e.find(text) != std::string::npos) return true;
    }
    return false;
}

} // namespace

TEST(metadata_minimal_with_defaults) {
    ModMetadata m;
    auto r = parse_text(kMinimal, m);
    CHECK(r.ok);
    CHECK(r.warnings.empty());
    CHECK_EQ(m.id, std::string("hp-bars"));
    CHECK_EQ(m.name, std::string("hp-bars"));  // defaults to id
    CHECK_EQ(m.version.to_string(), std::string("1.2.0"));
    CHECK(m.sync == SyncMode::Client);
    CHECK(m.reload == ReloadMode::Live);
    CHECK(m.entry.empty());
}

TEST(metadata_full_example) {
    ModMetadata m;
    auto r = parse_text(R"({
      "schema": 1, "id": "better-hp-bars", "version": "1.2.0", "name": "Better HP Bars",
      "description": "d", "authors": ["A", "B"], "license": "MIT", "icon": "icon.png",
      "links": {"homepage": "https://example.com"},
      "game": ">=1.26a <1.29", "tome": ">=0.3.0",
      "depends": {"tome-ui": "^1.0"}, "recommends": {"fonts": "*"},
      "conflicts": {"old": "*"}, "breaks": {"legacy": "<2.0"},
      "entry": {"1.31": "x64/mod.dll", "*": "mod.dll"},
      "assets": "assets/", "requires": ["CUnit.hp_bar"],
      "natives": ["native TomeFoo takes nothing returns nothing"],
      "provides": ["hp-bars"], "sync": "required", "reload": "map",
      "load": {"after": ["tome-ui"], "before": ["other"]},
      "config": "config.schema.json",
      "keybinds": [{"id": "toggle", "default": "Ctrl+H"}]
    })", m);
    CHECK(r.ok);
    CHECK(r.warnings.empty());
    CHECK_EQ(m.authors.size(), size_t(2));
    CHECK_EQ(m.depends.size(), size_t(1));
    CHECK_EQ(m.depends[0].id, std::string("tome-ui"));
    CHECK(m.sync == SyncMode::Required);
    CHECK(m.reload == ReloadMode::Map);
    CHECK_EQ(m.load_after.size(), size_t(1));
    CHECK_EQ(m.keybinds.size(), size_t(1));
    CHECK_EQ(m.keybinds[0].id, std::string("toggle"));
    CHECK_EQ(m.keybinds[0].label, std::string("toggle"));  // defaults to the id
    CHECK_EQ(keys::format(m.keybinds[0].key), std::string("Ctrl+H"));

    GameVersion v127b, v131;
    parse_game_version("1.27b", v127b);
    parse_game_version("1.31.1.12164", v131);
    CHECK_EQ(m.entry_for(v127b), std::string("mod.dll"));
    CHECK_EQ(m.entry_for(v131), std::string("x64/mod.dll"));
}

TEST(metadata_reports_missing_and_wrong_types) {
    ModMetadata m;
    auto r = parse_text(R"({"schema": 1, "id": 5, "version": "1.0"})", m);
    CHECK(!r.ok);
    CHECK(has_error_containing(r, "\"id\" must be a string"));
    CHECK(has_error_containing(r, "not a semantic version"));
    CHECK(has_error_containing(r, "missing required field \"game\""));
    CHECK(has_error_containing(r, "missing required field \"tome\""));
    CHECK(has_error_containing(r, "missing required field \"sync\""));
}

TEST(metadata_validates_ids) {
    CHECK(is_valid_mod_id("hello"));
    CHECK(is_valid_mod_id("hp-bars_2"));
    CHECK(!is_valid_mod_id("Hello"));
    CHECK(!is_valid_mod_id("1mod"));
    CHECK(!is_valid_mod_id("a"));
    CHECK(!is_valid_mod_id("has space"));

    ModMetadata m;
    auto r = parse_text(R"({"schema":1,"id":"tome","version":"1.0.0","game":"*","tome":"*","sync":"client"})", m);
    CHECK(has_error_containing(r, "reserved"));
}

TEST(metadata_limits_version_length) {
    ModMetadata m;
    std::string ok = "1.0.0-" + std::string(kMaxVersionLength - 6, 'a');
    auto r = parse_text(R"({"schema":1,"id":"long","version":")" + ok +
                            R"(","game":"*","tome":"*","sync":"required"})", m);
    CHECK(r.errors.empty());
    r = parse_text(R"({"schema":1,"id":"long","version":")" + ok + "a" +
                       R"(","game":"*","tome":"*","sync":"required"})", m);
    CHECK(has_error_containing(r, "longer than 64 characters"));
}

TEST(metadata_rejects_newer_schema) {
    ModMetadata m;
    auto r = parse_text(R"({"schema":2,"id":"x1","version":"1.0.0","game":"*","tome":"*","sync":"client"})", m);
    CHECK(has_error_containing(r, "newer than this Tome supports"));
}

// Found by fuzz/fuzz_json: a schema past INT_MAX was cast to int for the message.
TEST(metadata_rejects_huge_schema) {
    ModMetadata m;
    auto r = parse_text(R"({"schema":2222222222222222222222222222222222222221,"id":"x1","version":"1.0.0"})", m);
    CHECK(has_error_containing(r, "\"schema\" 2.2222222222222223e+39 is newer"));
    r = parse_text(R"({"schema":3000000000,"id":"x1","version":"1.0.0"})", m);
    CHECK(has_error_containing(r, "\"schema\" 3000000000 is newer"));
}

TEST(metadata_rejects_paths_outside_mod_folder) {
    ModMetadata m;
    auto r = parse_text(
        R"({"schema":1,"id":"x1","version":"1.0.0","game":"*","tome":"*","sync":"client","entry":"../evil.dll"})", m);
    CHECK(has_error_containing(r, "inside the mod folder"));
    r = parse_text(
        R"({"schema":1,"id":"x1","version":"1.0.0","game":"*","tome":"*","sync":"client","entry":"C:\\x.dll"})", m);
    CHECK(has_error_containing(r, "inside the mod folder"));
    r = parse_text(
        R"({"schema":1,"id":"x1","version":"1.0.0","game":"*","tome":"*","sync":"client","entry":"bin/mod.dll"})", m);
    CHECK(r.ok);
}

TEST(metadata_warns_on_unknown_fields) {
    ModMetadata m;
    auto r = parse_text(
        R"({"schema":1,"id":"x1","version":"1.0.0","game":"*","tome":"*","sync":"client","colour":"red"})", m);
    CHECK(r.ok);
    CHECK_EQ(r.warnings.size(), size_t(1));
    // The editor's pointer to the JSON Schema isn't unknown.
    r = parse_text(R"({"$schema":"../tome/sdk/schemas/tome.mod.schema.json","schema":1,"id":"x1",
                       "version":"1.0.0","game":"*","tome":"*","sync":"client"})", m);
    CHECK(r.ok);
    CHECK(r.warnings.empty());
}

TEST(metadata_rejects_bad_enums_and_self_dependency) {
    ModMetadata m;
    auto r = parse_text(R"({"schema":1,"id":"x1","version":"1.0.0","game":"*","tome":"*",
                            "sync":"sometimes","reload":"never","depends":{"x1":"*"}})", m);
    CHECK(has_error_containing(r, "\"sync\""));
    CHECK(has_error_containing(r, "\"reload\""));
    CHECK(has_error_containing(r, "depend on itself"));
}

TEST(metadata_rejects_bad_keybinds) {
    ModMetadata m;
    auto r = parse_text(R"({"schema":1,"id":"kb","version":"1.0.0","game":"*","tome":"*","sync":"client",
      "keybinds": [{"id": "a", "default": "Ctrl+Nope"}, {"id": "a"}, {"label": "no id"}]})", m);
    CHECK(!r.ok);
    CHECK(has_error_containing(r, "isn't a key"));
    CHECK(has_error_containing(r, "listed twice"));
    CHECK(has_error_containing(r, "\"id\" must be"));
}

TEST(metadata_wasm) {
    auto with = [](const char* extra) {
        return std::string(R"({"schema": 1, "id": "wasm-mod", "version": "1.0.0", "game": "*", "tome": "*", "sync": "client", )") +
               extra + "}";
    };
    ModMetadata m;
    auto r = parse_text(with(R"("wasm": "mod/w.wasm")"), m);
    for (const std::string& e : r.errors) test::fail(__FILE__, __LINE__, e);
    CHECK(r.ok);
    CHECK(r.warnings.empty());
    CHECK_EQ(m.wasm, std::string("mod/w.wasm"));
    CHECK(m.entry.empty());

    ModMetadata bad;
    CHECK(has_error_containing(parse_text(with(R"("wasm": "../w.wasm")"), bad), "inside the mod folder"));
    CHECK(has_error_containing(parse_text(with(R"("wasm": "w.dll")"), bad), ".wasm file"));
    CHECK(has_error_containing(parse_text(with(R"("wasm": 3)"), bad), "\"wasm\""));
    CHECK(has_error_containing(parse_text(with(R"("wasm": "w.wasm", "entry": "w.dll")"), bad), "not both"));
}
