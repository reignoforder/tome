#include "core/assets.h"
#include "test.h"

using namespace tome;
using namespace tome::assets;

namespace {

ModAssets mod(const char* id, ReloadMode reload, uint64_t fingerprint) {
    return {id, reload, fingerprint};
}

std::vector<std::string> ids(const Plan& p) {
    std::vector<std::string> out;
    for (const ModAssets& m : p.mount) out.push_back(m.id);
    return out;
}

} // namespace

TEST(assets_archive_path) {
    CHECK_EQ(archive_path("Units/Human/Footman/Footman.blp"), std::string("Units\\Human\\Footman\\Footman.blp"));
    CHECK_EQ(archive_path("UI\\FrameDef\\x.fdf"), std::string("UI\\FrameDef\\x.fdf"));
    CHECK_EQ(archive_path("war3mapMap.blp"), std::string("war3mapMap.blp"));
    CHECK(archive_path("").empty());
    CHECK(archive_path("a//b").empty());
    CHECK(archive_path("a/../b").empty());
    CHECK(archive_path("./a").empty());
    CHECK(archive_path("a/").empty());
    CHECK(archive_path("C:/x").empty());
    CHECK(archive_path("a\tb").empty());
    CHECK(archive_path("(listfile)").empty());
    CHECK(archive_path("(Attributes)").empty());
}

TEST(assets_plan_startup_applies_everything) {
    Plan p = plan({}, {mod("a", ReloadMode::Restart, 1), mod("b", ReloadMode::Map, 2)}, Moment::Startup);
    CHECK(p.changed);
    CHECK(p.pending.empty());
    CHECK(ids(p) == std::vector<std::string>({"a", "b"}));
}

TEST(assets_plan_unchanged) {
    std::vector<ModAssets> mounted = {mod("a", ReloadMode::Live, 1)};
    Plan p = plan(mounted, mounted, Moment::Refresh);
    CHECK(!p.changed);
    CHECK(p.pending.empty());
}

TEST(assets_plan_reload_mode_change_needs_no_rebuild) {
    Plan p = plan({mod("a", ReloadMode::Live, 1)}, {mod("a", ReloadMode::Restart, 1)}, Moment::Refresh);
    CHECK(!p.changed);
    CHECK(p.pending.empty());
    CHECK(p.mount[0].reload == ReloadMode::Restart);
}

TEST(assets_plan_live_applies_on_refresh) {
    Plan add = plan({}, {mod("a", ReloadMode::Live, 1)}, Moment::Refresh);
    CHECK(add.changed);
    CHECK(ids(add) == std::vector<std::string>({"a"}));

    Plan remove = plan({mod("a", ReloadMode::Live, 1)}, {}, Moment::Refresh);
    CHECK(remove.changed);
    CHECK(remove.mount.empty());

    Plan edit = plan({mod("a", ReloadMode::Live, 1)}, {mod("a", ReloadMode::Live, 2)}, Moment::Refresh);
    CHECK(edit.changed);
    CHECK_EQ(edit.mount[0].fingerprint, uint64_t{2});
}

TEST(assets_plan_map_waits_for_main_menu) {
    std::vector<ModAssets> mounted = {mod("a", ReloadMode::Map, 1)};
    Plan during_game = plan(mounted, {}, Moment::Refresh);
    CHECK(!during_game.changed);
    CHECK(ids(during_game) == std::vector<std::string>({"a"}));
    CHECK_EQ(during_game.pending.size(), size_t{1});
    CHECK(during_game.pending[0].reload == ReloadMode::Map);

    Plan at_menu = plan(mounted, {}, Moment::MainMenu);
    CHECK(at_menu.changed);
    CHECK(at_menu.mount.empty());
    CHECK(at_menu.pending.empty());

    // Or when the next map starts loading.
    Plan at_load = plan(mounted, {}, Moment::MapLoad);
    CHECK(at_load.changed);
    CHECK(at_load.mount.empty());

    // An edited file keeps the old version mounted until then.
    Plan edit = plan(mounted, {mod("a", ReloadMode::Map, 2)}, Moment::Refresh);
    CHECK(!edit.changed);
    CHECK_EQ(edit.mount[0].fingerprint, uint64_t{1});
}

