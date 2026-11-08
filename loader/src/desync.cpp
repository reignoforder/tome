// SPDX-License-Identifier: GPL-3.0-or-later
#include "desync.h"

#include "core/desync.h"
#include "core/handshake.h"
#include "core/log_guard.h"
#include "events.h"
#include "guard.h"
#include "handshake.h"
#include "log.h"
#include "mod_handle.h"
#include "mod_manager.h"
#include "units.h"
#include "unwind.h"

#include <windows.h>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace tome::desync_api {
namespace {

struct Registration {
    tome_mod* mod;
    tome_checksum_fn fn;
    void* user;
};

// Registration may happen on any thread (tome_mod_enable); the rest is the
// main thread's. Callbacks run with the lock released.
SRWLOCK g_lock = SRWLOCK_INIT;
std::map<std::string, Registration> g_registered;  // by mod id: the order every computer calls them in
std::set<const tome_mod*> g_revoking;

desync::Checker g_checker;
// Other players' malformed actions, logged a few at a time.
handshake::LogBudget g_bad_log(10);

tome_result TOME_CALL api_on_checksum(tome_mod* mod, tome_checksum_fn fn, void* user) {
    if (!mod) return TOME_ERR_INVALID_ARGUMENT;
    ExclusiveLock hold(g_lock);
    if (!fn) {
        g_registered.erase(mod->id);
        return TOME_OK;
    }
    g_registered[mod->id] = {mod, fn, user};
    if (g_revoking.insert(mod).second) {
        mod->add_revoker([mod] {
            ExclusiveLock hold(g_lock);
            auto it = g_registered.find(mod->id);
            if (it != g_registered.end() && it->second.mod == mod) g_registered.erase(it);
            g_revoking.erase(mod);
        });
    }
    return TOME_OK;
}

// "player 1 (Guest)": the slot as the log's other lines give it, and the name.
std::string player_label(int slot) {
    const std::string name = units::player_name(slot);
    return "player " + std::to_string(slot) + (name.empty() ? "" : " (" + name + ")");
}

// For the player: the name, or "Player 2" (the game's numbering).
std::string player_name_for_screen(int slot) {
    const std::string name = units::player_name(slot);
    return name.empty() ? "Player " + std::to_string(slot + 1) : name;
}

void report(const std::vector<desync::Comparison>& found, bool replay) {
    for (const desync::Comparison& c : found) {
        const bool first = g_checker.first_report(c.player, c.mod_id, c.same());
        const std::string who = player_label(c.player);
        const std::string when = desync::format_game_time(game_clock::to_seconds(c.round * desync::kInterval));
        if (c.same()) {
            // The first match says the checks run; the rest are quiet.
            if (first) {
                TOME_INFO("Desync check: %s's state matches %s's (round %u, at %s of game time)", c.mod_id.c_str(),
                          who.c_str(), c.round, when.c_str());
            }
            continue;
        }
        if (!first) {
            TOME_DEBUG("Desync check: %s still differs from %s's in round %u (%016llx, theirs %016llx)",
                       c.mod_id.c_str(), who.c_str(), c.round, static_cast<unsigned long long>(c.local),
                       static_cast<unsigned long long>(c.remote));
            continue;
        }
        TOME_WARN("Desync check: %s's state differs between %s (%016llx) and %s (%016llx) in round %u, at %s of "
                  "game time; later rounds are only logged at debug level",
                  c.mod_id.c_str(), replay ? "this replay" : "this computer",
                  static_cast<unsigned long long>(c.local), who.c_str(), static_cast<unsigned long long>(c.remote),
                  c.round, when.c_str());
        const std::string screen = player_name_for_screen(c.player);
        const std::string text =
            replay ? c.mod_id + " plays this replay differently from the recorded game (" + screen + "'s computer) at " +
                         when + "."
                   : c.mod_id + " differs between you and " + screen + " at " + when +
                         ": the game may go out of sync. Details in tome.log.";
        if (events::display_text("|cffffcc00Tome:|r " + text, 20.0f)) TOME_INFO("Desync check: told the player");
    }
}

void run_round(uint32_t round, bool replay) {
    std::vector<Registration> due;
    {
        SharedLock hold(g_lock);
        for (const auto& [id, r] : g_registered) {
            // Only mods every player has: the handshake makes sure of those.
            if (r.mod->sync_required && !r.mod->faulted) due.push_back(r);
        }
    }
    std::vector<desync::Comparison> found;
    // Dropped if a crash guard further out abandons this round.
    OnUnwind release([&] {
        std::vector<Registration>().swap(due);
        std::vector<desync::Comparison>().swap(found);
    });
    for (const Registration& r : due) {
        uint64_t hash = 0;
        if (!guard::run(r.mod, "checksum callback", nullptr, [&] { hash = r.fn(r.mod, r.user); })) continue;
        g_checker.local(round, r.mod->id, hash, found);
        if (!replay) {
            const desync::Part part = desync::encode(round, r.mod->id, hash);
            if (!part.mission.empty()) handshake::send_sync_action(desync::kCacheFile, part.mission, part.key, 0);
        }
    }
    report(found, replay);
}

} // namespace

void fill_api(tome_api& api) {
    api.on_checksum = api_on_checksum;
}

void on_game_start() {
    g_checker.reset();
    g_bad_log.reset();
}

void on_game_step(game_clock::Micros before, game_clock::Micros now) {
    const uint32_t round = desync::round_ending(before, now);
    if (!round) return;
    // Only where players can differ: a multiplayer game, or the replay of
    // one (the game lock's rule, core/game_lock.h). Not before the players
    // are all in place.
    if (!mod_manager().game_locked() || !handshake::game_stepped()) return;
    {
        SharedLock hold(g_lock);
        if (g_registered.empty()) return;
    }
    run_round(round, handshake::in_replay());
}

void receive(int sender, const std::string& mission, const std::string& key, bool replay) {
    uint32_t round = 0;
    std::string mod_id;
    uint64_t hash = 0;
    if (!desync::parse(mission, key, round, mod_id, hash)) {
        switch (g_bad_log.take(sender)) {
        case handshake::LogBudget::Verdict::Log:
            TOME_WARN("Desync check: player %d sent a malformed checksum (\"%s\"); dropped", sender,
                      log::printable(mission, 100).c_str());
            break;
        case handshake::LogBudget::Verdict::Exhausted:
            TOME_WARN("Desync check: player %d sent too many malformed checksums; not logging the rest", sender);
            break;
        case handshake::LogBudget::Verdict::Quiet: break;
        }
        return;
    }
    // Live, this computer's own came back: it's the one it compares with.
    if (!replay && sender == units::local_slot()) return;
    std::vector<desync::Comparison> found;
    g_checker.remote(sender, round, mod_id, hash, found);
    report(found, replay);
}

} // namespace tome::desync_api
