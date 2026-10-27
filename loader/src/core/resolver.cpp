#include "resolver.h"

#include <algorithm>
#include <map>

namespace tome {

const char* to_string(ModState state) {
    switch (state) {
    case ModState::Active:             return "active";
    case ModState::DisabledByUser:     return "disabled";
    case ModState::IncompatibleGame:   return "incompatible game version";
    case ModState::IncompatibleTome:   return "incompatible Tome version";
    case ModState::MissingDependency:  return "missing dependency";
    case ModState::DependencyInactive: return "dependency inactive";
    case ModState::Breaks:             return "breaks another mod";
    case ModState::DuplicateId:        return "duplicate id";
    case ModState::Cycle:              return "dependency cycle";
    }
    return "?";
}

namespace {

bool provides(const ModMetadata& meta, const std::string& id) {
    return std::find(meta.provides.begin(), meta.provides.end(), id) != meta.provides.end();
}

} // namespace

ResolveResult resolve(const std::vector<ResolveInput>& inputs, const ResolveEnvironment& env) {
    const size_t n = inputs.size();
    ResolveResult result;
    result.mods.resize(n);
    auto& mods = result.mods;

    auto set_state = [&](size_t i, ModState state, std::string reason) {
        mods[i].state = state;
        mods[i].reason = std::move(reason);
    };

    // Duplicate ids: neither copy loads, since there's no safe way to pick one.
    std::map<std::string, std::vector<size_t>> by_id;
    for (size_t i = 0; i < n; ++i) by_id[inputs[i].meta->id].push_back(i);
    for (const auto& [id, indices] : by_id) {
        if (indices.size() > 1) {
            for (size_t i : indices) {
                set_state(i, ModState::DuplicateId,
                          "more than one installed mod uses the id \"" + id + "\"");
            }
        }
    }

    for (size_t i = 0; i < n; ++i) {
        if (mods[i].state != ModState::Active) continue;
        const ModMetadata& m = *inputs[i].meta;
        if (!m.game.matches(env.game_version)) {
            const char* alias = game_version_alias(env.game_version);
            set_state(i, ModState::IncompatibleGame,
                      "supports game " + m.game.text() + ", this is " +
                      (alias ? std::string(alias) : env.game_version.to_string()));
        } else if (!m.tome.matches(env.tome_version)) {
            set_state(i, ModState::IncompatibleTome,
                      "needs Tome " + m.tome.text() + ", this is " + env.tome_version.to_string());
        } else if (!inputs[i].user_enabled) {
            set_state(i, ModState::DisabledByUser, "disabled");
        }
    }

    // Find the mod satisfying `dep` among all installed (non-duplicate) mods.
    auto find_provider = [&](const Dependency& dep, bool active_only) -> long {
        auto it = by_id.find(dep.id);
        if (it != by_id.end() && it->second.size() == 1) {
            size_t j = it->second[0];
            if (dep.range.matches(inputs[j].meta->version) && (!active_only || mods[j].state == ModState::Active)) {
                return static_cast<long>(j);
            }
        }
        // Virtual ids ("provides") can satisfy only version-agnostic dependencies.
        if (dep.range.any()) {
            for (size_t j = 0; j < n; ++j) {
                if (provides(*inputs[j].meta, dep.id) && (!active_only || mods[j].state == ModState::Active)) {
                    return static_cast<long>(j);
                }
            }
        }
        return -1;
    };

    // Deactivating one mod can deactivate its dependents, so iterate to a fixed point.
    auto settle_dependencies = [&] {
        for (bool changed = true; changed;) {
            changed = false;
            for (size_t i = 0; i < n; ++i) {
                if (mods[i].state != ModState::Active) continue;
                for (const Dependency& dep : inputs[i].meta->depends) {
                    if (find_provider(dep, true) >= 0) continue;
                    long installed = find_provider(dep, false);
                    if (installed >= 0) {
                        const ResolvedMod& other = mods[static_cast<size_t>(installed)];
                        set_state(i, ModState::DependencyInactive,
                                  "needs \"" + dep.id + "\", which is " +
                                  (other.state == ModState::DisabledByUser ? std::string("disabled")
                                                                           : "unable to load (" + other.reason + ")"));
                    } else {
                        auto it = by_id.find(dep.id);
                        if (it != by_id.end() && it->second.size() == 1) {
                            set_state(i, ModState::MissingDependency,
                                      "needs \"" + dep.id + "\" " + dep.range.text() + ", but version " +
                                      inputs[it->second[0]].meta->version.to_string() + " is installed");
                        } else {
                            set_state(i, ModState::MissingDependency,
                                      "needs \"" + dep.id + "\" " + dep.range.text() + ", which isn't installed");
                        }
                    }
                    changed = true;
                    break;
                }
            }
        }
    };

    // Topological sort of the active mods: dependencies and "load.after"
    // targets first. Ties are broken by id so the order is the same on every
    // machine. Mods it can't order are part of (or behind) a cycle: they're
    // set to Cycle, and the result says whether there were any.
    SemVerRange any_version;
    std::string unused;
    parse_semver_range("*", any_version, unused);
    auto active_index = [&](const std::string& id) -> long {
        return find_provider(Dependency{id, any_version}, true);
    };
    auto sort_active = [&]() -> bool {
        result.load_order.clear();
        std::vector<std::vector<size_t>> edges(n);  // edges[a] contains b: a loads before b
        std::vector<int> indegree(n, 0);
        auto add_edge = [&](size_t before, size_t after) {
            if (before == after) return;
            if (std::find(edges[before].begin(), edges[before].end(), after) != edges[before].end()) return;
            edges[before].push_back(after);
            ++indegree[after];
        };
        for (size_t i = 0; i < n; ++i) {
            if (mods[i].state != ModState::Active) continue;
            const ModMetadata& m = *inputs[i].meta;
            for (const Dependency& dep : m.depends) {
                long j = find_provider(dep, true);
                if (j >= 0) add_edge(static_cast<size_t>(j), i);
            }
            for (const std::string& id : m.load_after) {
                long j = active_index(id);
                if (j >= 0) add_edge(static_cast<size_t>(j), i);
            }
            for (const std::string& id : m.load_before) {
                long j = active_index(id);
                if (j >= 0) add_edge(i, static_cast<size_t>(j));
            }
        }

        auto by_mod_id = [&](size_t a, size_t b) { return inputs[a].meta->id > inputs[b].meta->id; };
        std::vector<size_t> ready;  // kept as a min-heap on id
        for (size_t i = 0; i < n; ++i) {
            if (mods[i].state == ModState::Active && indegree[i] == 0) ready.push_back(i);
        }
        std::make_heap(ready.begin(), ready.end(), by_mod_id);
        while (!ready.empty()) {
            std::pop_heap(ready.begin(), ready.end(), by_mod_id);
            size_t i = ready.back();
            ready.pop_back();
            result.load_order.push_back(i);
            for (size_t j : edges[i]) {
                if (--indegree[j] == 0) {
                    ready.push_back(j);
                    std::push_heap(ready.begin(), ready.end(), by_mod_id);
                }
            }
        }

        std::vector<size_t> stuck;
        for (size_t i = 0; i < n; ++i) {
            if (mods[i].state == ModState::Active &&
                std::find(result.load_order.begin(), result.load_order.end(), i) == result.load_order.end()) {
                stuck.push_back(i);
            }
        }
        if (stuck.empty()) return false;
        std::vector<std::string> ids;
        for (size_t i : stuck) ids.push_back(inputs[i].meta->id);
        std::sort(ids.begin(), ids.end());
        std::string list;
        for (const std::string& id : ids) {
            if (!list.empty()) list += ", ";
            list += id;
        }
        for (size_t i : stuck) set_state(i, ModState::Cycle, "load order cycle among: " + list);
        return true;
    };

    // "breaks": a mod that can't run together with an active mod stays off,
    // one at a time, judged only against mods that really run, so folder
    // order doesn't matter: dependencies are settled first, and a mod whose
    // target may still go (it breaks a mod itself, or depends on one that
    // does) waits until that's decided. Cycles are decided before that: a
    // mod in one can't run whatever "breaks" says, so it breaks nothing and
    // nothing has to give way to it.
    for (;;) {
        settle_dependencies();
        if (sort_active()) continue;
        std::vector<std::vector<size_t>> targets(n);  // the active mods each active mod breaks
        for (size_t i = 0; i < n; ++i) {
            if (mods[i].state != ModState::Active) continue;
            for (const Dependency& dep : inputs[i].meta->breaks) {
                long other = find_provider(dep, true);
                if (other >= 0 && static_cast<size_t>(other) != i) targets[i].push_back(static_cast<size_t>(other));
            }
        }
        std::vector<bool> may_go(n, false);
        for (size_t i = 0; i < n; ++i) may_go[i] = !targets[i].empty();
        for (bool grew = true; grew;) {
            grew = false;
            for (size_t i = 0; i < n; ++i) {
                if (may_go[i] || mods[i].state != ModState::Active) continue;
                for (const Dependency& dep : inputs[i].meta->depends) {
                    long j = find_provider(dep, true);
                    if (j >= 0 && may_go[static_cast<size_t>(j)]) {
                        may_go[i] = grew = true;
                        break;
                    }
                }
            }
        }
        long off = -1, because = -1;
        for (size_t i = 0; i < n && off < 0; ++i) {
            for (size_t t : targets[i]) {
                if (!may_go[t]) {  // t stays active, so i can't run
                    off = static_cast<long>(i);
                    because = static_cast<long>(t);
                    break;
                }
            }
        }
        // Every target may still go (mods breaking each other in a ring): the first one gives way.
        for (size_t i = 0; i < n && off < 0; ++i) {
            if (!targets[i].empty()) {
                off = static_cast<long>(i);
                because = static_cast<long>(targets[i][0]);
            }
        }
        if (off < 0) break;
        const ModMetadata& other = *inputs[static_cast<size_t>(because)].meta;
        set_state(static_cast<size_t>(off), ModState::Breaks,
                  "can't run together with \"" + other.id + "\" " + other.version.to_string());
    }

    // Warnings only; they never stop a mod from loading.
    for (size_t i = 0; i < n; ++i) {
        if (mods[i].state != ModState::Active) continue;
        const ModMetadata& m = *inputs[i].meta;
        for (const Dependency& dep : m.conflicts) {
            long other = find_provider(dep, true);
            if (other >= 0 && static_cast<size_t>(other) != i) {
                mods[i].warnings.push_back("may conflict with \"" + inputs[static_cast<size_t>(other)].meta->id + "\"");
            }
        }
        for (const Dependency& dep : m.recommends) {
            if (find_provider(dep, true) < 0) {
                mods[i].warnings.push_back("recommends \"" + dep.id + "\" " + dep.range.text());
            }
        }
    }

    return result;
}

std::vector<size_t> dependents_of(const std::vector<ResolveInput>& inputs, const ResolveEnvironment& env,
                                  size_t target) {
    std::vector<ResolveInput> without = inputs;
    without[target].user_enabled = false;
    ResolveResult before = resolve(inputs, env);
    ResolveResult after = resolve(without, env);
    std::vector<size_t> result;
    for (size_t i = 0; i < inputs.size(); ++i) {
        if (i != target && before.mods[i].state == ModState::Active &&
            after.mods[i].state == ModState::DependencyInactive) {
            result.push_back(i);
        }
    }
    return result;
}

} // namespace tome
