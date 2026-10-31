#pragma once

// One row of the mod list, as the mod manager reports it: for the log, the
// /tome commands and the mod menu.

#include "json.h"
#include "keys.h"
#include "settings.h"

#include <string>
#include <vector>

namespace tome {

struct ModStatus {
    std::string folder;   // in tome/mods: a folder's name, or a package's file name
    std::string dir;      // where its files are (UTF-8): the folder, or the unpacked package
    bool package = false;  // a .tome
    std::string id;       // empty if tome.mod.json couldn't be read
    std::string name;
    std::string version;
    bool user_enabled = false;
    bool running = false; // enabled right now
    std::string state;    // "active", "disabled", "waiting" (game lock), "approval" (code not allowed), "error", ...
    std::string reason;
    std::string approval;  // state "approval": "new", "changed" or "signer" (mod_state::Need); "" otherwise
    std::vector<std::string> warnings;
    std::string assets_pending;  // why the mod's asset changes are waiting; "" if they aren't
    bool restart_pending = false;  // ...and they wait for a restart
    std::string locked;  // what a multiplayer game's lock does to it (core/game_lock.h, note); "" if nothing

    // For the mod menu's details page.
    std::string description;
    std::string icon;  // tome.mod.json "icon", relative to the mod folder; "" if none or its files aren't allowed yet
    std::vector<std::string> authors;
    std::string sync;    // "client" / "required"; "" if the metadata is invalid
    std::string sync_hash;  // a running "required" mod: its identity in the multiplayer handshake (8 hex digits)
    std::string code_hash;  // a mod with code: SHA-256 of its files (core/mod_state.h); "" otherwise
    std::string signer;     // the key of its valid tome.sig (core/signature.h, base64); "" if not signed
    std::string code_kind;  // "native" (a DLL), "scripts" (game scripts among its assets), "wasm" (a WebAssembly module), "assets" (other game files) or "" (none)
    std::string reload;  // "live" / "map" / "restart"
    std::vector<std::string> errors;  // every problem with tome.mod.json, in full
    std::string crash_calls;  // after a crash: where it was called from (likely), as logged

    // Settings (core/settings.h): the schema and the current values. A
    // schema that failed to parse leaves `settings` empty and says why in
    // `settings_error`.
    settings::Schema settings;
    json::Value settings_values;
    std::string settings_error;
    std::vector<keys::Declared> keybinds;
};

} // namespace tome