TEST(assets_plan_restart_waits_for_next_launch) {
    Plan add = plan({}, {mod("a", ReloadMode::Restart, 1)}, Moment::MainMenu);
    CHECK(!add.changed);
    CHECK(add.mount.empty());
    CHECK_EQ(add.pending.size(), size_t{1});

    Plan remove = plan({mod("a", ReloadMode::Restart, 1)}, {}, Moment::MainMenu);
    CHECK(!remove.changed);
    CHECK(!plan({}, {mod("a", ReloadMode::Restart, 1)}, Moment::MapLoad).changed);
    CHECK(ids(remove) == std::vector<std::string>({"a"}));
}

TEST(assets_plan_keeps_load_order_and_held_mods) {
    // "held" can't be removed yet; it stays after the mod it followed.
    std::vector<ModAssets> mounted = {mod("x", ReloadMode::Live, 1), mod("held", ReloadMode::Restart, 5)};
    std::vector<ModAssets> wanted = {mod("x", ReloadMode::Live, 1), mod("y", ReloadMode::Live, 3)};
    Plan p = plan(mounted, wanted, Moment::Refresh);
    CHECK(p.changed);
    CHECK(ids(p) == std::vector<std::string>({"x", "held", "y"}));
    CHECK_EQ(p.pending.size(), size_t{1});
    CHECK_EQ(p.pending[0].id, std::string("held"));

    // Holding the last mod back changes nothing, so there's nothing to rebuild.
    Plan same = plan(mounted, {mod("x", ReloadMode::Live, 1)}, Moment::Refresh);
    CHECK(!same.changed);
    CHECK(ids(same) == std::vector<std::string>({"x", "held"}));

    // With no predecessor left, it goes first.
    Plan first = plan({mod("held", ReloadMode::Map, 5), mod("x", ReloadMode::Live, 1)}, {mod("x", ReloadMode::Live, 1)},
                      Moment::Refresh);
    CHECK(!first.changed);
    CHECK(ids(first) == std::vector<std::string>({"held", "x"}));
}

TEST(assets_plan_frozen_mods_keep_what_they_have) {
    // Locked for a multiplayer game: new files, even live ones, wait.
    std::vector<ModAssets> mounted = {mod("a", ReloadMode::Live, 1), mod("req", ReloadMode::Live, 2)};
    std::set<std::string> frozen = {"req"};
    Plan changed = plan(mounted, {mod("a", ReloadMode::Live, 1), mod("req", ReloadMode::Live, 3)}, Moment::Refresh,
                        frozen);
    CHECK(!changed.changed);
    CHECK(changed.pending.empty());
    CHECK_EQ(changed.mount[1].fingerprint, uint64_t{2});

    // Turned off: it stays mounted in its place, not listed as pending.
    Plan off = plan(mounted, {mod("a", ReloadMode::Live, 1)}, Moment::Refresh, frozen);
    CHECK(!off.changed);
    CHECK(off.pending.empty());
    CHECK(ids(off) == std::vector<std::string>({"a", "req"}));

    // A frozen mod with nothing mounted gets nothing; others still apply.
    Plan none = plan(mounted, {mod("a", ReloadMode::Live, 4), mod("new", ReloadMode::Live, 5)}, Moment::Refresh,
                     {"a", "req", "new"});
    CHECK(!none.changed);
    CHECK(ids(none) == std::vector<std::string>({"a", "req"}));
    Plan other = plan(mounted, {mod("a", ReloadMode::Live, 4), mod("req", ReloadMode::Live, 2)}, Moment::Refresh,
                      frozen);
    CHECK(other.changed);
    CHECK_EQ(other.mount[0].fingerprint, uint64_t{4});
}
