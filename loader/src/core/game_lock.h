#pragma once

// Locking "sync": "required" mods during a multiplayer game or replay
// (docs/research.md §5.7). Turning one on or off, reloading it or changing
// its assets mid-game would desync, so the mods that run when the game
// starts keep running until it ends, and the ones that don't stay off.
// Toggles still go into state.json; they apply when the game ends.

#include <set>
#include <string>
#include <vector>

namespace tome::game_lock {

// Whether a game locks required mods: a replay, or more than one person
// playing (a single player game, alone or against computers, can't desync).
bool applies(bool replay, int human_players);

struct RunningMod {
    std::string id;
    bool required = false;
    std::vector<std::string> depends;   // ids
    std::vector<std::string> provides;
};

// The mods that stay running until the game ends: every running required
// mod and, since stopping a dependency stops its dependents, whatever
// running mod they depend on, directly or not (by id or by "provides").
std::set<std::string> locked_mods(const std::vector<RunningMod>& running);

// A mod resolve() lets run, as a refresh sees it.
struct Candidate {
    std::string id;
    bool required = false;
    std::vector<std::string> depends;   // ids
    std::vector<std::string> provides;
    bool fails = false;    // its DLL failed to load before and hasn't changed since
    bool running = false;  // enabled already
};

struct StartPlan {
    std::vector<size_t> start;   // the candidates that run (or keep running), in order
    std::set<std::string> held;  // ids of candidates that wait for the game to end
};

// Which candidates (in load order) a refresh runs: not those that fail to
// load, or that need one that does (they show their error). With `lock_on`,
// not the required mods outside `locked` (locked_mods) that aren't running
// already, nor what needs one of them while no mod that runs meets that
// need: those are held. A running mod always keeps running.
StartPlan plan_start(const std::vector<Candidate>& mods, bool lock_on, const std::set<std::string>& locked);

// What the mod list says about a mod while a game holds the lock; "" if the
// lock doesn't affect it.
//   running, locked:   "locked during this multiplayer game; turns off when it
//                       ends" if the user turned it off, "...; changes apply
//                       when it ends" if its files changed, else "locked during
//                       this multiplayer game"
//   wanted, held off:  "starts when the multiplayer game ends"
std::string note(bool locked, bool held, bool user_enabled, bool changed);

} // namespace tome::game_lock
