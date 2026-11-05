// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/mod_menu_model.h"
#include "core/tome_settings.h"
#include "core/update_check.h"
#include "test.h"

#include <cstdio>

using namespace tome;
using namespace tome::update_check;

namespace {

SemVer v(const char* text) {
    SemVer out;
    if (!parse_semver(text, out)) test::fail(__FILE__, __LINE__, std::string("bad semver in test: ") + text);
    return out;
}

bool contains(const std::string& s, const std::string& part) {
    return s.find(part) != std::string::npos;
}

} // namespace

TEST(update_release_parses) {
    // Trimmed from GitHub's reply.
    const char* reply = R"({"url":"https://api.github.com/repos/reignoforder/tome/releases/1",
        "html_url":"https://example.com/elsewhere","tag_name":"v0.2.0","name":"Tome 0.2.0",
        "draft":false,"prerelease":false,"assets":[{"name":"tome-0.2.0.zip"}]})";
    Release r;
    std::string error;
    CHECK(parse_release(reply, r, error));
    CHECK_EQ(r.tag, std::string("v0.2.0"));
    CHECK_EQ(r.version.to_string(), std::string("0.2.0"));
    // The page comes from the tag, not from html_url.
    CHECK_EQ(release_page(r.tag), std::string("https://github.com/reignoforder/tome/releases/tag/v0.2.0"));

    CHECK(parse_release(R"({"tag_name":"1.0.0-rc.1"})", r, error));  // no "v"; prerelease flag missing
    CHECK_EQ(r.version.to_string(), std::string("1.0.0-rc.1"));
}

TEST(update_release_refuses) {
    Release r;
    r.tag = "kept";
    std::string error;
    CHECK(!parse_release("", r, error));
    CHECK(!parse_release("not json", r, error));
    CHECK(contains(error, "isn't JSON"));
    CHECK(!parse_release("[]", r, error));
    CHECK(!parse_release(R"({"name":"x"})", r, error));
    CHECK(contains(error, "tag_name"));
    CHECK(!parse_release(R"({"tag_name":5})", r, error));
    CHECK(!parse_release(R"({"tag_name":"v1.0.0","draft":true})", r, error));
    CHECK(!parse_release(R"({"tag_name":"v1.0.0","prerelease":true})", r, error));
    CHECK(contains(error, "prerelease"));
    CHECK(!parse_release(R"({"tag_name":"latest"})", r, error));
    CHECK(!parse_release(R"({"tag_name":"v1.0"})", r, error));
    CHECK(!parse_release(R"({"tag_name":""})", r, error));
    // Characters that could end up in the URL or the popup.
    CHECK(!parse_release(R"({"tag_name":"v1.0.0/../x"})", r, error));
    CHECK(!parse_release(R"({"tag_name":"v1.0.0?a=b"})", r, error));
    CHECK(!parse_release(R"({"tag_name":"v1.0.0|cffff0000"})", r, error));
    CHECK(!parse_release(R"({"tag_name":"v1.0.0 "})", r, error));
    CHECK(!parse_release(std::string(R"({"tag_name":"v1.0.0-)") + std::string(80, 'a') + "\"}", r, error));
    CHECK(!parse_release(std::string(kMaxReply + 1, ' '), r, error));
    CHECK(contains(error, "too long"));
    CHECK_EQ(r.tag, std::string("kept"));  // untouched on failure

    // A bad tag never reaches the page's URL.
    CHECK_EQ(release_page("../x"), std::string("https://github.com/reignoforder/tome/releases"));
}

TEST(update_state_round_trips) {
    State s;
    s.last_check = 1793750400;
    s.latest = "0.2.0";
    s.tag = "v0.2.0";
    s.notified = "0.1.5";
    s.zip_sha256 = std::string(64, 'c');
    s.rolled_back = "0.1.9";
    s.installed_mix = std::string(64, 'd');
    State back = parse_state(format_state(s));
    CHECK_EQ(back.zip_sha256, s.zip_sha256);
    CHECK_EQ(back.installed_mix, s.installed_mix);
    CHECK_EQ(back.rolled_back, s.rolled_back);
    CHECK_EQ(back.last_check, s.last_check);
    CHECK_EQ(back.latest, s.latest);
    CHECK_EQ(back.tag, s.tag);
    CHECK_EQ(back.notified, s.notified);

    State none = parse_state(format_state(State{}));
    CHECK_EQ(none.last_check, int64_t(0));
    CHECK(none.latest.empty() && none.tag.empty() && none.notified.empty());
    CHECK(none.installed_mix.empty());
    // Not a hash: dropped.
    CHECK(parse_state(R"({"installed_mix":"abc"})").installed_mix.empty());
}

