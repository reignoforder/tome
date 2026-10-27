#include "core/version.h"
#include "test.h"

using namespace tome;

namespace {

SemVer v(const char* text) {
    SemVer out;
    if (!parse_semver(text, out)) test::fail(__FILE__, __LINE__, std::string("bad semver in test: ") + text);
    return out;
}

bool in(const char* range_text, const char* version) {
    SemVerRange range;
    std::string err;
    if (!parse_semver_range(range_text, range, err)) {
        test::fail(__FILE__, __LINE__, "bad range in test: " + err);
        return false;
    }
    return range.matches(v(version));
}

bool game_in(const char* range_text, const char* version) {
    GameVersionRange range;
    GameVersion gv;
    std::string err;
    if (!parse_game_range(range_text, range, err) || !parse_game_version(version, gv)) {
        test::fail(__FILE__, __LINE__, "bad game range/version in test: " + err);
        return false;
    }
    return range.matches(gv);
}

} // namespace

TEST(semver_parses_and_orders) {
    SemVer out;
    CHECK(parse_semver("1.2.3", out));
    CHECK(!parse_semver("1.2", out));
    CHECK(!parse_semver("1.2.x", out));
    CHECK(!parse_semver("v1.2.3", out));
    CHECK(parse_semver("1.0.0-beta.2+build.5", out));
    CHECK_EQ(out.to_string(), std::string("1.0.0-beta.2"));

    // Precedence examples from semver.org.
    const char* ordered[] = {"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
                             "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0", "1.0.1", "1.1.0", "2.0.0"};
    for (size_t i = 0; i + 1 < sizeof(ordered) / sizeof(ordered[0]); ++i) {
        CHECK(compare(v(ordered[i]), v(ordered[i + 1])) < 0);
        CHECK(compare(v(ordered[i + 1]), v(ordered[i])) > 0);
    }
    CHECK_EQ(compare(v("1.2.3+a"), v("1.2.3+b")), 0);
}

TEST(semver_range_basics) {
    CHECK(in("*", "0.0.1"));
    CHECK(in("", "5.0.0"));
    CHECK(in("1.2.3", "1.2.3"));
    CHECK(!in("1.2.3", "1.2.4"));
    CHECK(in(">=1.2 <2", "1.9.9"));
    CHECK(!in(">=1.2 <2", "2.0.0"));
    CHECK(!in(">=1.2 <2", "1.1.9"));
    CHECK(in(">1.2", "1.3.0"));
    CHECK(!in(">1.2", "1.2.9"));
    CHECK(in("<=1.2", "1.2.9"));
    CHECK(!in("<=1.2", "1.3.0"));
    CHECK(in("1.x", "1.9.0"));
    CHECK(!in("1.x", "2.0.0"));
    CHECK(in("1.2", "1.2.7"));
    CHECK(!in("1.2", "1.3.0"));
    CHECK(in("<1 || >=3", "0.5.0"));
    CHECK(in("<1 || >=3", "3.1.0"));
    CHECK(!in("<1 || >=3", "2.0.0"));
}

TEST(semver_range_caret_and_tilde) {
    CHECK(in("^1.2.3", "1.9.0"));
    CHECK(!in("^1.2.3", "2.0.0"));
    CHECK(!in("^1.2.3", "1.2.2"));
    CHECK(in("^0.2.3", "0.2.9"));
    CHECK(!in("^0.2.3", "0.3.0"));
    CHECK(in("^0.0.3", "0.0.3"));
    CHECK(!in("^0.0.3", "0.0.4"));
    CHECK(in("^1.2", "1.5.0"));
    CHECK(in("~1.2.3", "1.2.9"));
    CHECK(!in("~1.2.3", "1.3.0"));
    CHECK(in("~1", "1.9.9"));
    CHECK(!in("~1", "2.0.0"));
    // Pre-releases of the next major don't sneak in under the upper bound.
    CHECK(!in("^1.0.0", "2.0.0-alpha"));
}

TEST(semver_range_operator_apart_from_version) {
    CHECK(in(">= 1.0.0", "1.2.0"));
    CHECK(!in(">= 1.0.0", "0.9.0"));
    CHECK(in(">= 1.2 < 2", "1.5.0"));
    CHECK(!in(">= 1.2 < 2", "2.0.0"));
    CHECK(in("^ 1.2", "1.9.0"));
    CHECK(game_in(">= 1.27", "1.27.1.7085"));
    CHECK(!game_in("< 1.27", "1.27.1.7085"));
    SemVerRange range;
    std::string err;
    CHECK(!parse_semver_range(">=", range, err));
    CHECK(!parse_semver_range("1.0.0 >=", range, err));
}

TEST(semver_range_any) {
    // Every version, pre-releases of 0.0.0 too.
    CHECK(in("*", "0.0.0-alpha"));
    CHECK(in("x", "0.0.0-alpha"));
    CHECK(in("", "0.0.0-alpha"));
    SemVerRange range;
    std::string err;
    for (const char* text : {"*", "x", "X", "", "x.x", "*.*.*", "1 || *"}) {
        CHECK(parse_semver_range(text, range, err) && range.any());
    }
    for (const char* text : {"1.x", ">=0.0.0", "^0"}) {
        CHECK(parse_semver_range(text, range, err) && !range.any());
    }
}

TEST(semver_range_rejects_garbage) {
    SemVerRange range;
    std::string err;
    CHECK(!parse_semver_range(">=banana", range, err));
    CHECK(!err.empty());
    CHECK(!parse_semver_range("1.2 ||", range, err));
    CHECK(!parse_semver_range("^", range, err));
}

TEST(game_versions_and_aliases) {
    GameVersion gv;
    CHECK(parse_game_version("1.27b", gv));
    CHECK_EQ(gv.to_string(), std::string("1.27.1.7085"));
    CHECK(parse_game_version("1.27.1.7085", gv));
    CHECK_EQ(std::string(game_version_alias(gv)), std::string("1.27b"));
    CHECK(!parse_game_version("1.27z", gv));
    CHECK(!parse_game_version("1.2.3.4.5", gv));
}

TEST(game_ranges) {
    CHECK(game_in("1.27b", "1.27.1.7085"));
    CHECK(!game_in("1.27b", "1.27a"));
    CHECK(game_in(">=1.26a <1.29", "1.27b"));
    CHECK(!game_in(">=1.26a <1.29", "1.24e"));
    CHECK(!game_in(">=1.26a <1.29", "1.29.2.9231"));
    CHECK(game_in("1.27", "1.27a"));
    CHECK(game_in("1.27", "1.27b"));
    CHECK(!game_in("1.27", "1.26a"));
    CHECK(game_in(">1.27", "1.28.5.7680"));
    CHECK(!game_in(">1.27", "1.27.1.9999"));
    CHECK(game_in("*", "1.20.4.6074"));
    CHECK(game_in("1.24e || 1.26a", "1.26a"));
    CHECK(!game_in("1.24e || 1.26a", "1.27b"));

    GameVersionRange range;
    std::string err;
    CHECK(!parse_game_range(">=1.27z", range, err));
}
