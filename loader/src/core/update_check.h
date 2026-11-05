// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The update check: at most once a day, when the player allowed it (Tome's
// "update_check" setting, asked once on the first start), Tome asks GitHub
// for its latest release and says when there's a newer one, if that release
// is signed with a release key built into Tome. With one click (Update Now)
// it downloads tome-<version>.zip, installs it if its SHA-256 is the one the
// signed SHA256SUMS lists, and offers a restart; the previous tome.mix stays
// as tome.mix.old until the new one has reached the main menu, and is put
// back if it doesn't, twice (should_roll_back). This part is free of Windows APIs so it can be unit-tested:
// the parsers of what comes from the network (untrusted bytes:
// fuzz/fuzz_update.cpp), the release signature, tome/update.json, when a
// check is due, and the popups' and Tome page's text. update_check.cpp
// (loader/src) asks over WinHTTP.
//
// Release signatures: each release has SHA256SUMS (sha256sum's output for
// its files, tome-<version>.zip among them) and SHA256SUMS.sig, a tome.sig
// style file (core/signature.h: {"schema": 1, "key", "signature"}) whose
// Ed25519 signature is over release_message(). The keys allowed to sign
// releases are in loader/release-keys.txt (parse_keys), built into
// tome.mix; tools/release_sign.py signs with the secret key, kept offline.

#include "popup.h"
#include "signature.h"
#include "version.h"
#include "zip.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tome::update_check {

constexpr const char* kRepository = "reignoforder/tome";
constexpr const char* kLatestUrl = "https://api.github.com/repos/reignoforder/tome/releases/latest";
constexpr int64_t kInterval = 24 * 60 * 60;   // seconds between checks
constexpr size_t kMaxReply = 1024 * 1024;     // a longer reply is refused
constexpr const char* kSumsFile = "SHA256SUMS";
constexpr const char* kSumsSignatureFile = "SHA256SUMS.sig";
constexpr size_t kMaxSums = 64 * 1024;        // a longer SHA256SUMS is refused
constexpr size_t kMaxZip = 64 * 1024 * 1024;  // a longer tome-<version>.zip is refused

// "tome-<version>.zip", the player download a release's SHA256SUMS lists.
std::string zip_name(const std::string& version);

// What an update installs from the player download: tome.mix and Tome's own
// texts (tome/README.txt, tome/DISCLAIMER.txt, tome/licenses/...), never
// anything else under tome/ (mods, settings, state). Paths are relative to
// the folder tome.mix is in, with '/'. False with `error` if the zip has no
// tome.mix or no file at all to install.
bool plan_install(const std::vector<zip::Entry>& entries, std::vector<const zip::Entry*>& out,
                  std::string& error);

struct State;

// Whether tome.mix.old is the version an update replaced: the tome.mix in
// place (`mix_sha256`, its SHA-256) is the one that update installed
// (State::installed_mix). Otherwise a tome.mix.old there is someone else's
// (left by hand, or a developer's build deployed over an update) and Tome
// neither rolls back to it nor deletes it.
bool old_from_update(const State& state, std::string_view mix_sha256);

// At startup, with the previous version kept as tome.mix.old by an update
// (old_from_update): put it back once this version failed to reach the
// main menu twice in a row (the crash-loop count, crash_report::Starts).
// The third start (still this version: it's loaded) puts it back, so the
// fourth runs the old one.
constexpr int kRollBackAfter = 2;
bool should_roll_back(bool old_from_update, int failed_starts);

struct Release {
    SemVer version;
    std::string tag;  // as GitHub has it ("v0.2.0"); only [A-Za-z0-9.+-]
};

// GitHub's "latest release" JSON (tag_name, draft, prerelease). A tag may
// start with "v". False with `error` for anything else: a draft or a
// prerelease, a tag that isn't a version or has other characters.
bool parse_release(std::string_view reply, Release& out, std::string& error);

// The release's page, made from the tag (never a URL from the reply).
std::string release_page(const std::string& tag);

// The URL of one of the release's files. For kLatestUrl, GitHub's download
// URL (https://github.com/<repo>/releases/download/<tag>/<file>); for any
// other URL (TOME_DEBUG_UPDATE_URL), "<tag>/<file>" next to it.
std::string asset_url(std::string_view latest_url, const std::string& tag, const char* file);

// loader/release-keys.txt: one base64 public key per line; blank lines and
// lines starting with '#' are skipped. False with `error` (naming the line)
// for anything else.
bool parse_keys(std::string_view text, std::vector<signature::Key>& out, std::string& error);

// What a release's SHA256SUMS.sig signs:
// "tome-release-v1\n<version>\n" followed by SHA256SUMS as is.
std::string release_message(std::string_view version, std::string_view sums);

// Whether `sums` and `sums_signature` make `release` a signed release: the
// signature file parses, its key is one of `trusted`, the signature over
// release_message(release's version, sums) is valid, and sums lists
// tome-<version>.zip. False with `error` otherwise (also with no keys).
// `zip_sha256` gets that zip's hash.
bool verify_release(const Release& release, std::string_view sums, std::string_view sums_signature,
                    const std::vector<signature::Key>& trusted, std::string& zip_sha256, std::string& error);

// tome/update.json: what the last check found.
struct State {
    int64_t last_check = 0;  // Unix time of the last successful check; 0 if none
    std::string latest;      // the latest release's version then; "" if none
    std::string tag;         // ...and its tag
    std::string notified;    // the version the popup was last shown for
    std::string zip_sha256;  // the latest version's tome-<version>.zip, from its signed SHA256SUMS
    std::string rolled_back; // a version installed and then rolled back: never offered again
    std::string installed_mix;  // SHA-256 of the tome.mix an update put in place (tome.mix.old kept for it); "" if none
};

// A missing, invalid or partly invalid file gives what could be read.
State parse_state(std::string_view text);
std::string format_state(const State& state);

// A check is due a day after the last one (or right away if the clock went back).
bool due(const State& state, int64_t now);

// Whether the version the state knows of is newer than `current`.
bool newer(const State& state, const SemVer& current);

// Whether to show the update popup: a newer version with its zip's signed
// hash, not shown before, and not one that was rolled back.
bool should_notify(const State& state, const SemVer& current);

// The first start's question: kAllow turns the setting on, kDeny off.
constexpr int kAllow = 0, kDeny = 1;
popup::Spec ask_prompt();

// A newer (signed) release: kUpdateNow downloads and installs it, kOpenPage
// opens release_page() in the browser. With the zip's hash from the signed
// SHA256SUMS, if known.
constexpr int kUpdateNow = 0, kOpenPage = 1, kNotNow = 2;
popup::Spec update_prompt(const std::string& current, const std::string& latest, const std::string& zip_sha256);

// After an install (`automatic`: by the update_install setting, not a
// click): kRestartNow restarts the game (restart.h).
constexpr int kRestartNow = 0, kRestartLater = 1;
popup::Spec installed_prompt(const std::string& version, bool in_game, bool automatic);

// An install that failed: kOpenPage offers the download by hand.
popup::Spec install_failed_prompt(const std::string& version, const std::string& error);

// The Tome page's "Updates:" line (with the game's color codes).
enum class Activity { Idle, Checking, Failed, Downloading, Installed, InstallFailed };
std::string status_line(bool enabled, Activity activity, const std::string& error, const State& state,
                        const SemVer& current);

// "2026-11-04" (UTC) for a Unix time.
std::string format_date(int64_t unix_time);

} // namespace tome::update_check
