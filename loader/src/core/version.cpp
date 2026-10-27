#include "version.h"

#include <cstdio>
#include <type_traits>

namespace tome {
namespace {

struct GameAlias {
    const char* name;
    GameVersion version;
};

// Builds the community refers to by name. 1.27b is confirmed from this repo's
// game.dll; the others are from community sources. Verify each one against a
// real binary when adding its mappings.
const GameAlias kGameAliases[] = {
    {"1.24e", {{1, 24, 4, 6387}}},
    {"1.26a", {{1, 26, 0, 6401}}},
    {"1.27a", {{1, 27, 0, 52240}}},
    {"1.27b", {{1, 27, 1, 7085}}},
};

std::vector<std::string_view> split(std::string_view text, std::string_view sep) {
    std::vector<std::string_view> parts;
    size_t start = 0;
    for (;;) {
        size_t at = text.find(sep, start);
        if (at == std::string_view::npos) {
            parts.push_back(text.substr(start));
            return parts;
        }
        parts.push_back(text.substr(start, at - start));
        start = at + sep.size();
    }
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// Comparators of a range, split on whitespace. An operator on its own
// (">= 1.2") goes with the version after it.
std::vector<std::string> split_comparators(std::string_view s) {
    std::vector<std::string> tokens;
    bool joining = false;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        size_t start = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
        if (i == start) continue;
        std::string_view token = s.substr(start, i - start);
        if (joining) {
            tokens.back() += token;
        } else {
            tokens.emplace_back(token);
        }
        joining = token.find_first_not_of("<>=^~") == std::string_view::npos;
    }
    return tokens;
}

bool parse_uint(std::string_view s, uint32_t& out) {
    if (s.empty() || s.size() > 9) return false;
    uint32_t value = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<uint32_t>(c - '0');
    }
    out = value;
    return true;
}

bool is_wildcard(std::string_view s) {
    return s == "x" || s == "X" || s == "*";
}

// Splits a version into up to `max_parts` numeric parts. A part may be a
// wildcard, which ends the version (it becomes partial). Returns the number of
// concrete parts, or -1 on error. `rest` receives anything after the numbers
// (semver pre-release / build metadata).
int parse_partial(std::string_view text, uint32_t* parts, int max_parts, std::string_view* rest) {
    size_t end = text.find_first_of("-+");
    std::string_view numbers = text.substr(0, end);
    if (rest) {
        *rest = end == std::string_view::npos ? std::string_view() : text.substr(end);
    } else if (end != std::string_view::npos) {
        return -1;
    }
    auto pieces = split(numbers, ".");
    if (pieces.empty() || static_cast<int>(pieces.size()) > max_parts) return -1;

    int concrete = 0;
    bool wildcard_seen = false;
    for (std::string_view piece : pieces) {
        if (is_wildcard(piece)) {
            wildcard_seen = true;
            continue;
        }
        if (wildcard_seen) return -1;  // "1.x.3"
        if (!parse_uint(piece, parts[concrete])) return -1;
        ++concrete;
    }
    for (int i = concrete; i < max_parts; ++i) parts[i] = 0;
    return concrete;
}

bool is_numeric(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

template <typename V>
bool holds(const Comparator<V>& c, const V& v) {
    int cmp = compare(v, c.version);
    switch (c.op) {
    case Comparator<V>::Op::Eq: return cmp == 0;
    case Comparator<V>::Op::Lt: return cmp < 0;
    case Comparator<V>::Op::Le: return cmp <= 0;
    case Comparator<V>::Op::Gt: return cmp > 0;
    case Comparator<V>::Op::Ge: return cmp >= 0;
    }
    return false;
}

template <typename V>
using Op = typename Comparator<V>::Op;

// Splits a leading operator off a comparator token.
template <typename V>
std::string_view take_operator(std::string_view token, Op<V>& op, bool& has_op) {
    has_op = true;
    if (token.substr(0, 2) == ">=") { op = Op<V>::Ge; return token.substr(2); }
    if (token.substr(0, 2) == "<=") { op = Op<V>::Le; return token.substr(2); }
    if (token.substr(0, 1) == ">")  { op = Op<V>::Gt; return token.substr(1); }
    if (token.substr(0, 1) == "<")  { op = Op<V>::Lt; return token.substr(1); }
    if (token.substr(0, 1) == "=")  { op = Op<V>::Eq; return token.substr(1); }
    has_op = false;
    op = Op<V>::Eq;
    return token;
}

// Comparators that express a partial version "1.2" (= 1.2.*) for a version
// with `total` parts of which `concrete` are given.
template <typename V>
void push_prefix_range(std::vector<Comparator<V>>& set, const uint32_t* parts, int concrete, int total) {
    V low;
    for (int i = 0; i < total; ++i) {
        if constexpr (std::is_same_v<V, SemVer>) {
            uint32_t* fields[] = {&low.major, &low.minor, &low.patch};
            *fields[i] = i < concrete ? parts[i] : 0;
        } else {
            low.part[i] = i < concrete ? parts[i] : 0;
        }
    }
    if (concrete == 0) {
        return;  // "*": no comparator, so even pre-releases of 0.0.0 match
    }
    set.push_back({Op<V>::Ge, low});
    V high = low;
    if constexpr (std::is_same_v<V, SemVer>) {
        uint32_t* fields[] = {&high.major, &high.minor, &high.patch};
        *fields[concrete - 1] += 1;
        for (int i = concrete; i < total; ++i) *fields[i] = 0;
        high.prerelease = {"0"};  // exclude pre-releases of the next version
    } else {
        high.part[concrete - 1] += 1;
        for (int i = concrete; i < total; ++i) high.part[i] = 0;
    }
    set.push_back({Op<V>::Lt, high});
}

} // namespace

std::string SemVer::to_string() const {
    char buf[48];
    snprintf(buf, sizeof(buf), "%u.%u.%u", major, minor, patch);
    std::string s = buf;
    for (size_t i = 0; i < prerelease.size(); ++i) {
        s += i == 0 ? '-' : '.';
        s += prerelease[i];
    }
    return s;
}

bool parse_semver(std::string_view text, SemVer& out) {
    uint32_t parts[3];
    std::string_view rest;
    if (parse_partial(text, parts, 3, &rest) != 3) return false;
    SemVer v;
    v.major = parts[0];
    v.minor = parts[1];
    v.patch = parts[2];
    // Build metadata (+...) is ignored for precedence.
    size_t plus = rest.find('+');
    std::string_view pre = rest.substr(0, plus);
    if (!pre.empty()) {
        pre.remove_prefix(1);  // '-'
        for (std::string_view id : split(pre, ".")) {
            if (id.empty()) return false;
            for (char c : id) {
                bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
                if (!ok) return false;
            }
            v.prerelease.emplace_back(id);
        }
    }
    out = std::move(v);
    return true;
}

int compare(const SemVer& a, const SemVer& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    // A version without pre-release ranks above one with.
    if (a.prerelease.empty() != b.prerelease.empty()) return a.prerelease.empty() ? 1 : -1;
    for (size_t i = 0; i < a.prerelease.size() && i < b.prerelease.size(); ++i) {
        const std::string& x = a.prerelease[i];
        const std::string& y = b.prerelease[i];
        bool xn = is_numeric(x), yn = is_numeric(y);
        if (xn && yn) {
            if (x.size() != y.size()) return x.size() < y.size() ? -1 : 1;
            if (x != y) return x < y ? -1 : 1;
        } else if (xn != yn) {
            return xn ? -1 : 1;  // numeric identifiers rank lower
        } else if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    if (a.prerelease.size() != b.prerelease.size()) return a.prerelease.size() < b.prerelease.size() ? -1 : 1;
    return 0;
}

std::string GameVersion::to_string() const {
    char buf[48];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", part[0], part[1], part[2], part[3]);
    return buf;
}

bool parse_game_version(std::string_view text, GameVersion& out) {
    for (const GameAlias& alias : kGameAliases) {
        if (text == alias.name) {
            out = alias.version;
            return true;
        }
    }
    GameVersion v;
    if (parse_partial(text, v.part, 4, nullptr) <= 0) return false;
    out = v;
    return true;
}

int compare(const GameVersion& a, const GameVersion& b) {
    for (int i = 0; i < 4; ++i) {
        if (a.part[i] != b.part[i]) return a.part[i] < b.part[i] ? -1 : 1;
    }
    return 0;
}

const char* game_version_alias(const GameVersion& version) {
    for (const GameAlias& alias : kGameAliases) {
        if (compare(alias.version, version) == 0) return alias.name;
    }
    return nullptr;
}

template <typename V>
bool Range<V>::any() const {
    for (const auto& set : alternatives_) {
        if (set.empty()) return true;
    }
    return false;
}

template <typename V>
bool Range<V>::matches(const V& version) const {
    for (const auto& set : alternatives_) {
        bool all = true;
        for (const auto& c : set) {
            if (!holds(c, version)) {
                all = false;
                break;
            }
        }
        if (all) return true;
    }
    return false;
}

template class Range<SemVer>;
template class Range<GameVersion>;

bool parse_semver_range(std::string_view text, SemVerRange& out, std::string& error) {
    SemVerRange range;
    range.text_ = std::string(trim(text));
    for (std::string_view alt : split(text, "||")) {
        std::vector<Comparator<SemVer>> set;
        auto tokens = split_comparators(alt);
        if (tokens.empty()) {
            if (trim(text).empty()) {
                tokens.push_back("*");
            } else {
                error = "empty alternative in range \"" + std::string(text) + "\"";
                return false;
            }
        }
        for (std::string_view token : tokens) {
            char prefix = token.front();
            if (prefix == '^' || prefix == '~') {
                SemVer base;
                uint32_t parts[3];
                std::string_view rest;
                int concrete = parse_partial(token.substr(1), parts, 3, &rest);
                if (concrete <= 0) {
                    error = "invalid version in \"" + std::string(token) + "\"";
                    return false;
                }
                if (concrete == 3) {
                    if (!parse_semver(token.substr(1), base)) {
                        error = "invalid version in \"" + std::string(token) + "\"";
                        return false;
                    }
                } else {
                    base.major = parts[0];
                    base.minor = parts[1];
                    base.patch = 0;
                }
                SemVer high;
                if (prefix == '~') {
                    // ~1.2.3 → <1.3.0 ; ~1 → <2.0.0
                    if (concrete >= 2) { high.major = base.major; high.minor = base.minor + 1; }
                    else { high.major = base.major + 1; }
                } else {
                    // ^1.2.3 → <2.0.0 ; ^0.2.3 → <0.3.0 ; ^0.0.3 → <0.0.4
                    if (base.major > 0 || concrete == 1) { high.major = base.major + 1; }
                    else if (base.minor > 0 || concrete == 2) { high.minor = base.minor + 1; }
                    else { high.patch = base.patch + 1; }
                }
                high.prerelease = {"0"};
                set.push_back({Op<SemVer>::Ge, base});
                set.push_back({Op<SemVer>::Lt, high});
                continue;
            }

            Op<SemVer> op;
            bool has_op;
            std::string_view version_text = take_operator<SemVer>(token, op, has_op);
            uint32_t parts[3];
            std::string_view rest;
            int concrete = parse_partial(version_text, parts, 3, &rest);
            if (concrete < 0 || (concrete == 0 && has_op)) {
                error = "invalid version in \"" + std::string(token) + "\"";
                return false;
            }
            if (concrete < 3 && !rest.empty()) {
                error = "pre-release needs a full version in \"" + std::string(token) + "\"";
                return false;
            }
            if (!has_op && concrete < 3) {
                push_prefix_range(set, parts, concrete, 3);
                continue;
            }
            SemVer v;
            if (concrete == 3) {
                if (!parse_semver(version_text, v)) {
                    error = "invalid version in \"" + std::string(token) + "\"";
                    return false;
                }
            } else {
                // ">=1.2" means ">=1.2.0"; "<1.2" means "<1.2.0"; ">1.2" means ">=1.3.0"; "<=1.2" means "<1.3.0".
                v.major = parts[0];
                v.minor = concrete >= 2 ? parts[1] : 0;
                if (op == Op<SemVer>::Gt || op == Op<SemVer>::Le) {
                    if (concrete == 1) v.major += 1; else v.minor += 1;
                    op = op == Op<SemVer>::Gt ? Op<SemVer>::Ge : Op<SemVer>::Lt;
                    v.prerelease = {"0"};
                }
                if (op == Op<SemVer>::Eq) {
                    push_prefix_range(set, parts, concrete, 3);
                    continue;
                }
            }
            set.push_back({op, v});
        }
        range.alternatives_.push_back(std::move(set));
    }
    out = std::move(range);
    return true;
}

bool parse_game_range(std::string_view text, GameVersionRange& out, std::string& error) {
    GameVersionRange range;
    range.text_ = std::string(trim(text));
    for (std::string_view alt : split(text, "||")) {
        std::vector<Comparator<GameVersion>> set;
        auto tokens = split_comparators(alt);
        if (tokens.empty()) {
            if (trim(text).empty()) {
                tokens.push_back("*");
            } else {
                error = "empty alternative in range \"" + std::string(text) + "\"";
                return false;
            }
        }
        for (std::string_view token : tokens) {
            Op<GameVersion> op;
            bool has_op;
            std::string_view version_text = take_operator<GameVersion>(token, op, has_op);

            bool is_alias = false;
            for (const GameAlias& alias : kGameAliases) {
                if (version_text == alias.name) {
                    set.push_back({op, alias.version});
                    is_alias = true;
                    break;
                }
            }
            if (is_alias) continue;

            uint32_t parts[4];
            int concrete = parse_partial(version_text, parts, 4, nullptr);
            if (concrete < 0 || (concrete == 0 && has_op)) {
                error = "unknown game version \"" + std::string(version_text) + "\"";
                return false;
            }
            if (concrete < 4 && (!has_op || op == Op<GameVersion>::Eq)) {
                push_prefix_range(set, parts, concrete, 4);
                continue;
            }
            GameVersion v;
            for (int i = 0; i < 4; ++i) v.part[i] = parts[i];
            if (concrete < 4 && (op == Op<GameVersion>::Gt || op == Op<GameVersion>::Le)) {
                // ">1.27" means after every 1.27.x.x build.
                v.part[concrete - 1] += 1;
                op = op == Op<GameVersion>::Gt ? Op<GameVersion>::Ge : Op<GameVersion>::Lt;
            }
            set.push_back({op, v});
        }
        range.alternatives_.push_back(std::move(set));
    }
    out = std::move(range);
    return true;
}

} // namespace tome