TEST(update_state_tolerates_bad_files) {
    CHECK_EQ(parse_state("").last_check, int64_t(0));
    CHECK_EQ(parse_state("{").last_check, int64_t(0));
    CHECK_EQ(parse_state("[1]").last_check, int64_t(0));
    State s = parse_state(R"({"last_check":-5,"latest":"x","tag":"v1.0.0","notified":"nope"})");
    CHECK_EQ(s.last_check, int64_t(0));
    CHECK(s.latest.empty() && s.tag.empty() && s.notified.empty());
    s = parse_state(R"({"last_check":"1","latest":"1.0.0","tag":"bad/tag"})");
    CHECK_EQ(s.last_check, int64_t(0));
    CHECK(s.latest.empty());  // dropped with its tag
    s = parse_state(R"({"last_check":1e300,"latest":"1.0.0","tag":"v1.0.0","zip_sha256":"ABC"})");
    CHECK_EQ(s.last_check, int64_t(0));
    CHECK_EQ(s.latest, std::string("1.0.0"));
    CHECK(s.zip_sha256.empty());
}

TEST(update_due_once_a_day) {
    State s;
    CHECK(due(s, 1000));  // never checked
    s.last_check = 1000000;
    CHECK(!due(s, 1000000));
    CHECK(!due(s, 1000000 + kInterval - 1));
    CHECK(due(s, 1000000 + kInterval));
    CHECK(due(s, 999999));  // the clock went back
}

TEST(update_newer_and_notify) {
    State s;
    CHECK(!newer(s, v("0.1.0")));
    s.latest = "0.2.0";
    s.tag = "v0.2.0";
    CHECK(newer(s, v("0.1.0")));
    CHECK(newer(s, v("0.2.0-beta.1")));
    CHECK(!newer(s, v("0.2.0")));
    CHECK(!newer(s, v("0.3.0")));  // a developer build ahead of the release
    // Newer, but without the signed hash (found by a version not older than
    // it, then put back by hand): no popup claiming it's signed.
    CHECK(!should_notify(s, v("0.1.0")));
    s.zip_sha256 = std::string(64, 'a');
    CHECK(should_notify(s, v("0.1.0")));
    s.notified = "0.2.0";
    CHECK(!should_notify(s, v("0.1.0")));
    s.latest = "0.2.1";
    CHECK(should_notify(s, v("0.1.0")));  // a newer one than the one shown
    s.rolled_back = "0.2.1";
    CHECK(!should_notify(s, v("0.1.0")));  // installed, didn't start, undone: never again
    s.latest = "0.2.2";
    CHECK(should_notify(s, v("0.1.0")));   // its fix is offered
}

TEST(update_prompts) {
    popup::Spec ask = ask_prompt();
    CHECK(popup::validate(ask).empty());
    CHECK_EQ(ask.options.size(), size_t(2));
    CHECK_EQ(ask.default_option, kAllow);
    CHECK_EQ(ask.cancel_option, kDeny);
    CHECK(contains(ask.text, "GitHub"));
    CHECK(contains(ask.text, "Settings"));

    popup::Spec update = update_prompt("0.1.0", "0.2.0", std::string(64, 'a'));
    CHECK(popup::validate(update).empty());
    CHECK(contains(update.text, "0.2.0 is out, signed with Tome's release key; this is 0.1.0"));
    CHECK(contains(update.text, "SHA-256 of tome-0.2.0.zip:\n" + std::string(64, 'a')));
    CHECK(!contains(update_prompt("0.1.0", "0.2.0", "").text, "SHA-256"));
    CHECK_EQ(update.options.size(), size_t(3));  // one row
    CHECK_EQ(update.options[kUpdateNow], std::string("Update Now"));
    CHECK_EQ(update.options[kOpenPage], std::string("Release Page"));
    CHECK_EQ(update.options[kNotNow], std::string("Not Now"));
    CHECK_EQ(update.default_option, kUpdateNow);
    CHECK_EQ(update.cancel_option, kNotNow);

    popup::Spec installed = installed_prompt("0.2.0", false, false);
    CHECK(popup::validate(installed).empty());
    CHECK(contains(installed.text, "0.2.0 is installed. It runs from the next start"));
    CHECK(!contains(installed.text, "leaves this game"));
    CHECK(!contains(installed.text, "automatically"));
    CHECK(contains(installed_prompt("0.2.0", true, false).text, "leaves this game"));
    popup::Spec automatic = installed_prompt("0.2.0", false, true);
    CHECK(popup::validate(automatic).empty());
    CHECK(contains(automatic.text, "installed automatically, as set in Mods > Tome > Settings"));
    CHECK_EQ(automatic.options[kRestartNow], std::string("Restart Now"));
    CHECK_EQ(installed.options[kRestartNow], std::string("Restart Now"));
    CHECK_EQ(installed.cancel_option, kRestartLater);

    popup::Spec failed = install_failed_prompt("0.2.0", "no permission");
    CHECK(popup::validate(failed).empty());
    CHECK(contains(failed.text, "couldn't be installed: no permission"));
    CHECK(contains(failed.text, "Nothing was changed"));
}

