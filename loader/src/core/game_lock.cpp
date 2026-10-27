#include "game_lock.h"

#include <algorithm>

namespace tome::game_lock {

bool applies(bool replay, int human_players) {
    return replay || human_players > 1;
}

namespace {

bool meets(const std::string& id, const std::vector<std::string>& provides, const std::string& dep) {
    return id == dep || std::find(provides.begin(), provides.end(), dep) != provides.end();
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
                for (const RunningMod& other : running) {
                    if (meets(other.id, other.provides, dep) && locked.insert(other.id).second) grew = true;
                }
            }
        }
    }
    return locked;
}

StartPlan plan_start(const std::vector<Candidate>& mods, bool lock_on, const std::set<std::string>& locked) {
    StartPlan plan;
    // Ids and "provides" names: what can't be met by a failed or held mod.
    std::set<std::string> failed, held;
    auto add = [](std::set<std::string>& to, const Candidate& m) {
        to.insert(m.id);
        to.insert(m.provides.begin(), m.provides.end());
    };
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
                return std::any_of(plan.start.begin(), plan.start.end(), [&](size_t j) {
                    return meets(mods[j].id, mods[j].provides, dep);
                });
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
