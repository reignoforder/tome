// SPDX-License-Identifier: GPL-3.0-or-later
#include "update_check.h"

#include "core/tome_settings.h"
#include "core/update_check.h"
#include "crash_loop.h"
#include "frames.h"
#include "log.h"
#include "main_thread.h"
#include "mod_menu.h"
#include "popups.h"
#include "restart.h"
#include "sha256.h"
#include "tome_settings.h"
#include "unwind.h"

#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>

#include <atomic>
#include <vector>

namespace tome::update_check {

// loader/release-keys.txt as it was at build time (generated/release_keys.cpp).
extern const char* const kReleaseKeysText;

namespace {

// The menu shows UI opened from code once it has finished appearing (~3 s).
constexpr unsigned kMenuDelayMs = 4000;
constexpr DWORD kTimeoutMs = 15000;

SRWLOCK g_lock = SRWLOCK_INIT;  // the fields below
std::wstring g_file;            // tome/update.json
State g_state;
Activity g_activity = Activity::Idle;
std::string g_error;            // the last failed check's reason
std::string g_url;              // what to ask; "" if checks can't run in this build
std::vector<signature::Key> g_keys;  // allowed to sign releases; set by init, then only read
SemVer g_current;
bool g_started = false;         // on_main_menu started the check (main thread)
std::string g_showing;          // the version whose popup is up or queued; "" if none (main thread)
std::atomic<bool> g_checking{false};
std::atomic<bool> g_installing{false};
bool g_automatic = false;       // the install running or last finished was the setting's, not a click (main thread)
std::wstring g_mix;             // tome.mix as loaded (its own path); "" if unknown
bool g_keep_old = false;        // tome.mix.old is the version before this one: removed once this one settled
// The file this code was loaded from has been renamed to tome.mix.old by an
// install (restart.cpp's helper must run from it, not the new tome.mix).
std::atomic<bool> g_moved{false};

std::wstring old_mix() { return g_mix + L".old"; }
std::wstring new_mix() { return g_mix + L".new"; }
std::wstring failed_mix() { return g_mix + L".failed"; }

bool file_exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring self_path() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&self_path), &self)) {
        return L"";
    }
    return module_path(self);
}

int64_t unix_now() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return static_cast<int64_t>((t - 116444736000000000ULL) / 10000000ULL);
}

bool enabled() {
    return tome_settings::update_check_on();
}

void save_locked() {
    if (!write_file_atomic(g_file, format_state(g_state))) TOME_WARN("Couldn't write tome/update.json");
}

std::string win_error(const char* what) {
    DWORD code = GetLastError();
    const char* why = "";
    switch (code) {
    case ERROR_WINHTTP_TIMEOUT: why = ": timed out"; break;
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: why = ": no network, or the name isn't known"; break;
    case ERROR_WINHTTP_CANNOT_CONNECT: why = ": couldn't connect"; break;
    case ERROR_WINHTTP_SECURE_FAILURE: why = ": the secure connection failed"; break;
    default: break;
    }
    char buf[160];
    snprintf(buf, sizeof(buf), "%s failed (error %lu%s)", what, code, why);
    return buf;
}

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
};

