// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/update_check.h"

#include "core/json.h"

#include <cinttypes>
#include <cstdio>

namespace tome::update_check {
namespace {

const char* const kGray = "|cff999999";
const char* const kYellow = "|cffffcc00";

std::string colored(const char* color, const std::string& text) {
    return std::string(color) + text + "|r";
}

bool tag_chars_ok(std::string_view tag) {
    if (tag.empty() || tag.size() > 64) return false;
    for (char c : tag) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                  c == '-' || c == '+';
        if (!ok) return false;
    }
    return true;
}

bool is_sha256(std::string_view text) {
    if (text.size() != 64) return false;
    for (char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// Calls fn(line, number) for each line of `text` ('\n', a '\r' before it dropped).
template <typename Fn>
bool each_line(std::string_view text, Fn fn) {
    int number = 0;
    while (!text.empty()) {
        size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!fn(line, ++number)) return false;
    }
    return true;
}

bool true_member(const json::Value& root, const char* key) {
    const json::Value* v = root.find(key);
    return v && v->is_bool() && v->as_bool();
}

std::string string_member(const json::Value& root, const char* key) {
    const json::Value* v = root.find(key);
    return v && v->is_string() ? v->as_string() : std::string();
}

} // namespace

bool parse_release(std::string_view reply, Release& out, std::string& error) {
    if (reply.size() > kMaxReply) {
        error = "the reply is too long";
        return false;
    }
    json::Value root;
    json::ParseResult parsed = json::parse(reply, root);
    if (!parsed.ok) {
        error = "the reply isn't JSON (" + parsed.error + ")";
        return false;
    }
    if (!root.is_object()) {
        error = "the reply isn't an object";
        return false;
    }
    const json::Value* tag = root.find("tag_name");
    if (!tag || !tag->is_string()) {
        error = "the reply has no tag_name";
        return false;
    }
    if (true_member(root, "draft") || true_member(root, "prerelease")) {
        error = "the latest release is a draft or a prerelease";
        return false;
    }
    const std::string& name = tag->as_string();
    if (!tag_chars_ok(name)) {
        error = "the release's tag isn't a version";
        return false;
    }
    std::string_view version = name;
    if (version.front() == 'v') version.remove_prefix(1);
    Release release;
    if (!parse_semver(version, release.version)) {
        error = "the release's tag \"" + name + "\" isn't a version";
        return false;
    }
    release.tag = name;
    out = std::move(release);
    return true;
}

std::string release_page(const std::string& tag) {
    std::string page = std::string("https://github.com/") + kRepository + "/releases";
    if (tag_chars_ok(tag)) page += "/tag/" + tag;
    return page;
}

std::string asset_url(std::string_view latest_url, const std::string& tag, const char* file) {
    if (latest_url == kLatestUrl) {
        return std::string("https://github.com/") + kRepository + "/releases/download/" + tag + "/" + file;
    }
    size_t slash = latest_url.rfind('/');
    std::string base(latest_url.substr(0, slash == std::string_view::npos ? 0 : slash + 1));
    return base + tag + "/" + file;
}

bool parse_keys(std::string_view text, std::vector<signature::Key>& out, std::string& error) {
    std::vector<signature::Key> keys;
    bool ok = each_line(text, [&](std::string_view line, int number) {
        line = trim(line);
        if (line.empty() || line.front() == '#') return true;
        signature::Key key;
        if (!signature::decode_key(line, key)) {
            error = "line " + std::to_string(number) + " isn't a base64 public key";
            return false;
        }
        if (signature::small_order(key)) {
            error = "line " + std::to_string(number) + " is a weak key";
            return false;
        }
        keys.push_back(key);
        return true;
    });
    if (!ok) return false;
    out = std::move(keys);
    return true;
}

std::string release_message(std::string_view version, std::string_view sums) {
    std::string message = "tome-release-v1\n";
    message += version;
    message += '\n';
    message += sums;
    return message;
}

bool verify_release(const Release& release, std::string_view sums, std::string_view sums_signature,
                    const std::vector<signature::Key>& trusted, std::string& zip_sha256, std::string& error) {
    if (trusted.empty()) {
        error = "this build of Tome has no release key to check releases with";
        return false;
    }
    if (sums.size() > kMaxSums) {
        error = std::string(kSumsFile) + " is too long";
        return false;
    }
    signature::Signature sig;
    std::string why;
    if (!signature::parse(sums_signature, sig, why)) {
        error = std::string(kSumsSignatureFile) + " isn't valid (" + why + ")";
        return false;
    }
    bool known = false;
    for (const signature::Key& key : trusted) known = known || key == sig.key;
    if (!known) {
        error = "the release is signed with a key that isn't Tome's (" + signature::encode_key(sig.key) + ")";
        return false;
    }
    const std::string version = release.version.to_string();
    if (!signature::verify(sig, release_message(version, sums))) {
        error = "the release's signature doesn't match its files";
        return false;
    }
    // sha256sum's lines: "<hash>  <name>" ("<hash> *<name>" in binary mode).
    const std::string zip = zip_name(version);
    std::string found;
    each_line(sums, [&](std::string_view line, int) {
        if (line.size() == 64 + 2 + zip.size() && is_sha256(line.substr(0, 64)) && line[64] == ' ' &&
            (line[65] == ' ' || line[65] == '*') && line.substr(66) == zip) {
            found = std::string(line.substr(0, 64));
            return false;
        }
        return true;
    });
    if (found.empty()) {
        error = std::string("the signed ") + kSumsFile + " doesn't list " + zip;
        return false;
    }
    zip_sha256 = found;
    return true;
}

std::string zip_name(const std::string& version) {
    return "tome-" + version + ".zip";
}

bool plan_install(const std::vector<zip::Entry>& entries, std::vector<const zip::Entry*>& out,
                  std::string& error) {
    std::vector<const zip::Entry*> files;
    bool mix = false;
    for (const zip::Entry& e : entries) {
        if (e.directory()) continue;
        // read_directory refused unsafe names and folded duplicates already.
        const std::string& n = e.name;
        bool ours = n == "tome.mix" || n == "tome/README.txt" || n == "tome/DISCLAIMER.txt" ||
                    (n.rfind("tome/licenses/", 0) == 0 && n.size() > 14);
        if (!ours) continue;
        mix = mix || n == "tome.mix";
        files.push_back(&e);
    }
    if (!mix) {
        error = "the download has no tome.mix";
        return false;
    }
    out = std::move(files);
    return true;
}

bool old_from_update(const State& state, std::string_view mix_sha256) {
    return !state.installed_mix.empty() && mix_sha256 == state.installed_mix;
}

bool should_roll_back(bool old_from_update, int failed_starts) {
    return old_from_update && failed_starts >= kRollBackAfter;
}

State parse_state(std::string_view text) {
    State state;
    json::Value root;
    if (!json::parse(text, root).ok || !root.is_object()) return state;
    if (const json::Value* v = root.find("last_check"); v && v->is_number() && v->as_number() > 0 &&
                                                        v->as_number() < 1e15) {
        state.last_check = static_cast<int64_t>(v->as_number());
    }
    // A version or tag that doesn't parse is dropped with the other.
    SemVer parsed;
    std::string latest = string_member(root, "latest"), tag = string_member(root, "tag");
    if (parse_semver(latest, parsed) && tag_chars_ok(tag)) {
        state.latest = latest;
        state.tag = tag;
    }
    if (std::string notified = string_member(root, "notified"); parse_semver(notified, parsed)) {
        state.notified = notified;
    }
    if (std::string hash = string_member(root, "zip_sha256"); !state.latest.empty() && is_sha256(hash)) {
        state.zip_sha256 = hash;
    }
    if (std::string v = string_member(root, "rolled_back"); parse_semver(v, parsed)) state.rolled_back = v;
    if (std::string hash = string_member(root, "installed_mix"); is_sha256(hash)) state.installed_mix = hash;
    return state;
}

std::string format_state(const State& state) {
    json::Value root = json::Value::object();
    root.set("last_check", json::Value::number(static_cast<double>(state.last_check)));
    root.set("latest", json::Value::string(state.latest));
    root.set("tag", json::Value::string(state.tag));
    root.set("notified", json::Value::string(state.notified));
    root.set("zip_sha256", json::Value::string(state.zip_sha256));
    root.set("rolled_back", json::Value::string(state.rolled_back));
    root.set("installed_mix", json::Value::string(state.installed_mix));
    return json::serialize(root);
}

bool due(const State& state, int64_t now) {
    return state.last_check <= 0 || now < state.last_check || now - state.last_check >= kInterval;
}

bool newer(const State& state, const SemVer& current) {
    SemVer latest;
    return !state.latest.empty() && parse_semver(state.latest, latest) && compare(latest, current) > 0;
}

bool should_notify(const State& state, const SemVer& current) {
    // Without the signed hash (the check found it not newer than the version
    // that ran it, and this one is older: put back by hand) the popup's
    // "signed" would be untrue and Update Now couldn't check the download.
    return newer(state, current) && !state.zip_sha256.empty() && state.notified != state.latest &&
           state.rolled_back != state.latest;
}

popup::Spec ask_prompt() {
    popup::Spec spec;
    spec.title = "Check for Updates?";
    spec.text = "Tome can ask GitHub once a day whether there's a newer version of Tome, and tell you when "
                "there is. It sends nothing about you or your mods (GitHub sees your IP address, as with any "
                "website) and installs nothing unless you click Update Now.\n\nYou can change this later in Mods > Tome > Settings.";
    spec.options = {"Check for Updates", "Don't Check"};
    spec.default_option = kAllow;
    spec.cancel_option = kDeny;
    return spec;
}

popup::Spec update_prompt(const std::string& current, const std::string& latest, const std::string& zip_sha256) {
    popup::Spec spec;
    spec.title = "Tome Update";
    spec.text = "Tome " + latest + " is out, signed with Tome's release key; this is " + current +
                ".\n\nUpdate Now downloads it, checks it against the signed release and installs it; it "
                "takes effect when the game restarts. Your mods and settings stay. The release page "
                "says what changed.";
    if (!zip_sha256.empty()) spec.text += "\n\nSHA-256 of " + zip_name(latest) + ":\n" + zip_sha256;
    spec.options = {"Update Now", "Release Page", "Not Now"};
    spec.default_option = kUpdateNow;
    spec.cancel_option = kNotNow;
    return spec;
}

popup::Spec installed_prompt(const std::string& version, bool in_game, bool automatic) {
    popup::Spec spec;
    spec.title = "Tome Updated";
    spec.text = "Tome " + version + " is installed" +
                std::string(automatic ? " (signed with Tome's release key; installed automatically, as set in "
                                        "Mods > Tome > Settings)"
                                      : "") +
                ". It runs from the next start of the game." +
                std::string(in_game ? "\n\nRestarting now leaves this game." : "");
    spec.options = {"Restart Now", "Later"};
    spec.default_option = kRestartNow;
    spec.cancel_option = kRestartLater;
    return spec;
}

popup::Spec install_failed_prompt(const std::string& version, const std::string& error) {
    popup::Spec spec;
    spec.title = "Update Failed";
    spec.text = "Tome " + version + " couldn't be installed: " + error +
                ".\n\nNothing was changed. You can download it from the release page and unpack it "
                "into the Warcraft III folder, as when installing.";
    spec.options = {"Release Page", "Close"};
    spec.default_option = 0;
    spec.cancel_option = 1;
    return spec;
}

std::string format_date(int64_t unix_time) {
    // Howard Hinnant's civil_from_days.
    int64_t days = unix_time / 86400 - (unix_time % 86400 < 0 ? 1 : 0);
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    int64_t doe = days - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t year = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t day = doy - (153 * mp + 2) / 5 + 1;
    int64_t month = mp < 10 ? mp + 3 : mp - 9;
    if (month <= 2) ++year;
    char buf[32];
    snprintf(buf, sizeof(buf), "%04" PRId64 "-%02" PRId64 "-%02" PRId64, year, month, day);
    return buf;
}

std::string status_line(bool enabled, Activity activity, const std::string& error, const State& state,
                        const SemVer& current) {
    if (activity == Activity::Downloading) return "installing " + state.latest + "...";
    if (activity == Activity::Installed) return colored(kYellow, state.latest + " installed: restart the game to use it");
    if (activity == Activity::InstallFailed) {
        return colored(kYellow, "couldn't install " + state.latest + ": " + error) + " (" +
               release_page(state.tag).substr(8) + ")";
    }
    if (!state.rolled_back.empty() && state.rolled_back == state.latest) {
        return colored(kYellow, state.latest + " didn't start and was undone") + " (" +
               release_page(state.tag).substr(8) + ")";
    }
    if (newer(state, current)) {
        return colored(kYellow, state.latest + " is out") + " (" + release_page(state.tag).substr(8) + ")";
    }
    if (!enabled) return "not checked (Settings: Check for Updates)";
    if (activity == Activity::Checking) return "checking...";
    if (activity == Activity::Failed) return colored(kYellow, "couldn't check: " + error);
    if (state.last_check <= 0) return "not checked yet";
    return "up to date " + colored(kGray, "(checked " + format_date(state.last_check) + ")");
}

} // namespace tome::update_check