namespace {

zip::Entry entry(const char* name) {
    zip::Entry e;
    e.name = name;
    return e;
}

std::vector<std::string> names(const std::vector<const zip::Entry*>& plan) {
    std::vector<std::string> out;
    for (const zip::Entry* e : plan) out.push_back(e->name);
    return out;
}

} // namespace

TEST(update_install_plan) {
    // The player download's layout (cmake/dist.cmake).
    std::vector<zip::Entry> entries = {
        entry("tome.mix"),           entry("tome/"),
        entry("tome/DISCLAIMER.txt"), entry("tome/README.txt"),
        entry("tome/licenses/"),      entry("tome/licenses/GPL-3.0.txt"),
        entry("tome/licenses/wamr/LICENSE"), entry("tome/mods/"),
    };
    std::vector<const zip::Entry*> plan;
    std::string error;
    CHECK(plan_install(entries, plan, error));
    CHECK(names(plan) == (std::vector<std::string>{"tome.mix", "tome/DISCLAIMER.txt", "tome/README.txt",
                                                   "tome/licenses/GPL-3.0.txt", "tome/licenses/wamr/LICENSE"}));

    // Never the player's things, whatever a zip carries.
    entries.push_back(entry("tome/mods/evil/tome.mod.json"));
    entries.push_back(entry("tome/settings.json"));
    entries.push_back(entry("tome/state.json"));
    entries.push_back(entry("war3.exe"));
    entries.push_back(entry("Game.dll"));
    entries.push_back(entry("other.mix"));
    entries.push_back(entry("tome/licenses"));  // a file named like the folder
    CHECK(plan_install(entries, plan, error));
    CHECK_EQ(plan.size(), size_t(5));

    // No tome.mix: nothing to install.
    std::vector<zip::Entry> no_mix = {entry("tome/README.txt")};
    plan.clear();
    CHECK(!plan_install(no_mix, plan, error));
    CHECK(contains(error, "no tome.mix"));
    CHECK(plan.empty());
    CHECK(!plan_install({}, plan, error));
    CHECK(!plan_install({entry("tome.mix/")}, plan, error));  // a folder isn't the file
}

TEST(update_old_from_update) {
    State s;
    const std::string mix(64, 'a'), other(64, 'b');
    CHECK(!old_from_update(s, mix));  // no update installed one
    CHECK(!old_from_update(s, ""));
    s.installed_mix = mix;
    CHECK(old_from_update(s, mix));
    CHECK(!old_from_update(s, other));  // tome.mix replaced since (by hand, a developer's build)
    CHECK(!old_from_update(s, ""));     // tome.mix couldn't be read
}

TEST(update_rollback_rule) {
    CHECK(!should_roll_back(false, 0));
    CHECK(!should_roll_back(false, 10));  // nothing to go back to
    CHECK(!should_roll_back(true, 0));
    CHECK(!should_roll_back(true, kRollBackAfter - 1));
    CHECK(should_roll_back(true, kRollBackAfter));
    CHECK(should_roll_back(true, kRollBackAfter + 3));
}

