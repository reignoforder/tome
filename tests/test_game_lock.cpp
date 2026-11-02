// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/game_lock.h"
#include "test.h"

using namespace tome;
using namespace tome::game_lock;

TEST(game_lock_applies) {
    CHECK(!applies(false, 0));
    CHECK(!applies(false, 1));  // alone, or against computers
    CHECK(applies(false, 2));
    CHECK(applies(true, 1));  // a replay, whoever played it
    CHECK(applies(true, 0));
}

TEST(game_lock_locked_mods) {
    std::vector<RunningMod> running = {
        {"lib", false, {}, {}, {}},
        {"api", false, {"lib"}, {"api-v1"}, {}},
        {"req", true, {"api-v1"}, {}, {}},
        {"other", false, {"lib"}, {}, {}},
        {"ui", false, {"req"}, {}, {}},
    };
    // req, and what it needs: api (by "provides"), then lib through api.
    // Its dependents and unrelated client mods stay free.
    CHECK(locked_mods(running) == std::set<std::string>({"req", "api", "lib"}));

    for (RunningMod& m : running) m.required = false;
    CHECK(locked_mods(running).empty());
    CHECK(locked_mods({}).empty());

    // A dependency that isn't running (shouldn't happen) is simply skipped.
    CHECK(locked_mods({{"req", true, {"gone"}, {}, {}}}) == std::set<std::string>({"req"}));
}

TEST(game_lock_note) {
    CHECK(note(false, false, true, false).empty());
    CHECK(note(false, false, false, true).empty());
    CHECK_EQ(note(true, false, true, false), std::string("locked during this multiplayer game"));
    CHECK_EQ(note(true, false, false, false),
             std::string("locked during this multiplayer game; turns off when it ends"));
    CHECK_EQ(note(true, false, true, true),
             std::string("locked during this multiplayer game; changes apply when it ends"));
    CHECK_EQ(note(false, true, true, false), std::string("starts when the multiplayer game ends"));
}

namespace {

Candidate mod(const char* id, bool required = false, std::vector<std::string> depends = {},
              std::vector<std::string> provides = {}) {
    Candidate c;
    c.id = id;
    c.required = required;
    c.depends = std::move(depends);
    c.provides = std::move(provides);
    return c;
}

std::vector<std::string> started(const std::vector<Candidate>& mods, const StartPlan& plan) {
    std::vector<std::string> ids;
    for (size_t i : plan.start) ids.push_back(mods[i].id);
    return ids;
}

using Ids = std::vector<std::string>;

} // namespace

TEST(game_lock_plan_without_a_game) {
    std::vector<Candidate> mods = {mod("lib"), mod("req", true, {"lib"}), mod("ui", false, {"req"})};
    StartPlan plan = plan_start(mods, false, {});
    CHECK(started(mods, plan) == Ids({"lib", "req", "ui"}));
    CHECK(plan.held.empty());

    // A DLL known to fail takes its dependents (by id or "provides") along.
    mods = {mod("lib", false, {}, {"api"}), mod("user", false, {"api"}), mod("free")};
    mods[0].fails = true;
    plan = plan_start(mods, false, {});
    CHECK(started(mods, plan) == Ids({"free"}));
    CHECK(plan.held.empty());
}

TEST(game_lock_plan_holds_new_required_mods) {
    std::vector<Candidate> mods = {mod("locked-req", true), mod("new-req", true), mod("ui", false, {"new-req"}),
                                   mod("client"), mod("ui2", false, {"locked-req"})};
    mods[0].running = true;
    StartPlan plan = plan_start(mods, true, {"locked-req"});
    CHECK(started(mods, plan) == Ids({"locked-req", "client", "ui2"}));
    CHECK(plan.held == std::set<std::string>({"new-req", "ui"}));
}

TEST(game_lock_plan_shows_failures_rather_than_waiting) {
    // A required mod that crashed mid-game (no longer locked) or whose DLL
    // failed: it shows its error, it doesn't wait for the game to end.
    std::vector<Candidate> mods = {mod("crashed", true), mod("ui", false, {"crashed"})};
    mods[0].fails = true;
    StartPlan plan = plan_start(mods, true, {});
    CHECK(plan.start.empty());
    CHECK(plan.held.empty());
}