// GETs `url`, at most `max_size` bytes. HTTP 404 (no release published yet,
// or no such file) gives true with an empty body; an empty reply to a 200 is
// an error.
bool fetch(const std::string& url, std::string& body, std::string& error, size_t max_size = kMaxReply) {
    std::wstring wide = widen(url);
    wchar_t host[256], path[2048], extra[2048];
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host;
    parts.dwHostNameLength = ARRAYSIZE(host);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = ARRAYSIZE(path);
    // Without a buffer for it, the query ("?...") would be dropped from the request.
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = ARRAYSIZE(extra);
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts)) {
        error = "the URL isn't valid";
        return false;
    }
    std::wstring object = path, query = extra;
    if (!query.empty() && query[0] == L'?') object += query.substr(0, query.find(L'#'));  // no fragment
    const bool https = parts.nScheme == INTERNET_SCHEME_HTTPS;
    if (!https && !TOME_DEV) {
        error = "only https:// is allowed";
        return false;
    }

    Handle session{WinHttpOpen(L"Tome/" TOME_VERSION_W, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.h) {
        error = win_error("WinHttpOpen");
        return false;
    }
    // GitHub needs TLS 1.2, which Windows 7 only offers when asked (and
    // updated: KB3140245). Newer Windows takes TLS 1.3 too.
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x00002000 /* TLS1_3 */;
    if (!WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
    WinHttpSetTimeouts(session.h, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);

    Handle connection{WinHttpConnect(session.h, host, parts.nPort, 0)};
    if (!connection.h) {
        error = win_error("WinHttpConnect");
        return false;
    }
    Handle request{WinHttpOpenRequest(connection.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0)};
    if (!request.h) {
        error = win_error("WinHttpOpenRequest");
        return false;
    }
    // GitHub's API headers only for its API; release files come from
    // github.com (a redirect to its file servers), which wants none.
    const bool api = url.rfind("https://api.github.com/", 0) == 0;
    const wchar_t* headers = api ? L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n"
                                 : WINHTTP_NO_ADDITIONAL_HEADERS;
    if (!WinHttpSendRequest(request.h, headers, api ? static_cast<DWORD>(-1) : 0, WINHTTP_NO_REQUEST_DATA, 0, 0,
                            0)) {
        error = win_error("Sending the request");
        return false;
    }
    if (!WinHttpReceiveResponse(request.h, nullptr)) {
        error = win_error("Receiving the reply");
        return false;
    }
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
        error = win_error("Reading the reply's status");
        return false;
    }
    if (status == 404) {
        body.clear();
        return true;
    }
    if (status != 200) {
        error = "the server answered HTTP " + std::to_string(status) +
                (status == 403 || status == 429 ? " (too many requests; try later)" : "");
        return false;
    }
    body.clear();
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available)) {
            error = win_error("Reading the reply");
            return false;
        }
        if (available == 0) break;
        if (available > max_size - body.size()) {  // not body.size() + available: that can wrap
            error = "the reply is too long";
            return false;
        }
        size_t at = body.size();
        body.resize(at + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, body.data() + at, available, &read)) {
            error = win_error("Reading the reply");
            return false;
        }
        body.resize(at + read);
    }
    // Only a 404 means "nothing there": an empty 200 (a proxy, a broken
    // server) would otherwise pass for "no release yet" and skip a day.
    if (body.empty()) {
        error = "the reply is empty";
        return false;
    }
    return true;
}

void open_page(const std::string& tag) {
    std::wstring url = widen(release_page(tag));
    auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr,
                                                          SW_SHOWNORMAL));
    if (result <= 32) TOME_WARN("Update check: couldn't open the release page (%ld)", static_cast<long>(result));
}

void start_install(bool automatic);

// Only with Check for Updates on (the setting's description says so).
bool auto_install() {
    return enabled() && tome_settings::update_install_on();
}

// Call with g_lock held: an install is running, or done this session (the
// next start runs it). A check mustn't replace that state: notify() relies on
// it not to offer (or install) another version on top.
bool install_active_locked() {
    return g_activity == Activity::Downloading || g_activity == Activity::Installed;
}

// Main thread: the popup for a newer version, once per version. Not again
// while it's up: start() shows one found before, and the check it starts
// finishes before the player has answered. With Install Updates
// Automatically on, installs it instead (also once per version: a failed
// automatic install shows its own popup and isn't retried by itself). The
// version counts as shown once the install has finished (finish_install),
// so one the game closed in the middle of is offered again.
void notify() {
    const bool automatic = auto_install();
    std::string latest, tag, zip_sha256;
    {
        ExclusiveLock hold(g_lock);
        if (!should_notify(g_state, g_current) || g_state.latest == g_showing) return;
        if (install_active_locked()) return;
        latest = g_state.latest;
        tag = g_state.tag;
        zip_sha256 = g_state.zip_sha256;
    }
    if (automatic) {
        TOME_INFO("Update: installing %s automatically (Install Updates Automatically is on)", latest.c_str());
        start_install(true);
        return;
    }
    g_showing = latest;
    popups::show(update_prompt(TOME_VERSION, latest, zip_sha256), [latest, tag](int choice) {
        if (g_showing == latest) g_showing.clear();
        // Taken away (TOME_POPUP_CLOSED): shown again at the next start.
        if (choice != kUpdateNow && choice != kOpenPage && choice != kNotNow) return;
        if (choice == kUpdateNow) {
            start_install(false);  // shown once it has finished (finish_install)
            return;
        }
        {
            ExclusiveLock hold(g_lock);
            // Not if a newer one was found since: its own popup says.
            if (g_state.latest == latest) {
                g_state.notified = latest;
                save_locked();
            }
        }
        if (choice == kOpenPage) open_page(tag);
    });
}

