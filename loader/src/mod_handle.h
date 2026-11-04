// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The handle passed to mods (tome_mod in tome.h). Kept apart from the mod
// manager so the guard and the hook engine can use it on their own.

#include "core/hooks.h"
#include "unwind.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Defined at global scope to match the opaque declaration in tome.h.
struct tome_mod {
    std::string id;
    std::string version;
    std::string dir;         // UTF-8, for the API
    std::string config_dir;  // UTF-8, for the API
    std::wstring dir_w;
    std::wstring config_dir_w;  // tome/mods/config/<id>, created on demand

    // Undo actions for everything this mod registered through the API, run in
    // reverse order on disable (docs/research.md §5.4). `tag` identifies one
    // registration, so undoing it early (unhook) can drop its revoker.
    struct Revoker {
        std::function<void()> fn;
        const void* tag = nullptr;
    };
    std::vector<Revoker> revokers;

    // One per hook handler; the DLL is freed only once none of them runs.
    std::vector<std::shared_ptr<tome::hooks::Gate>> hook_gates;

    // Set when one of the mod's callbacks crashed (guard.cpp). Guarded calls
    // skip the mod from then on, until a new DLL is loaded.
    std::atomic<bool> faulted{false};

    // "sync": "required" in tome.mod.json (the last resolve): only such mods
    // may change the game in a multiplayer game (objects.cpp).
    std::atomic<bool> sync_required{false};

    // The thread running the mod's tome_mod_load or tome_mod_enable, or 0.
    // Until that returns, guarded calls from other threads skip the mod
    // (guard.cpp): a handler it registered must not run while the mod is
    // still setting up, e.g. a TICK on the main thread during the first
    // load, which runs on the bootstrap thread.
    std::atomic<unsigned long> starting_on{0};
    std::string fault;        // the first crash, for the log and the mod's status
    std::string fault_calls;  // where it was called from (likely), as logged

    // Guards revokers, hook_gates and fault: hook() may be called from any
    // thread, and a crash may happen on any thread.
    SRWLOCK lock = SRWLOCK_INIT;

    void add_revoker(std::function<void()> fn, const void* tag = nullptr) {
        tome::ExclusiveLock hold(lock);
        revokers.push_back({std::move(fn), tag});
    }

    // Forgets the revokers of registrations that were already undone.
    void drop_revokers(const std::vector<const void*>& tags) {
        tome::ExclusiveLock hold(lock);
        std::erase_if(revokers, [&](const Revoker& r) {
            return r.tag && std::find(tags.begin(), tags.end(), r.tag) != tags.end();
        });
    }

    // Runs every revoker registered so far, newest first, and forgets them.
    void revoke_all() {
        std::vector<Revoker> taken;
        {
            tome::ExclusiveLock hold(lock);
            taken.swap(revokers);
        }
        for (auto it = taken.rbegin(); it != taken.rend(); ++it) it->fn();
    }

    std::vector<std::shared_ptr<tome::hooks::Gate>> take_hook_gates() {
        tome::ExclusiveLock hold(lock);
        std::vector<std::shared_ptr<tome::hooks::Gate>> taken;
        taken.swap(hook_gates);
        return taken;
    }

    // Records the first crash; later ones are only logged.
    void set_fault(const std::string& text, const std::string& calls) {
        tome::ExclusiveLock hold(lock);
        if (!faulted.exchange(true)) {
            fault = text;
            fault_calls = calls;
        }
    }

    std::string fault_text() {
        tome::SharedLock hold(lock);
        return fault;
    }

    std::string fault_calls_text() {
        tome::SharedLock hold(lock);
        return fault_calls;
    }

    // A new DLL starts with a clean record.
    void clear_fault() {
        tome::ExclusiveLock hold(lock);
        faulted = false;
        fault.clear();
        fault_calls.clear();
    }
};