TEST(game_lock_plan_keeps_running_mods) {
    // A client mod that runs already, through a "provides" a held mod shares.
    std::vector<Candidate> mods = {mod("bars-a", false, {}, {"hp-bars"}), mod("bars-b", true, {}, {"hp-bars"}),
                                   mod("user", false, {"hp-bars"})};
    mods[0].running = true;
    mods[2].running = true;
    StartPlan plan = plan_start(mods, true, {});
    CHECK(started(mods, plan) == Ids({"bars-a", "user"}));
    CHECK(plan.held == std::set<std::string>({"bars-b"}));

    // Enabled mid-game instead: bars-a, which runs, meets its need.
    mods[2].running = false;
    plan = plan_start(mods, true, {});
    CHECK(started(mods, plan) == Ids({"bars-a", "user"}));

    // Without another provider it waits.
    mods[0].running = false;
    mods[0].required = true;
    plan = plan_start(mods, true, {});
    CHECK(plan.start.empty());
    CHECK(plan.held == std::set<std::string>({"bars-a", "bars-b", "user"}));
}

TEST(game_lock_waits) {
    // While the lock holds, a mod that isn't running waits if it's required,
    // blocked (a locked mod runs from its folder) or broken by a locked mod,
    // or if it could meet what a locked mod needs: a locked mod mustn't come
    // to rely on a mod that may still stop before the game ends.
    Newcomer m;
    m.id = "xm";
    m.provides = {"lib"};
    CHECK(waits(m, {"lib"}));     // provides what a locked mod needs
    CHECK(!waits(m, {"other"}));
    m.id = "lib";
    m.provides.clear();
    CHECK(waits(m, {"lib"}));     // by id
    m.id = "free";
    CHECK(!waits(m, {"lib"}));
    m.required = true;
    CHECK(waits(m, {}));
    m.required = false;
    m.blocked = true;
    CHECK(waits(m, {}));
    m.blocked = false;
    m.broken = true;
    CHECK(waits(m, {}));
}

TEST(game_lock_versioned_dependencies) {
    // "provides" only meets a dependency on any version, as in resolve():
    // am needs "lib" >=2, which bm's "provides": ["lib"] can't meet.
    std::vector<RunningMod> running = {{"am", true, {"lib"}, {}, {"lib"}}, {"lib", false, {}, {}, {}},
                                       {"bm", false, {}, {"lib"}, {}}};
    CHECK(locked_mods(running) == std::set<std::string>({"am", "lib"}));
    running[0].versioned.clear();  // any version: bm could meet it too
    CHECK(locked_mods(running) == std::set<std::string>({"am", "bm", "lib"}));

    // cm needs "lib" >=2; lib (required) waits for the game's end, and bm,
    // which runs, can't stand in for it: cm waits too.
    std::vector<Candidate> mods = {mod("bm", false, {}, {"lib"}), mod("lib", true), mod("cm", false, {"lib"})};
    mods[0].running = true;
    mods[2].versioned = {"lib"};
    StartPlan plan = plan_start(mods, true, {});
    CHECK(started(mods, plan) == Ids({"bm"}));
    CHECK(plan.held == std::set<std::string>({"lib", "cm"}));
}

TEST(game_lock_plan_counts_running_providers_in_any_order) {
    // hud needs "hp-bars": bars-req (required, enabled mid-game) waits, but
    // zz-bars, which runs, provides it, though the load order puts it later.
    std::vector<Candidate> mods = {mod("bars-req", true, {}, {"hp-bars"}), mod("hud", false, {"hp-bars"}),
                                   mod("zz-bars", false, {}, {"hp-bars"})};
    mods[2].running = true;
    StartPlan plan = plan_start(mods, true, {});
    CHECK(started(mods, plan) == Ids({"hud", "zz-bars"}));
    CHECK(plan.held == std::set<std::string>({"bars-req"}));
}
