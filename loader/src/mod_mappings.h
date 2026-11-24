// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Mods' own mapping files (tome.mod.json "mappings", core/mappings.h
// parse_mod_build): the symbols each installed mod maps for the running
// build, named "<mod id>/<name>". The mod manager publishes them after each
// scan; the hook manager and resolve look names up here, next to Tome's own
// mappings. A published table stays in memory for the session (lookups hand
// out pointers into it, and mods keep resolve's tome_symbol*): a new one is
// made only when a mod's file changes.

#include "core/mappings.h"
#include "tome/tome.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tome::mod_mappings {

// A mapping symbol as the mod API's resolve returns it, named `name`
// (absolute addresses from `base`).
tome_symbol to_api_symbol(const mappings::Symbol& sym, const char* name, uintptr_t base);

// The running game.dll's base, for resolve's absolute addresses.
void init(uintptr_t game_base);

// Replaces the symbols of mod `id` with `build`'s; null removes them. The
// same table again changes nothing, and one published before is reused.
// `withheld`, with a null build: why the mod's file isn't published (the
// mod is off, or not allowed yet), for hook errors (missing_reason).
void publish(const std::string& id, std::shared_ptr<const mappings::Build> build, std::string withheld = {});

// Why "<mod id>/<name>" isn't published although that mod has a file: the
// reason publish was given; empty if there's none.
std::string missing_reason(std::string_view name);

// The mods that have symbols published.
std::vector<std::string> ids();

// A symbol by "<mod id>/<name>"; null if there's none. Stays valid.
const mappings::Symbol* find(std::string_view name);

// The same as the mod API's tome_symbol (absolute addresses). Stays valid.
const tome_symbol* resolve(std::string_view name);

// Whether verify can check files: init ran (or set_verifier set a check).
// Before (the startup assets' scan in DllMain), the mod manager reads
// files but doesn't check or publish them yet.
bool ready();

// Checks a mod's file against the running game.dll as it was before Tome
// patched it (patch.h note_original): every function's "bytes", every
// call's target, every import slot's import, as Tome's own mapping is
// checked at startup. One message per mismatch; empty if it matches (or
// it can't check yet: ready() is false). The mod manager publishes only
// files that pass, so what resolve hands out was checked. Runs the check
// set_verifier set, if any, instead.
std::vector<std::string> verify(const mappings::Build& build);

// verify's own check, whatever set_verifier set.
std::vector<std::string> check_code(const mappings::Build& build);

// Replaces verify's check (null: check_code again); it runs under no lock
// of ours. The fake game (testing/fake_game.h) writes the file's code into
// its image first.
void set_verifier(std::function<std::vector<std::string>(const mappings::Build&)> fn);

} // namespace tome::mod_mappings