TEST(update_status_line) {
    SemVer current = v("0.1.0");
    State s;
    CHECK(contains(status_line(false, Activity::Idle, "", s, current), "not checked"));
    CHECK(contains(status_line(true, Activity::Idle, "", s, current), "not checked yet"));
    CHECK(contains(status_line(true, Activity::Checking, "", s, current), "checking"));
    CHECK(contains(status_line(true, Activity::Failed, "timed out", s, current), "couldn't check: timed out"));
    s.last_check = 1793750400;  // 2026-11-04
    CHECK(contains(status_line(true, Activity::Idle, "", s, current), "up to date"));
    CHECK(contains(status_line(true, Activity::Idle, "", s, current), "2026-11-04"));
    s.latest = "0.2.0";
    s.tag = "v0.2.0";
    std::string line = status_line(false, Activity::Idle, "", s, current);  // shown even when turned off since
    CHECK(contains(line, "0.2.0 is out"));
    CHECK(contains(line, "github.com/reignoforder/tome/releases/tag/v0.2.0"));
    CHECK(contains(status_line(true, Activity::Downloading, "", s, current), "installing 0.2.0"));
    CHECK(contains(status_line(true, Activity::Installed, "", s, current), "0.2.0 installed: restart"));
    CHECK(contains(status_line(true, Activity::InstallFailed, "no permission", s, current),
                   "couldn't install 0.2.0: no permission"));
    s.rolled_back = "0.2.0";
    CHECK(contains(status_line(true, Activity::Idle, "", s, current), "0.2.0 didn't start and was undone"));
}

TEST(update_dates) {
    CHECK_EQ(format_date(0), std::string("1970-01-01"));
    CHECK_EQ(format_date(951782400), std::string("2000-02-29"));
    CHECK_EQ(format_date(1793750400), std::string("2026-11-04"));
    CHECK_EQ(format_date(1793836799), std::string("2026-11-04"));
    CHECK_EQ(format_date(-1), std::string("1969-12-31"));
}

TEST(update_setting_and_tome_page) {
    const settings::Setting* s = tome_settings::schema().find(tome_settings::kUpdateCheck);
    CHECK(s != nullptr);
    if (s) {
        CHECK(s->type == settings::Type::Bool);
        CHECK(!tome_settings::get_bool(json::Value::object(), tome_settings::kUpdateCheck));  // off by default
    }
    const settings::Setting* install = tome_settings::schema().find(tome_settings::kUpdateInstall);
    CHECK(install != nullptr);
    if (install) {
        CHECK(install->type == settings::Type::Bool);
        CHECK(!tome_settings::get_bool(json::Value::object(), tome_settings::kUpdateInstall));  // off by default
        CHECK(contains(install->description, "With Check for Updates on"));
    }

    menu::Data d;
    d.tome.version = "0.1.0";
    menu::State st;
    st.page = menu::Page::Tome;
    CHECK(!contains(menu::build(st, d, 4).body, "Updates:"));  // hidden when ""
    d.tome.updates = "up to date";
    CHECK(contains(menu::build(st, d, 4).body, "Updates: |rup to date"));
}

namespace {

// Made by tools/release_sign.py's code (sdk/support/tome_sign.py) with RFC
// 8032's first test key: the C++ check against another implementation.
const char* const kTestKey = "11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo=";
const char* const kOtherKey = "PUAXw+hDiVqStwqnTRt+vJyYLM8uxJaMwM1V8Sr0Zgw=";  // RFC 8032's second
const char* const kTestSignature =
    "IJMzA9ZjeAREI7UyI1cmUdEWpUqMUGNMAjQqZOhi9fEnv6uLgnrTf6/w+b91fLCFwsNHFhbn+6jPv5TJU3xxBg==";

std::string test_sums() {
    return std::string(64, 'a') + "  tome-0.2.0.zip\n" + std::string(64, 'b') + "  tome-sdk-0.2.0.zip\n";
}

std::string sig_file(const char* key, const char* signature) {
    return std::string(R"({"schema": 1, "key": ")") + key + R"(", "signature": ")" + signature + "\"}";
}

std::vector<signature::Key> keys_of(const char* text) {
    std::vector<signature::Key> keys;
    std::string error;
    if (!parse_keys(text, keys, error)) test::fail(__FILE__, __LINE__, "bad keys in test: " + error);
    return keys;
}

Release release_of(const char* tag) {
    Release r;
    std::string error;
    if (!parse_release(std::string(R"({"tag_name":")") + tag + "\"}", r, error)) {
        test::fail(__FILE__, __LINE__, "bad release in test: " + error);
    }
    return r;
}

} // namespace

TEST(update_release_signature_verifies) {
    std::vector<signature::Key> trusted = keys_of(kTestKey);
    std::string hash, error;
    CHECK(verify_release(release_of("v0.2.0"), test_sums(), sig_file(kTestKey, kTestSignature), trusted, hash,
                         error));
    CHECK_EQ(hash, std::string(64, 'a'));  // the player zip's, not the SDK's

    // Among other keys (a backup first).
    trusted = keys_of((std::string("# backup\n") + kOtherKey + "\n\n" + kTestKey + "\n").c_str());
    CHECK_EQ(trusted.size(), size_t(2));
    CHECK(verify_release(release_of("v0.2.0"), test_sums(), sig_file(kTestKey, kTestSignature), trusted, hash,
                         error));
}

