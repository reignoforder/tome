#include "core/resolver.h"
#include "test.h"

#include <deque>
#include <initializer_list>

using namespace tome;

namespace {

// Builds metadata from compact JSON fragments; keeps storage stable for the
// pointers in ResolveInput.
struct Fixture {
    std::deque<ModMetadata> metas;
    std::vector<ResolveInput> inputs;
    ResolveEnvironment env;

    Fixture() {
        parse_semver("0.1.0", env.tome_version);
        parse_game_version("1.27b", env.game_version);
    }

    size_t add(const std::string& id, const std::string& extra = "", bool enabled = true,
               const std::string& version = "1.0.0") {
        std::string text = R"({"schema":1,"id":")" + id + R"(","version":")" + version +
                           R"(","game":"*","tome":"*","sync":"client")" + (extra.empty() ? "" : "," + extra) + "}";
        json::Value root;
        if (!json::parse(text, root).ok) test::fail(__FILE__, __LINE__, "bad JSON: " + text);
        metas.emplace_back();
        auto r = parse_metadata(root, metas.back());
        if (!r.ok) test::fail(__FILE__, __LINE__, "bad metadata: " + (r.errors.empty() ? "" : r.errors[0]));
        inputs.push_back({&metas.back(), enabled});
        return inputs.size() - 1;
    }

    ResolveResult run() { return resolve(inputs, env); }

    std::vector<std::string> order(const ResolveResult& r) const {
        std::vector<std::string> ids;
        for (size_t i : r.load_order) ids.push_back(inputs[i].meta->id);
        return ids;
    }
};

std::vector<std::string> ids(std::initializer_list<const char*> list) {
    return std::vector<std::string>(list.begin(), list.end());
}

} // namespace

TEST(resolver_orders_dependencies_first) {
    Fixture f;
    f.add("zeta", R"("depends":{"alpha":"*"})");
    f.add("alpha");
    f.add("mid", R"("depends":{"zeta":"^1"})");
    auto r = f.run();
    CHECK_EQ(f.order(r), ids({"alpha", "zeta", "mid"}));
}

TEST(resolver_ties_break_alphabetically) {
    Fixture f;
    f.add("cc");
    f.add("aa");
    f.add("bb");
    auto r = f.run();
    CHECK_EQ(f.order(r), ids({"aa", "bb", "cc"}));
}

TEST(resolver_honours_load_before_and_after) {
    Fixture f;
    f.add("aa", R"("load":{"after":["zz"]})");
    f.add("zz");
    f.add("mm", R"("load":{"before":["zz"]})");
    f.add("qq", R"("load":{"after":["not-installed"]})");
    auto r = f.run();
    CHECK_EQ(f.order(r), ids({"mm", "qq", "zz", "aa"}));
}

TEST(resolver_missing_and_wrong_version_dependency) {
    Fixture f;
    size_t a = f.add("needs-ghost", R"("depends":{"ghost":"*"})");
    size_t b = f.add("needs-new", R"("depends":{"lib":">=2"})");
    f.add("lib", "", true, "1.5.0");
    auto r = f.run();
    CHECK(r.mods[a].state == ModState::MissingDependency);
    CHECK(r.mods[a].reason.find("isn't installed") != std::string::npos);
    CHECK(r.mods[b].state == ModState::MissingDependency);
    CHECK(r.mods[b].reason.find("1.5.0 is installed") != std::string::npos);
    CHECK_EQ(f.order(r), ids({"lib"}));
}

TEST(resolver_disabled_dependency_cascades) {
    Fixture f;
    size_t lib = f.add("lib", "", false);
    size_t mid = f.add("mid", R"("depends":{"lib":"*"})");
    size_t top = f.add("top", R"("depends":{"mid":"*"})");
    auto r = f.run();
    CHECK(r.mods[lib].state == ModState::DisabledByUser);
    CHECK(r.mods[mid].state == ModState::DependencyInactive);
    CHECK(r.mods[mid].reason.find("disabled") != std::string::npos);
    CHECK(r.mods[top].state == ModState::DependencyInactive);
    CHECK(r.load_order.empty());
}

TEST(resolver_incompatible_game_and_tome) {
    Fixture f;
    size_t g = f.add("old-mod");
    f.metas.back().game = GameVersionRange();
    std::string err;
    parse_game_range("1.24e", f.metas.back().game, err);
    size_t t = f.add("future-mod");
    parse_semver_range(">=2.0", f.metas.back().tome, err);
    auto r = f.run();
    CHECK(r.mods[g].state == ModState::IncompatibleGame);
    CHECK(r.mods[g].reason.find("1.27b") != std::string::npos);
    CHECK(r.mods[t].state == ModState::IncompatibleTome);
}

TEST(resolver_duplicate_ids_block_both) {
    Fixture f;
    size_t a = f.add("dup");
    size_t b = f.add("dup");
    auto r = f.run();
    CHECK(r.mods[a].state == ModState::DuplicateId);
    CHECK(r.mods[b].state == ModState::DuplicateId);
}

TEST(resolver_breaks) {
    Fixture f;
    size_t a = f.add("new-bars", R"("breaks":{"old-bars":"<2"})");
    size_t b = f.add("old-bars", "", true, "1.0.0");
    auto r = f.run();
    CHECK(r.mods[a].state == ModState::Breaks);
    CHECK(r.mods[b].state == ModState::Active);
}

TEST(resolver_breaks_ignored_when_other_disabled) {
    Fixture f;
    size_t a = f.add("new-bars", R"("breaks":{"old-bars":"*"})");
    f.add("old-bars", "", false);
    auto r = f.run();
    CHECK(r.mods[a].state == ModState::Active);
}

TEST(resolver_conflicts_and_recommends_are_warnings) {
    Fixture f;
    size_t a = f.add("aa", R"("conflicts":{"bb":"*"},"recommends":{"cc":"*"})");
    f.add("bb");
    auto r = f.run();
    CHECK(r.mods[a].state == ModState::Active);
    CHECK_EQ(r.mods[a].warnings.size(), size_t(2));
}

TEST(resolver_provides_satisfies_unversioned_dependency) {
    Fixture f;
    f.add("ujapi-port", R"("provides":["ujapi"])");
    size_t user = f.add("map-helper", R"("depends":{"ujapi":"*"})");
    size_t strict = f.add("strict", R"("depends":{"ujapi":">=1"})");
    auto r = f.run();
    CHECK(r.mods[user].state == ModState::Active);
    CHECK(r.mods[strict].state == ModState::MissingDependency);
    CHECK_EQ(f.order(r), ids({"ujapi-port", "map-helper"}));
}

TEST(resolver_detects_cycles) {
    Fixture f;
    size_t a = f.add("aa", R"("depends":{"bb":"*"})");
    size_t b = f.add("bb", R"("depends":{"aa":"*"})");
    size_t c = f.add("cc", R"("depends":{"aa":"*"})");
    size_t d = f.add("dd");
    auto r = f.run();
    CHECK(r.mods[a].state == ModState::Cycle);
    CHECK(r.mods[b].state == ModState::Cycle);
    CHECK(r.mods[c].state == ModState::Cycle);
    CHECK(r.mods[d].state == ModState::Active);
    CHECK_EQ(f.order(r), ids({"dd"}));
}
