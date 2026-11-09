// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/mod_list.h"
#include "test.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

using namespace tome;
using namespace tome::mod_list;

namespace {

ModStatus row(const char* id, const char* version, const char* state) {
    ModStatus s;
    s.folder = id;
    s.dir = std::string("C:\\tome\\mods\\") + id;
    s.id = id;
    s.name = std::string("The ") + id;
    s.version = version;
    s.state = state;
    s.user_enabled = std::string(state) == "active";
    s.running = s.user_enabled;
    s.sync = "client";
    return s;
}

} // namespace

TEST(mod_list_state_codes) {
    CHECK_EQ(state_code("active"), TOME_MOD_ACTIVE);
    CHECK_EQ(state_code("disabled"), TOME_MOD_DISABLED);
    CHECK_EQ(state_code("approval"), TOME_MOD_APPROVAL);
    CHECK_EQ(state_code("waiting"), TOME_MOD_WAITING);
    CHECK_EQ(state_code("error"), TOME_MOD_ERROR);
    CHECK_EQ(state_code("something new"), TOME_MOD_ERROR);
}

TEST(mod_list_fills_entries) {
    std::vector<ModStatus> status = {row("alpha", "1.2.0", "active"), row("beta", "0.3.1-rc.1", "approval")};
    status[1].reason = "new code";
    status[1].package = true;
    status[1].folder = "beta.tome";
    status[1].sync = "required";
    status[1].restart_pending = true;
    status[1].provides = {"hp-bars", "mana-bars"};
    Snapshot list(status);
    status.clear();  // the snapshot keeps its own strings

    CHECK_EQ(list.fill(nullptr, sizeof(tome_mod_info), 0), 2);
    tome_mod_info out[4];
    std::memset(out, 0xCC, sizeof(out));
    CHECK_EQ(list.fill(out, sizeof(tome_mod_info), 4), 2);
    CHECK_EQ(out[0].size, static_cast<uint32_t>(sizeof(tome_mod_info)));
    CHECK_EQ(std::string(out[0].id), std::string("alpha"));
    CHECK_EQ(std::string(out[0].name), std::string("The alpha"));
    CHECK_EQ(std::string(out[0].version), std::string("1.2.0"));
    CHECK_EQ(std::string(out[0].folder), std::string("alpha"));
    CHECK_EQ(std::string(out[0].dir), std::string("C:\\tome\\mods\\alpha"));
    CHECK_EQ(out[0].state, TOME_MOD_ACTIVE);
    CHECK_EQ(std::string(out[0].reason), std::string());
    CHECK_EQ(out[0].enabled, 1);
    CHECK_EQ(out[0].package, 0);
    CHECK_EQ(out[0].sync_required, 0);
    CHECK_EQ(out[0].restart_pending, 0);
    CHECK(out[0].provides == nullptr);
    CHECK_EQ(out[0].provides_count, 0);

    CHECK_EQ(std::string(out[1].folder), std::string("beta.tome"));
    CHECK_EQ(out[1].state, TOME_MOD_APPROVAL);
    CHECK_EQ(std::string(out[1].reason), std::string("new code"));
    CHECK_EQ(out[1].enabled, 0);
    CHECK_EQ(out[1].package, 1);
    CHECK_EQ(out[1].sync_required, 1);
    CHECK_EQ(out[1].restart_pending, 1);
    CHECK_EQ(out[1].provides_count, 2);
    CHECK_EQ(std::string(out[1].provides[0]), std::string("hp-bars"));
    CHECK_EQ(std::string(out[1].provides[1]), std::string("mana-bars"));

    // Fewer places than mods: fills what fits, still counts them all.
    tome_mod_info one;
    CHECK_EQ(list.fill(&one, sizeof(one), 1), 2);
    CHECK_EQ(std::string(one.id), std::string("alpha"));
}

TEST(mod_list_bad_arguments) {
    Snapshot list({row("alpha", "1.0.0", "active")});
    tome_mod_info out;
    CHECK_EQ(list.fill(&out, sizeof(out), -1), -1);
    CHECK_EQ(list.fill(nullptr, sizeof(out), 1), -1);
    CHECK_EQ(list.fill(&out, 0, 1), -1);
    CHECK_EQ(list.fill(&out, 3, 1), -1);
    Snapshot empty({});
    CHECK_EQ(empty.fill(&out, sizeof(out), 1), 0);
}

// A mod built against an older tome.h (a smaller struct) gets only what it
// knows of; a newer one (a larger struct) gets the rest zeroed.
TEST(mod_list_other_struct_sizes) {
    Snapshot list({row("alpha", "1.0.0", "active"), row("beta", "2.0.0", "disabled")});
    const uint32_t small = offsetof(tome_mod_info, version);
    std::vector<unsigned char> buffer(small * 2, 0xCC);
    CHECK_EQ(list.fill(reinterpret_cast<tome_mod_info*>(buffer.data()), small, 2), 2);
    tome_mod_info second;
    std::memcpy(&second, buffer.data() + small, small);
    CHECK_EQ(second.size, small);
    CHECK_EQ(std::string(second.id), std::string("beta"));

    struct Larger {
        tome_mod_info info;
        uint32_t later_field;
    };
    Larger larger[2];
    std::memset(larger, 0xCC, sizeof(larger));
    CHECK_EQ(list.fill(&larger[0].info, sizeof(Larger), 2), 2);
    CHECK_EQ(larger[1].info.size, static_cast<uint32_t>(sizeof(tome_mod_info)));
    CHECK_EQ(std::string(larger[1].info.id), std::string("beta"));
    CHECK_EQ(larger[1].info.state, TOME_MOD_DISABLED);
    CHECK_EQ(larger[0].later_field, 0u);
    CHECK_EQ(larger[1].later_field, 0u);
}

TEST(mod_list_compares_what_mods_see) {
    std::vector<ModStatus> status = {row("alpha", "1.0.0", "active")};
    Snapshot a(status);
    status[0].description = "changed, but not listed";
    status[0].warnings = {"a warning"};
    CHECK(a.same_as(status));
    status[0].version = "1.0.1";
    CHECK(!a.same_as(status));
    status[0].version = "1.0.0";
    status[0].provides = {"x"};
    CHECK(!a.same_as(status));
    status[0].provides.clear();
    status[0].state = "disabled";
    CHECK(!a.same_as(status));
    status.push_back(row("beta", "1.0.0", "active"));
    CHECK(!Snapshot({row("alpha", "1.0.0", "active")}).same_as(status));
}
