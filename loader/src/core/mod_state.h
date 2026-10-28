#pragma once

// tome/state.json: what the user chose for each mod (on or off) and which
// code they allowed to run. A mod with code (a DLL) doesn't run until the
// user turns it on once, in the mod menu or with /tome enable; that records
// a SHA-256 of its code files. The decisions here are portable; ModManager
// does the file work.

#include "json.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace tome::mod_state {

// 1: "enabled" only. 2: "approved" and "approved_from" too.
constexpr int kSchema = 2;

struct Approval {
    std::string hash;    // code_hash of what was allowed (64 hex digits); "" if nothing was
    std::string folder;  // where it was in tome/mods then: a folder's name or a package's file name
    bool operator==(const Approval&) const = default;
};

struct Entry {
    bool enabled = true;  // the user's choice; a new mod starts on, but its code still waits for approval
    Approval approved;
    bool operator==(const Entry&) const = default;
};

using State = std::map<std::string, Entry>;  // by mod id

// state.json's root. `legacy` is set for a file from before approvals
// (schema 1): its mods ran without one, so the caller approves the ones
// installed now instead of stopping them.
State parse(const json::Value& root, bool& legacy);
json::Value serialize(const State& state);

enum class Need {
    None,     // runs if it's on (no code, or the code was allowed)
    New,      // code that was never allowed
    Changed,  // code that differs from what was allowed
};

// Whether a mod has to be allowed (again) before it runs:
// - a mod without code never does;
// - with nothing allowed for its id, New;
// - a folder mod in the folder it was allowed in never again, so a mod
//   being built into tome/mods hot-reloads as before;
// - otherwise its code must hash to what was allowed (a replaced package,
//   or another folder or package with the same id): Changed if it doesn't.
// `code_hash` is "" if the code couldn't be read, which never matches.
Need need(const Entry* entry, bool has_code, bool package, const std::string& folder, const std::string& code_hash);

// For the mod list: why a mod that needs approval doesn't run. `native`:
// it has a DLL (otherwise only game scripts, is_script).
const char* describe(Need need, bool native);

struct CodeFile {
    std::string path;    // relative to the mod folder, with '/'
    std::string sha256;  // 64 hex digits
};

// Whether an asset is a script the game runs as JASS: a .j (common.j,
// Blizzard.j, a war3map.j), an AI script (.ai) or a preload file (.pld,
// which Preloader runs). An asset-only mod with one is code too: it runs
// in every game. `path` is '/' or '\' separated; ASCII case is ignored.
bool is_script(std::string_view path);

// Whether a file in a mod folder counts as code: everything but the top
// tome.mod.json and the assets folder (`assets`, relative, '/' or '\'),
// since a DLL can load any other file as code, plus the scripts among the
// assets (is_script). ASCII case is ignored.
bool is_code_file(std::string_view path, std::string_view assets);

// The text a mod's code hash is the SHA-256 of: one line per file, sorted by
// path, as sha256sum prints them ("<hash>  <path>\n"), so it can be checked
// by hand.
std::string manifest(std::vector<CodeFile> files);

// TOME_DEBUG_APPROVE, for tests: a comma-separated list of mod ids whose
// code counts as allowed, or "*" for every mod.
bool debug_approves(std::string_view list, std::string_view id);

} // namespace tome::mod_state