// Main thread: what the check thread found.
void finish(bool ok, const Release& release, bool none, const std::string& zip_sha256, const std::string& error,
            int64_t now) {
    bool dropped = false;
    {
        ExclusiveLock hold(g_lock);
        if (install_active_locked()) {
            // Not saved (not even last_check): the next start, running what
            // was installed, checks again.
            dropped = true;
        } else if (ok) {
            g_activity = Activity::Idle;
            g_error.clear();
            g_state.last_check = now;
            g_state.latest = none ? "" : release.version.to_string();
            g_state.tag = none ? "" : release.tag;
            g_state.zip_sha256 = none ? "" : zip_sha256;
            save_locked();
        } else {
            g_activity = Activity::Failed;
            g_error = error;
        }
    }
    if (dropped) {
        g_checking = false;
        TOME_INFO("Update check: an update was installed meanwhile; the next start checks again");
        return;
    }
    if (!ok) {
        TOME_WARN("Update check: couldn't check (%s)", error.c_str());
    } else if (none) {
        TOME_INFO("Update check: no release published yet");
    } else {
        TOME_INFO("Update check: the latest release is %s (%s); this is %s", release.version.to_string().c_str(),
                  zip_sha256.empty() ? "not newer" : "newer, signed with a release key", TOME_VERSION);
        if (!zip_sha256.empty()) {
            TOME_INFO("Update check: tome-%s.zip's SHA-256 is %s", release.version.to_string().c_str(),
                      zip_sha256.c_str());
        }
    }
    g_checking = false;
    mod_menu::on_mods_changed();  // the Tome page's line
    if (ok && enabled()) notify();  // not if the player turned the check off while it ran
}

DWORD WINAPI check_thread(void*) {
    std::string url;
    {
        SharedLock hold(g_lock);
        url = g_url;
    }
    std::string body, error, zip_sha256;
    Release release;
    bool ok = fetch(url, body, error);
    bool none = ok && body.empty();
    if (ok && !none) ok = parse_release(body, release, error);
    // A newer release is only announced if it's signed with a release key.
    if (ok && !none && compare(release.version, g_current) > 0) {
        std::string sums, sums_signature;
        ok = fetch(asset_url(url, release.tag, kSumsFile), sums, error) &&
             fetch(asset_url(url, release.tag, kSumsSignatureFile), sums_signature, error);
        if (ok && (sums.empty() || sums_signature.empty())) {
            ok = false;
            error = std::string("release ") + release.tag + " isn't signed (no " + kSumsSignatureFile + ")";
        }
        if (ok) ok = verify_release(release, sums, sums_signature, g_keys, zip_sha256, error);
    }
    int64_t now = unix_now();
    main_thread::post([=] { finish(ok, release, none, zip_sha256, error, now); });
    return 0;
}

// Main thread. Starts a check if it's allowed, on and due.
void check_if_due() {
    if (!enabled()) return;
    {
        SharedLock hold(g_lock);
        if (g_url.empty() || !due(g_state, unix_now())) return;
    }
    if (g_checking.exchange(true)) return;
    {
        ExclusiveLock hold(g_lock);
        if (!install_active_locked()) g_activity = Activity::Checking;
    }
    TOME_INFO("Update check: asking %s", g_url.c_str());
    HANDLE thread = CreateThread(nullptr, 0, check_thread, nullptr, 0, nullptr);
    if (!thread) {
        TOME_WARN("Update check: couldn't start its thread (error %lu)", GetLastError());
        ExclusiveLock hold(g_lock);
        if (g_activity == Activity::Checking) g_activity = Activity::Idle;
        g_checking = false;
        return;
    }
    CloseHandle(thread);
    mod_menu::on_mods_changed();
}

// --- installing -------------------------------------------------------------