TEST(update_release_signature_refuses) {
    const std::vector<signature::Key> trusted = keys_of(kTestKey);
    const std::string good = sig_file(kTestKey, kTestSignature);
    std::string hash = "kept", error;

    // No keys built in.
    CHECK(!verify_release(release_of("v0.2.0"), test_sums(), good, {}, hash, error));
    CHECK(contains(error, "no release key"));
    // Another key: the signature is right but the key isn't Tome's.
    CHECK(!verify_release(release_of("v0.2.0"), test_sums(), good, keys_of(kOtherKey),
                          hash, error));
    CHECK(contains(error, "isn't Tome's"));
    // The signature moved to another version.
    CHECK(!verify_release(release_of("v0.3.0"), test_sums(), good, trusted, hash, error));
    CHECK(contains(error, "doesn't match"));
    // A changed hash.
    std::string sums = test_sums();
    sums[0] = 'c';
    CHECK(!verify_release(release_of("v0.2.0"), sums, good, trusted, hash, error));
    // A line added.
    CHECK(!verify_release(release_of("v0.2.0"), test_sums() + std::string(64, 'd') + "  evil.exe\n", good, trusted,
                          hash, error));
    // Not a signature file, or a broken one.
    CHECK(!verify_release(release_of("v0.2.0"), test_sums(), "", trusted, hash, error));
    CHECK(!verify_release(release_of("v0.2.0"), test_sums(), "{}", trusted, hash, error));
    CHECK(contains(error, "SHA256SUMS.sig isn't valid"));
    CHECK(!verify_release(release_of("v0.2.0"), std::string(kMaxSums + 1, 'a'), good, trusted, hash, error));
    CHECK(contains(error, "too long"));
    CHECK_EQ(hash, std::string("kept"));

    // Validly signed for 0.2.0, but without tome-0.2.0.zip in it.
    const char* no_zip_signature =
        "mcp9+jgqWyPEdmonrh/sxmKQOkVvUOb7rSLogEvzpqoKrui1uLA6e3jM0A/++CMsKW5L35CzpJPxUIAwhqRECw==";
    CHECK(!verify_release(release_of("v0.2.0"), std::string(64, 'b') + "  tome-sdk-0.2.0.zip\n",
                          sig_file(kTestKey, no_zip_signature), trusted, hash, error));
    CHECK(contains(error, "doesn't list tome-0.2.0.zip"));
    CHECK_EQ(hash, std::string("kept"));
}

TEST(update_release_message_and_urls) {
    CHECK_EQ(release_message("0.2.0", "x\n"), std::string("tome-release-v1\n0.2.0\nx\n"));
    CHECK_EQ(asset_url(kLatestUrl, "v0.2.0", kSumsFile),
             std::string("https://github.com/reignoforder/tome/releases/download/v0.2.0/SHA256SUMS"));
    CHECK_EQ(asset_url("http://127.0.0.1:8765/latest.json", "v0.2.0", kSumsSignatureFile),
             std::string("http://127.0.0.1:8765/v0.2.0/SHA256SUMS.sig"));
}

TEST(update_release_keys_parse) {
    std::vector<signature::Key> keys;
    std::string error;
    CHECK(parse_keys("", keys, error));
    CHECK(keys.empty());
    CHECK(parse_keys("# only a comment\r\n\r\n   \n", keys, error));
    CHECK(keys.empty());
    CHECK(parse_keys(std::string("  ") + kTestKey + "  \r\n", keys, error));
    CHECK_EQ(keys.size(), size_t(1));
    CHECK(!parse_keys("# fine\nnot a key\n", keys, error));
    CHECK(contains(error, "line 2"));
    CHECK_EQ(keys.size(), size_t(1));  // untouched on failure
    CHECK(!parse_keys("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=", keys, error));  // the identity point: weak
    CHECK(contains(error, "weak"));
}

TEST(update_repository_release_keys_parse) {
    // loader/release-keys.txt as committed: what tome.mix gets.
    FILE* f = fopen(TOME_TEST_RELEASE_KEYS, "rb");
    CHECK(f != nullptr);
    if (!f) return;
    std::string text;
    char buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
    fclose(f);
    std::vector<signature::Key> keys;
    std::string error;
    CHECK(parse_keys(text, keys, error));
    CHECK_EQ(error, std::string());
}
