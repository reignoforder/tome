// SPDX-License-Identifier: GPL-3.0-or-later
// The update check's reply, a release's SHA256SUMS and SHA256SUMS.sig (from
// the network), release-keys.txt and tome/update.json: parse, and what's
// accepted must be safe to show and to put in a URL.
#include "core/update_check.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace tome;
using namespace tome::update_check;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string_view text(reinterpret_cast<const char*>(data), size);

    Release release;
    std::string error;
    if (parse_release(text, release, error)) {
        std::string page = release_page(release.tag);
        if (page.find("/tag/" + release.tag) == std::string::npos) __builtin_trap();
        for (char c : release.tag) {
            if (c == '/' || c == '|' || c == '?' || c == '#' || c == '%' || c <= ' ') __builtin_trap();
        }
        update_prompt("0.1.0", release.version.to_string(), "");
    }

    // The release's SHA256SUMS and SHA256SUMS.sig (from the network): the
    // input split in two at the first NUL, checked against RFC 8032's first
    // test key (a seed is a valid signature by it). A pass must name the
    // hash of the version's zip as the signed text lists it.
    size_t nul = text.find('\0');
    std::string_view sums = text.substr(0, nul);
    std::string_view sig = nul == std::string_view::npos ? std::string_view() : text.substr(nul + 1);
    static const std::vector<signature::Key> trusted = [] {
        std::vector<signature::Key> keys;
        std::string e;
        parse_keys("11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo=", keys, e);
        return keys;
    }();
    Release fixed;
    parse_semver("0.2.0", fixed.version);
    fixed.tag = "v0.2.0";
    std::string hash;
    if (verify_release(fixed, sums, sig, trusted, hash, error)) {
        if (hash.size() != 64 || (sums.find(hash + "  tome-0.2.0.zip") == std::string_view::npos &&
                                     sums.find(hash + " *tome-0.2.0.zip") == std::string_view::npos)) {
            __builtin_trap();
        }
    }
    std::vector<signature::Key> keys;
    parse_keys(text, keys, error);
    asset_url(text, release.tag, kSumsFile);

    // A zip (the downloaded release, after its hash matched): what an update
    // would install is only tome.mix and Tome's texts.
    std::vector<zip::Entry> entries;
    std::vector<const zip::Entry*> plan;
    if (zip::read_directory(text, entries, error) && plan_install(entries, plan, error)) {
        for (const zip::Entry* e : plan) {
            const std::string& n = e->name;
            if (n != "tome.mix" && n != "tome/README.txt" && n != "tome/DISCLAIMER.txt" &&
                n.rfind("tome/licenses/", 0) != 0) {
                __builtin_trap();
            }
        }
    }

    State state = parse_state(text);
    State again = parse_state(format_state(state));
    if (again.last_check != state.last_check || again.latest != state.latest || again.tag != state.tag ||
        again.notified != state.notified || again.zip_sha256 != state.zip_sha256 ||
        again.rolled_back != state.rolled_back || again.installed_mix != state.installed_mix) {
        __builtin_trap();
    }
    status_line(true, Activity::Idle, error, state, SemVer{});
    format_date(state.last_check);
    return 0;
}