// For the installing thread only: Windows' file virtualization off, so a
// write into a folder this process may not change (Program Files without
// admin rights) fails instead of landing in the VirtualStore, where the
// game wouldn't load it. Through a copy of the process token on this thread,
// so the game's own writes keep it. Best effort: Wine has no virtualization.
struct NoVirtualization {
    HANDLE token = nullptr;
    NoVirtualization() {
        HANDLE process = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &process)) return;
        if (DuplicateTokenEx(process, TOKEN_IMPERSONATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT, nullptr,
                             SecurityImpersonation, TokenImpersonation, &token)) {
            DWORD off = 0;
            SetTokenInformation(token, TokenVirtualizationEnabled, &off, sizeof(off));
            if (!SetThreadToken(nullptr, token)) {
                CloseHandle(token);
                token = nullptr;
            }
        }
        CloseHandle(process);
    }
    ~NoVirtualization() {
        if (token) {
            RevertToSelf();
            CloseHandle(token);
        }
    }
    NoVirtualization(const NoVirtualization&) = delete;
    NoVirtualization& operator=(const NoVirtualization&) = delete;
};

std::string last_error(const char* what) {
    DWORD code = GetLastError();
    char buf[160];
    snprintf(buf, sizeof(buf), "%s failed (error %lu%s)", what, code,
             code == ERROR_ACCESS_DENIED ? ": no permission to change the game folder" : "");
    return buf;
}

// Creates the folders between `base` and the file `path`.
void make_parents(const std::wstring& base, const std::wstring& path) {
    for (size_t at = path.find(L'\\', base.size() + 1); at != std::wstring::npos; at = path.find(L'\\', at + 1)) {
        CreateDirectoryW(path.substr(0, at).c_str(), nullptr);
    }
}

