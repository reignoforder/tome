// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// tome.mod.json: see docs/research.md §7 for the full schema.

#include "json.h"
#include "keys.h"
#include "version.h"

#include <string>
#include <vector>

namespace tome {

constexpr int kMetadataSchema = 1;
// The longest id and version a mod may have (both travel in one lobby chat
// line in the multiplayer handshake, core/handshake.h).
constexpr size_t kMaxIdLength = 64;
constexpr size_t kMaxVersionLength = 64;

struct Dependency {
    std::string id;
    SemVerRange range;
};

enum class SyncMode { Client, Required };
enum class ReloadMode { Live, Map, Restart };

struct ModMetadata {
    int schema = 0;
    std::string id;
    SemVer version;
    std::string name;  // defaults to id
    std::string description;
    std::vector<std::string> authors;
    std::string license;
    std::string icon;

    GameVersionRange game;
    SemVerRange tome;

    std::vector<Dependency> depends;
    std::vector<Dependency> recommends;
    std::vector<Dependency> conflicts;
    std::vector<Dependency> breaks;

    // DLL relative to the mod folder. Either one path for every build, or a
    // list of (game range, path) checked in order. Empty = assets-only mod.
    std::string entry;
    std::vector<std::pair<GameVersionRange, std::string>> entry_by_game;
    // Or a WebAssembly module (docs/wasm-mods.md), relative to the mod
    // folder, run sandboxed on every build; exclusive with `entry`.
    std::string wasm;

    std::string assets;
    std::vector<std::string> requires_;  // mapped names / capabilities (§6.3)
    std::vector<std::string> natives;    // JASS declarations
    std::vector<std::string> provides;

    SyncMode sync = SyncMode::Client;
    ReloadMode reload = ReloadMode::Live;

    std::vector<std::string> load_after;
    std::vector<std::string> load_before;

    std::string config;  // the settings schema, relative to the mod folder
    std::vector<keys::Declared> keybinds;

    // The DLL to load on this game build ("" if none applies).
    std::string entry_for(const GameVersion& game) const;
};

struct MetadataResult {
    bool ok = false;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

MetadataResult parse_metadata(const json::Value& root, ModMetadata& out);

// Lowercase letter first, then lowercase letters, digits, '-' or '_'; 2-64 chars.
bool is_valid_mod_id(std::string_view id);

const char* to_string(SyncMode mode);
const char* to_string(ReloadMode mode);

} // namespace tome
