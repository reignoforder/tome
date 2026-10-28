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

// A dependency is met by a mod with that id, or by one that "provides" it
// if the dependency takes any version (resolve() matches them the same way).
struct RunningMod {
    std::string id;
    bool required = false;
    std::vector<std::string> depends;   // ids
    std::vector<std::string> provides;
    std::vector<std::string> versioned;  // the depends with a version range: "provides" can't meet those
};

// The mods that stay running until the game ends: every running required
// mod and, since stopping a dependency stops its dependents, whatever
// running mod they depend on, directly or not (by id or by "provides").
std::set<std::string> locked_mods(const std::vector<RunningMod>& running);

// A mod the user wants that isn't running and isn't locked, as a refresh
// sees it while the lock holds, before resolve().
struct Newcomer {
    std::string id;
    bool required = false;
    std::vector<std::string> provides;
    bool blocked = false;  // its folder runs a locked mod under another id (the DLL would run twice)
    bool broken = false;   // a locked mod "breaks" it (as the locked mod was enabled)
};

// Whether it waits for the game to end: a required mod (the game started
// without it), a blocked or broken one, or one that could meet one of
// `locked_needs` (what the locked mods depend on), so a locked mod never
// comes to rely on a mod that may still stop. A refresh leaves such mods out
// of resolve() altogether (as if turned off, for that refresh only): resolved
// as active, they could turn running mods off (one that breaks them, or a
// load order cycle with them) and stand in for dependencies they won't meet.
bool waits(const Newcomer& mod, const std::set<std::string>& locked_needs);

// A mod resolve() lets run, as a refresh sees it.
struct Candidate {
    std::string id;
    bool required = false;
    std::vector<std::string> depends;   // ids
    std::vector<std::string> provides;
    std::vector<std::string> versioned;  // as in RunningMod
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
// already, nor what needs one of them while no mod that runs (wherever it
// comes in the load order) meets that need: those are held. A running mod
// always keeps running. (A refresh leaves out what waits() first, so this
// is a backstop.)
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
