// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/tome_settings.h"
#include "test.h"

#include <string>
#include <vector>

using namespace tome;

TEST(tome_settings_patch_settings_follow_the_others) {
    const settings::Schema& base = tome_settings::schema();
    settings::Schema s = tome_settings::with_patches(
        base, {{"memhack-guard", "closes the JASS typecast", ""}, {"blp-guard", "refuses malformed BLP textures", ""}});
    CHECK_EQ(s.settings.size(), base.settings.size() + 2);
    CHECK_EQ(s.settings[0].key, base.settings[0].key);
    const settings::Setting* memhack = s.find("patch.memhack-guard");
    CHECK(memhack != nullptr);
    if (!memhack) return;
    CHECK(memhack == &s.settings[base.settings.size()]);
    CHECK(memhack->type == settings::Type::Bool);
    CHECK_EQ(memhack->title, std::string("Patch: memhack-guard"));
    CHECK(memhack->description.rfind("Closes the JASS typecast. ", 0) == 0);
    CHECK(memhack->description.find("next start") != std::string::npos);
    // On by default.
    CHECK(tome_settings::get_bool(s, json::Value::object(), "patch.blp-guard"));
    CHECK(!tome_settings::get_bool(json::Value::object(), "patch.blp-guard"));  // not in the base schema
    std::vector<std::string> warnings;
    json::Value values = settings::load_values(s, json::Value::object(), warnings);
    CHECK(values.find("patch.blp-guard") && values.find("patch.blp-guard")->as_bool());
}

// A patch named like a setting of its own gets no second switch; a state
// given for this start comes first in the description.
TEST(tome_settings_patch_settings_skip_own_settings) {
    const settings::Schema& base = tome_settings::schema();
    settings::Schema s = tome_settings::with_patches(
        base, {{"widescreen", "keeps the picture's proportions (Tome > Settings)", ""},
               {"blp-guard", "refuses malformed BLP textures", "Not in force now: turned off by -tomenopatch."}});
    CHECK_EQ(s.settings.size(), base.settings.size() + 1);
    CHECK(!s.find("patch.widescreen"));
    const settings::Setting* blp = s.find("patch.blp-guard");
    CHECK(blp && blp->description.rfind("Not in force now: turned off by -tomenopatch. Refuses malformed", 0) == 0);
}

TEST(tome_settings_patch_off_reason) {
    json::Value values = json::Value::object();
    const std::vector<std::string> none;
    CHECK(tome_settings::patch_off_reason("blp-guard", none, values).empty());
    values.set("patch.blp-guard", json::Value::boolean(false));
    values.set("patch.memhack-guard", json::Value::boolean(true));
    values.set("patch.widescreen", json::Value::string("off"));  // not a bool: on
    CHECK_EQ(tome_settings::patch_off_reason("blp-guard", none, values), std::string("turned off in Tome > Settings"));
    CHECK(tome_settings::patch_off_reason("memhack-guard", none, values).empty());
    CHECK(tome_settings::patch_off_reason("widescreen", none, values).empty());
    // The command line wins over the setting, for this start.
    const std::vector<std::string> flags{"memhack-guard"};
    CHECK_EQ(tome_settings::patch_off_reason("memhack-guard", flags, values),
             std::string("turned off by -tomenopatch"));
    CHECK(tome_settings::patch_off_reason("widescreen", flags, values).empty());
    const std::vector<std::string> all{"all"};
    CHECK(!tome_settings::patch_off_reason("widescreen", all, json::Value::object()).empty());
}
