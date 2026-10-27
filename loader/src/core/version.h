#pragma once

// Version numbers and ranges used in tome.mod.json.
//
// Mod and Tome versions are semver ("1.2.3", "1.0.0-beta.1"). Ranges accept
// "*", exact versions, comparators (">=1.2 <2"), caret ("^1.2"), tilde ("~1.2"),
// partial versions ("1.2" means 1.2.x) and alternatives joined with "||".
//
// Game versions are the four-part file version of the game binary
// ("1.27.1.7085") or a community alias ("1.27b"). Game ranges accept "*",
// comparators, partial versions ("1.27" means 1.27.x.x) and "||".

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tome {

struct SemVer {
    uint32_t major = 0, minor = 0, patch = 0;
    std::vector<std::string> prerelease;  // dot-separated identifiers after '-'

    std::string to_string() const;
};

bool parse_semver(std::string_view text, SemVer& out);
int compare(const SemVer& a, const SemVer& b);  // <0, 0, >0

struct GameVersion {
    uint32_t part[4] = {0, 0, 0, 0};

    std::string to_string() const;
};

// Accepts "a.b.c.d" (missing trailing parts are 0) or a known alias like "1.27b".
bool parse_game_version(std::string_view text, GameVersion& out);
int compare(const GameVersion& a, const GameVersion& b);

// Community name for a build, or nullptr if it has none.
const char* game_version_alias(const GameVersion& version);

template <typename V>
struct Comparator {
    enum class Op { Eq, Lt, Le, Gt, Ge };
    Op op;
    V version;
};

// A union ("||") of intersections (space-separated comparators).
template <typename V>
class Range {
public:
    bool matches(const V& version) const;
    const std::string& text() const { return text_; }

private:
    friend bool parse_semver_range(std::string_view, Range<SemVer>&, std::string&);
    friend bool parse_game_range(std::string_view, Range<GameVersion>&, std::string&);

    std::string text_;
    std::vector<std::vector<Comparator<V>>> alternatives_;
};

using SemVerRange = Range<SemVer>;
using GameVersionRange = Range<GameVersion>;

bool parse_semver_range(std::string_view text, SemVerRange& out, std::string& error);
bool parse_game_range(std::string_view text, GameVersionRange& out, std::string& error);

} // namespace tome
