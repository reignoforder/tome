#include "game_lock.h"

#include <algorithm>

namespace tome::game_lock {

bool applies(bool replay, int human_players) {
    return replay || human_players > 1;
}

namespace {

// Whether a mod (`id`, `provides`) meets dependency `dep`: by id, or by
// "provides" if the dependency takes any version, as resolve() decides.
bool meets(const std::string& id, const std::vector<std::string>& provides, const std::string& dep, bool any_version) {
    return id == dep || (any_version && std::find(provides.begin(), provides.end(), dep) != provides.end());
}

bool any_version(const std::vector<std::string>& versioned, const std::string& dep) {
    return std::find(versioned.begin(), versioned.end(), dep) == versioned.end();
}

} // namespace

std::set<std::string> locked_mods(const std::vector<RunningMod>& running) {
    std::set<std::string> locked;
    for (const RunningMod& m : running) {
        if (m.required) locked.insert(m.id);
    }
    for (bool grew = true; grew;) {
        grew = false;
        for (const RunningMod& m : running) {
            if (!locked.count(m.id)) continue;
            for (const std::string& dep : m.depends) {
                const bool any = any_version(m.versioned, dep);
                for (const RunningMod& other : running) {
                    if (meets(other.id, other.provides, dep, any) && locked.insert(other.id).second) grew = true;
                }
            }
        }
    }
    return locked;
}

bool waits(const Newcomer& mod, const std::set<std::string>& locked_needs) {
    if (mod.required || mod.blocked || mod.broken) return true;
    return std::any_of(locked_needs.begin(), locked_needs.end(),
                       [&](const std::string& need) { return meets(mod.id, mod.provides, need, true); });
}

StartPlan plan_start(const std::vector<Candidate>& mods, bool lock_on, const std::set<std::string>& locked) {
    StartPlan plan;
    // Ids and "provides" names: what can't be met by a failed or held mod.
    std::set<std::string> failed, held;
    auto add = [](std::set<std::string>& to, const Candidate& m) {
        to.insert(m.id);
        to.insert(m.provides.begin(), m.provides.end());
    };
    // Running mods keep running, wherever they come in the load order (the
    // resolver orders a dependent only after the provider it picked).
    std::vector<size_t> running;
    for (size_t i = 0; i < mods.size(); ++i) {
        if (mods[i].running) running.push_back(i);
    }
    for (size_t i = 0; i < mods.size(); ++i) {
        const Candidate& m = mods[i];
        bool fails = m.fails || std::any_of(m.depends.begin(), m.depends.end(),
                                            [&](const std::string& dep) { return failed.count(dep) > 0; });
        if (fails) {
            add(failed, m);
            continue;
        }
        if (lock_on && !m.running && !locked.count(m.id)) {
            auto met = [&](const std::string& dep) {
                const bool any = any_version(m.versioned, dep);
                auto meets_dep = [&](size_t j) { return meets(mods[j].id, mods[j].provides, dep, any); };
                return std::any_of(plan.start.begin(), plan.start.end(), meets_dep) ||
                       std::any_of(running.begin(), running.end(), meets_dep);
            };
            bool hold = m.required || std::any_of(m.depends.begin(), m.depends.end(), [&](const std::string& dep) {
                            return held.count(dep) > 0 && !met(dep);
                        });
            if (hold) {
                add(held, m);
                plan.held.insert(m.id);
                continue;
            }
        }
        plan.start.push_back(i);
    }
    return plan;
}

std::string note(bool locked, bool held, bool user_enabled, bool changed) {
    if (held) return "starts when the multiplayer game ends";
    if (!locked) return {};
    std::string text = "locked during this multiplayer game";
    if (!user_enabled) return text + "; turns off when it ends";
    if (changed) return text + "; changes apply when it ends";
    return text;
}

} // namespace tome::game_lock
