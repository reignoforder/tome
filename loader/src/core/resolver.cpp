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

bool is_any_range(const SemVerRange& range) {
    return range.text().empty() || range.text() == "*";
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
        if (is_any_range(dep.range)) {
            for (size_t j = 0; j < n; ++j) {
                if (provides(*inputs[j].meta, dep.id) && (!active_only || mods[j].state == ModState::Active)) {
                    return static_cast<long>(j);
                }
            }
        }
        return -1;
    };

    // Deactivating one mod can deactivate its dependents, so iterate to a fixed point.
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t i = 0; i < n; ++i) {
            if (mods[i].state != ModState::Active) continue;
            const ModMetadata& m = *inputs[i].meta;

            for (const Dependency& dep : m.depends) {
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
            if (mods[i].state != ModState::Active) continue;

            for (const Dependency& dep : m.breaks) {
                long other = find_provider(dep, true);
                if (other >= 0 && static_cast<size_t>(other) != i) {
                    set_state(i, ModState::Breaks,
                              "can't run together with \"" + inputs[static_cast<size_t>(other)].meta->id + "\" " +
                                  inputs[static_cast<size_t>(other)].meta->version.to_string());
                    changed = true;
                    break;
                }
            }
        }
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

    // Topological sort: dependencies and "load.after" targets first. Ties are
    // broken by id so the order is the same on every machine.
    std::vector<std::vector<size_t>> edges(n);  // edges[a] contains b: a loads before b
    std::vector<int> indegree(n, 0);
    auto add_edge = [&](size_t before, size_t after) {
        if (before == after) return;
        if (std::find(edges[before].begin(), edges[before].end(), after) != edges[before].end()) return;
        edges[before].push_back(after);
        ++indegree[after];
    };
    SemVerRange any_version;
    std::string unused;
    parse_semver_range("*", any_version, unused);
    auto active_index = [&](const std::string& id) -> long {
        return find_provider(Dependency{id, any_version}, true);
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

    // Anything active but not ordered is part of (or behind) a cycle.
    std::vector<std::string> stuck;
    for (size_t i = 0; i < n; ++i) {
        if (mods[i].state == ModState::Active &&
            std::find(result.load_order.begin(), result.load_order.end(), i) == result.load_order.end()) {
            stuck.push_back(inputs[i].meta->id);
        }
    }
    if (!stuck.empty()) {
        std::sort(stuck.begin(), stuck.end());
        std::string list;
        for (const std::string& id : stuck) {
            if (!list.empty()) list += ", ";
            list += id;
        }
        for (size_t i = 0; i < n; ++i) {
            if (std::find(stuck.begin(), stuck.end(), inputs[i].meta->id) != stuck.end() &&
                mods[i].state == ModState::Active) {
                set_state(i, ModState::Cycle, "load order cycle among: " + list);
            }
        }
    }

    return result;
}

} // namespace tome