// Puts the new tome.mix in place of the loaded one, which Windows lets us
// rename but not overwrite: tome.mix -> tome.mix.old, tome.mix.new ->
// tome.mix. The game only loads *.mix, so neither of the others runs.
bool swap_mix(const std::string& mix, std::string& error) {
    if (!write_file_atomic(new_mix(), mix)) {
        error = last_error("Writing tome.mix.new");
        return false;
    }
    if (sha256_file(new_mix()) != sha256_bytes(mix)) {
        DeleteFileW(new_mix().c_str());
        error = "tome.mix.new didn't read back as written";
        return false;
    }
    if (!DeleteFileW(old_mix().c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        error = last_error("Removing an earlier tome.mix.old");
        DeleteFileW(new_mix().c_str());
        return false;
    }
    if (!MoveFileExW(g_mix.c_str(), old_mix().c_str(), 0)) {
        error = last_error("Renaming tome.mix");
        DeleteFileW(new_mix().c_str());
        return false;
    }
    g_moved = true;
    if (!MoveFileExW(new_mix().c_str(), g_mix.c_str(), 0)) {
        error = last_error("Putting the new tome.mix in place");
        if (MoveFileExW(old_mix().c_str(), g_mix.c_str(), 0)) {
            g_moved = false;
        } else {
            TOME_ERROR("Update: couldn't put tome.mix back from tome.mix.old (error %lu): rename it by hand",
                       GetLastError());
        }
        DeleteFileW(new_mix().c_str());
        return false;
    }
    // Saved now, not when the install finishes: a game that closes before
    // that still runs the new version, which may need the rollback.
    ExclusiveLock hold(g_lock);
    g_state.installed_mix = sha256_bytes(mix);
    save_locked();
    return true;
}

// The installing thread: download, check, unpack, swap. Nothing in the game
// folder changes unless the zip is the signed one and unpacks completely.
bool install(const std::string& url, const std::string& tag, const std::string& version,
             const std::string& expected_sha256, std::string& error) {
    std::string archive;
    if (!fetch(asset_url(url, tag, zip_name(version).c_str()), archive, error, kMaxZip)) return false;
    if (archive.empty()) {
        error = "the release has no " + zip_name(version);
        return false;
    }
    if (expected_sha256.empty() || sha256_bytes(archive) != expected_sha256) {
        error = "the download doesn't match the signed release";
        return false;
    }
    std::vector<zip::Entry> entries;
    std::vector<const zip::Entry*> plan;
    if (!zip::read_directory(archive, entries, error) || !plan_install(entries, plan, error)) return false;
    std::vector<std::pair<std::string, std::string>> files;  // name, contents
    for (const zip::Entry* e : plan) {
        std::string data;
        if (!zip::extract(archive, *e, data, error)) return false;
        files.emplace_back(e->name, std::move(data));
    }

    NoVirtualization no_virtualization;
    for (const auto& [name, data] : files) {
        if (name == "tome.mix" && !swap_mix(data, error)) return false;
    }
    // Tome's texts: a failure here leaves the old ones, nothing worse.
    const std::wstring dir = g_mix.substr(0, g_mix.find_last_of(L'\\'));
    for (const auto& [name, data] : files) {
        if (name == "tome.mix") continue;
        std::wstring path = dir + L"\\" + widen(name);
        for (wchar_t& c : path) c = c == L'/' ? L'\\' : c;
        make_parents(dir, path);
        if (!write_file_atomic(path, data)) TOME_WARN("Update: couldn't write %s", name.c_str());
    }
    return true;
}

void finish_install(bool ok, const std::string& version, const std::string& tag, const std::string& error) {
    {
        ExclusiveLock hold(g_lock);
        g_activity = ok ? Activity::Installed : Activity::InstallFailed;
        g_error = ok ? "" : error;
        // Done with, either way: not offered (or installed automatically) again.
        if (!version.empty()) {
            g_state.notified = version;
            save_locked();
        }
    }
    g_installing = false;
    mod_menu::on_mods_changed();
    if (!ok) {
        TOME_WARN("Update: couldn't install %s (%s); nothing was changed", version.c_str(), error.c_str());
        popups::show(install_failed_prompt(version, error), [tag](int choice) {
            if (choice == 0) open_page(tag);
        });
        return;
    }
    TOME_INFO("Update: installed Tome %s; it runs from the next start (tome.mix.old is %s until then)",
              version.c_str(), TOME_VERSION);
    bool in_game = false;
    frames::popup_parent(in_game);
    popups::show(installed_prompt(version, in_game, g_automatic), [](int choice) {
        if (choice == kRestartNow) restart::request("the update");
    });
}

DWORD WINAPI install_thread(void*) {
    std::string url, tag, version, expected;
    {
        SharedLock hold(g_lock);
        url = g_url;
        tag = g_state.tag;
        version = g_state.latest;
        expected = g_state.zip_sha256;
    }
    std::string error;
    bool ok = install(url, tag, version, expected, error);
    main_thread::post([=] { finish_install(ok, version, tag, error); });
    return 0;
}

// Main thread: Update Now.
void start_install(bool automatic) {
    std::string version, tag;
    {
        SharedLock hold(g_lock);
        // Installed this session (a second popup's Update Now): installing
        // again would make tome.mix.old the version that never ran.
        if (g_activity == Activity::Installed) return;
        version = g_state.latest;
        tag = g_state.tag;
    }
    if (g_mix.empty()) {
        finish_install(false, version, tag, "Tome couldn't find its own tome.mix");
        return;
    }
    if (g_installing.exchange(true)) return;
    g_automatic = automatic;  // only for the install that starts here
    {
        ExclusiveLock hold(g_lock);
        g_activity = Activity::Downloading;
    }
    TOME_INFO("Update: installing Tome %s", version.c_str());
    HANDLE thread = CreateThread(nullptr, 0, install_thread, nullptr, 0, nullptr);
    if (!thread) {
        g_installing = false;
        finish_install(false, version, tag, "couldn't start a thread");
        return;
    }
    CloseHandle(thread);
    mod_menu::on_mods_changed();
}

// At startup (init, g_lock held): an update that didn't reach the main menu
// twice goes back to the version before it.
void check_rollback() {
    DeleteFileW(new_mix().c_str());     // an install that didn't finish
    DeleteFileW(failed_mix().c_str());  // a version rolled back before: not loaded now
    // Only a tome.mix.old an update left for this very tome.mix: one left
    // by hand, or kept for a tome.mix replaced since, is someone else's.
    const bool from_update = !g_state.installed_mix.empty() && file_exists(old_mix()) &&
                             old_from_update(g_state, sha256_file(g_mix));
    if (!from_update && !g_state.installed_mix.empty()) {
        g_state.installed_mix.clear();
        save_locked();
    }
    const int failed = crash_loop::failed_starts();
    if (!should_roll_back(from_update, failed)) {
        g_keep_old = from_update;
        return;
    }
    // This version is the one that didn't start: it moves aside for the old.
    if (MoveFileExW(g_mix.c_str(), failed_mix().c_str(), 0)) {
        if (MoveFileExW(old_mix().c_str(), g_mix.c_str(), 0)) {
            g_state.rolled_back = TOME_VERSION;
            g_state.installed_mix.clear();
            save_locked();
            TOME_ERROR("Update: Tome %s didn't reach the main menu %d times in a row; the version before it "
                       "runs from the next start, and %s won't be offered again",
                       TOME_VERSION, failed, TOME_VERSION);
            return;
        }
        MoveFileExW(failed_mix().c_str(), g_mix.c_str(), 0);
    }
    TOME_ERROR("Update: couldn't put back the version before %s (error %lu)", TOME_VERSION, GetLastError());
}

void ask() {
    popups::show(ask_prompt(), [](int choice) {
        // Closed by a mod or Tome (TOME_POPUP_CLOSED) isn't an answer: ask next time.
        if (choice != kAllow && choice != kDeny) return;
        std::string error;
        if (!tome_settings::set(tome_settings::kUpdateCheck, json::Value::boolean(choice == kAllow), error)) {
            TOME_WARN("Update check: %s", error.c_str());
            return;
        }
        on_setting_changed(tome_settings::kUpdateCheck);
        mod_menu::on_mods_changed();  // a Tome settings page that's open shows the answer
    });
}

void start() {
    if (!tome_settings::stored(tome_settings::kUpdateCheck)) {
        ask();
        return;
    }
    // Turned off: nothing found before is shown (or installed) either.
    if (!enabled()) return;
    notify();  // found before, and the popup was taken away or the game closed first
    check_if_due();
}

} // namespace

