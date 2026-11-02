// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// WebAssembly mods (docs/wasm-mods.md): a module run by WAMR's interpreter
// (third_party/wamr) that can only call Tome's imports (module "tome"),
// every argument checked by core/wasm_abi. What Tome hands the module is
// copied into its memory (core/wasm_layout); game objects become handles,
// and frame handles are looked up by name on every use, so a module never
// holds a pointer. The mod manager owns one Instance per loaded Wasm mod and
// calls its lifecycle the way it calls a DLL's, under the crash guard.
//
// A trap (out of bounds, unreachable, the instruction budget or the call's
// time limit running out, a call stack too deep) is logged and handed to the guard as a mod
// failure (TOME_EXCEPTION_MOD_FAILED): the call is abandoned and the mod
// disabled, as for a crashed DLL. Only from the outermost call into the
// module: a trap inside a callback the module's own import call caused
// stays the module's exception, so the outer call traps too.

#include "core/wasm_imports.h"
#include "mod_handle.h"

#include <cstdint>
#include <memory>
#include <string>

namespace tome::wasm_mods {

// A module's limits (core/wasm_imports.h).
using wasm::kCallTimeLimitMs;
using wasm::kInstructionBudget;
using wasm::kMaxMemoryPages;
using wasm::kMaxModuleSize;
using wasm::kStackSize;

class Instance {
public:
    ~Instance();

    // Loads `path` and instantiates it for `mod`: nullptr, with `error`, if
    // it isn't a valid module, imports something Tome doesn't provide (or
    // anything but functions from "tome"), or an export Tome calls has the
    // wrong type.
    static std::unique_ptr<Instance> load(tome_mod* mod, const std::wstring& path, std::string& error);

    // The lifecycle exports: tome_mod_load() -> i32 (0 is success) is
    // required; tome_mod_enable/disable/unload() are optional. Call each
    // under guard::run: a trap raises TOME_EXCEPTION_MOD_FAILED.
    int32_t call_load();
    bool has(const char* lifecycle_export) const;
    void call(const char* lifecycle_export);

    // Enabling: frees the registrations the last disable revoked (and calls
    // the optional tome_callbacks_revoked() so the module frees its side),
    // then calls tome_mod_enable if it's exported. Under guard::run.
    void enable();
    // After the mod's registrations were all revoked (a disable).
    void revoked();

    struct Impl;

private:
    explicit Instance(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace tome::wasm_mods
