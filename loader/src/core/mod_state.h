#pragma once

// tome/state.json: what the user chose for each mod (on or off) and which
// code they allowed to run. A mod with code (a DLL, game scripts, or any
// other file the game reads) doesn't run until the user turns it on once, in
// the mod menu or with /tome enable; that records a SHA-256 of its files. The
// decisions here are portable; ModManager does the file work.

#include "json.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace tome::mod_state {

// 1: "enabled" only. 2: "approved" and "approved_from" too, for DLLs and
// game scripts. 3: the same for every mod with files the game reads (assets,
// an icon). Approvals of an older schema aren't carried over: its hashes
// cover other files, so those mods ask again.
constexpr int kSchema = 3;

// What a mod has that must be allowed, most powerful first wins.
enum class CodeKind {
    None,     // nothing the game or Tome runs or reads (only tome.mod.json, settings)
    Assets,   // files the game reads with its own loaders (models, textures, sounds, UI, an icon)
    Wasm,     // a WebAssembly module (docs/wasm-mods.md): sandboxed, but its natives reach map scripts
    Scripts,  // game scripts among its assets (is_script)
    Native,   // a DLL
};

struct Approval {
    std::string hash;    // code_hash of what was allowed (64 hex digits); "" if nothing was
    std::string folder;  // where it was in tome/mods then: a folder's name or a package's file name
    std::string key;     // the tome.sig key it was signed with (core/signature.h, base64); "" if it wasn't
    bool operator==(const Approval&) const = default;
};

struct Entry {
    bool enabled = true;  // the user's choice; a new mod starts on, but its code still waits for approval
    Approval approved;
    bool operator==(const Entry&) const = default;
};

using State = std::map<std::string, Entry>;  // by mod id

// state.json's root. `schema` is the file's (0 if it has none): one older
// than kSchema is written again as kSchema.
State parse(const json::Value& root, int& schema);
json::Value serialize(const State& state);

enum class Need {
    None,           // runs if it's on (no code, or the code was allowed)
    New,            // code that was never allowed
    Changed,        // code that differs from what was allowed
    SignerChanged,  // ...and what was allowed was signed, but this isn't, or by another key
};

// Whether a mod has to be allowed (again) before it runs:
// - a mod without code never does;
// - with nothing allowed for its id, New;
// - a folder mod in the folder it was allowed in never again, so a mod
//   being built into tome/mods hot-reloads as before;
// - otherwise its code must hash to what was allowed (a replaced package,
//   or another folder or package with the same id);
// - or, if what was allowed was signed, be signed by the same key (an
//   update from the same author): SignerChanged if it isn't;
// - Changed otherwise.
// `code_hash` is "" if the code couldn't be read, which never matches.
// `signer` is the key of the mod's valid tome.sig, "" if it has none.
Need need(const Entry* entry, bool has_code, bool package, const std::string& folder, const std::string& code_hash,
          const std::string& signer = {});

// For the mod list: why a mod that needs approval doesn't run.
const char* describe(Need need, CodeKind kind);

// ModStatus::code_kind: "native", "scripts", "wasm", "assets" or "".
const char* kind_name(CodeKind kind);

struct CodeFile {
    std::string path;    // relative to the mod folder, with '/'
    std::string sha256;  // 64 hex digits
};

// Whether an asset is a script the game runs as JASS: a .j (common.j,
// Blizzard.j, a war3map.j), an AI script (.ai) or a preload file (.pld,
// which Preloader runs). An asset-only mod with one is code too: it runs
// in every game. `path` is '/' or '\' separated; ASCII case is ignored.
bool is_script(std::string_view path);

// Whether a file in a mod folder is hashed for its approval: everything but
// the top tome.mod.json and tome.sig (a DLL can load any file as code, and
// the game reads the assets with parsers that trust them; tome.sig signs
// this hash). ASCII case is ignored.
bool is_code_file(std::string_view path);

// The text a mod's code hash is the SHA-256 of: one line per file, sorted by
// path, as sha256sum prints them ("<hash>  <path>\n"), so it can be checked
// by hand.
std::string manifest(std::vector<CodeFile> files);

// TOME_DEBUG_APPROVE, for tests (read only by developer builds, TOME_DEV): a comma-separated list of mod ids whose
// code counts as allowed, or "*" for every mod.
bool debug_approves(std::string_view list, std::string_view id);

} // namespace tome::mod_state