void init(const Paths& paths) {
    ExclusiveLock hold(g_lock);
    g_file = paths.tome_dir + L"\\update.json";
    parse_semver(TOME_VERSION, g_current);
    std::string text;
    if (read_file(g_file, text)) g_state = parse_state(text);
    g_mix = self_path();
    if (!g_mix.empty()) check_rollback();
    std::string error;
    if (!parse_keys(kReleaseKeysText, g_keys, error)) TOME_ERROR("Update check: release-keys.txt: %s", error.c_str());
    if (TOME_DEV) {
        char url[1024];
        DWORD len = GetEnvironmentVariableA("TOME_DEBUG_UPDATE_URL", url, sizeof(url));
        if (len > 0 && len < sizeof(url)) g_url = url;
        // A test's own release key, for a test server's signed releases.
        char key[128];
        len = GetEnvironmentVariableA("TOME_DEBUG_RELEASE_KEY", key, sizeof(key));
        signature::Key extra;
        if (len > 0 && len < sizeof(key) && signature::decode_key(key, extra)) g_keys.push_back(extra);
    } else {
        g_url = kLatestUrl;
    }
    if (!g_url.empty() && g_keys.empty()) {
        TOME_WARN("Update check: this build has no release key, so it can't announce releases");
    }
}

void on_main_menu() {
    // This version reached the main menu: once it has settled (crash_loop),
    // the one before it isn't needed for a rollback any more. Whether or not
    // update checks are on.
    if (g_keep_old && main_thread::installed()) {
        g_keep_old = false;
        main_thread::post_delayed(crash_loop::kGoodAfterMs + 1000, [] {
            // An update installed since has made tome.mix.old this very
            // version: the new one's rollback needs it.
            ExclusiveLock hold(g_lock);
            if (install_active_locked()) return;
            if (DeleteFileW(old_mix().c_str())) TOME_INFO("Update: this version started fine; removed tome.mix.old");
            g_state.installed_mix.clear();
            save_locked();
        });
    }
    if (g_started) return;
    {
        SharedLock hold(g_lock);
        if (g_url.empty()) return;  // a developer build without TOME_DEBUG_UPDATE_URL
    }
    // Popups need the main thread; a menu set up before it was found tries
    // again at the next one.
    if (!main_thread::installed()) return;
    g_started = true;
    main_thread::post_delayed(kMenuDelayMs, start);
}

void on_setting_changed(const std::string& key) {
    if (key != tome_settings::kUpdateCheck) return;
    if (enabled()) {
        check_if_due();
    } else {
        ExclusiveLock hold(g_lock);
        if (g_activity == Activity::Failed) g_activity = Activity::Idle;
    }
}

std::wstring loaded_file() {
    if (g_mix.empty()) return L"";
    return g_moved ? old_mix() : g_mix;
}

std::string status() {
    bool on = enabled();
    SharedLock hold(g_lock);
    if (g_url.empty()) return "";
    return status_line(on, g_activity, g_error, g_state, g_current);
}

} // namespace tome::update_check
