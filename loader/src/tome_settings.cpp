// SPDX-License-Identifier: GPL-3.0-or-later
#include "tome_settings.h"

#include "core/command_line.h"
#include "core/tome_settings.h"
#include "log.h"
#include "patches.h"
#include "unwind.h"

#include <atomic>
#include <vector>

namespace tome::tome_settings {
namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
std::wstring g_file;  // tome/settings.json
json::Value g_stored = json::Value::object();  // the file as last read or written, unknown keys kept
json::Value g_values = json::Value::object();  // every setting, defaults filled in
std::atomic<bool> g_widescreen{true};
std::atomic<bool> g_update_check{false};
std::atomic<bool> g_update_install{false};
bool g_harden_heap_at_start = false;  // set once, by init
std::vector<std::string> g_no_patch;  // the -tomenopatch values, by load
std::string g_error;  // why the file load read was invalid, or ""

// Call with g_lock held exclusively. Logs what's wrong in the file only
// when `warn` (at startup): a later set() would repeat it.
void refresh(bool warn) {
    std::vector<std::string> warnings;
    g_values = settings::load_values(schema_with_patches(), g_stored, warnings);
    if (warn) {
        for (const std::string& w : warnings) TOME_WARN("tome/settings.json: %s", w.c_str());
    }
    g_widescreen = get_bool(g_values, kWidescreen);
    g_update_check = get_bool(g_values, kUpdateCheck);
    g_update_install = get_bool(g_values, kUpdateInstall);
}

// Reads the file (none without `paths`) and the option, with g_lock held
// exclusively; the caller refreshes g_values. `error`: why the file was
// invalid, or "".
void read_locked(const Paths* paths, std::string& error) {
    g_file = paths ? paths->tome_dir + L"\\settings.json" : std::wstring();
    g_stored = json::Value::object();
    error.clear();
    std::string text;
    if (paths && read_file(g_file, text)) {
        json::Value root;
        json::ParseResult parsed = json::parse(text, root);
        if (parsed.ok && root.is_object()) {
            g_stored = std::move(root);
        } else {
            error = parsed.ok ? "not an object" : parsed.error;
        }
    }
    g_no_patch = command_line::list_values(GetCommandLineW(), kNoPatchOption);
}

} // namespace

void load(const Paths* paths) {
    ExclusiveLock hold(g_lock);
    read_locked(paths, g_error);
    refresh(false);
}

void init() {
    ExclusiveLock hold(g_lock);
    // What load read (the patches were installed with it), not the file
    // again: it may have changed since.
    if (!g_error.empty()) TOME_WARN("tome/settings.json is invalid (%s); using the defaults", g_error.c_str());
    refresh(true);
    g_harden_heap_at_start = get_bool(g_values, kHardenHeap);
    TOME_INFO("Tome settings: widescreen %s, update check %s, automatic install %s, stop on memory corruption %s",
              g_widescreen ? "on" : "off", !g_stored.find(kUpdateCheck) ? "not asked yet" : g_update_check ? "on" : "off",
              g_update_install ? "on" : "off", g_harden_heap_at_start ? "on" : "off");
    // Which patches are off is in the "Patches:" line (bootstrap.cpp), from
    // what install did; only names that match no patch are left to say.
    for (const std::string& name : g_no_patch) {
        bool known = name == "all";
        for (const patches::Patch* p : patches::all()) known = known || name == p->name;
        if (!known) TOME_WARN("-tomenopatch: no built-in patch \"%s\"", name.c_str());
    }
    // Without "=<names>" (list_values skips it) nothing is turned off: say so.
    if (command_line::has_flag(GetCommandLineW(), kNoPatchOption)) {
        TOME_WARN("-tomenopatch needs the patches' names (-tomenopatch=<name>[,<name>...] or =all): none turned off");
    }
}

const settings::Schema& schema_with_patches() {
    static const settings::Schema s = [] {
        std::vector<PatchInfo> list;
        for (const patches::Patch* p : patches::all()) list.push_back({p->name, p->summary, {}});
        return with_patches(schema(), list);
    }();
    return s;
}

settings::Schema menu_schema() {
    std::vector<PatchInfo> list;
    for (const patches::Patch* p : patches::all()) {
        patches::Outcome o = patches::outcome(p->name);
        std::string now;
        switch (o.state) {
        case patches::State::Applied: now = "In force."; break;
        case patches::State::Off:
        case patches::State::Skipped: now = "Not in force now: " + o.reason + "."; break;
        case patches::State::Failed: now = "Not in force now: it failed (" + o.reason + ")."; break;
        case patches::State::NotInstalled: now = "Not in force now."; break;
        }
        list.push_back({p->name, p->summary, std::move(now)});
    }
    return with_patches(schema(), list);
}

std::string patch_off(std::string_view name) {
    SharedLock hold(g_lock);
    return patch_off_reason(name, g_no_patch, g_values);
}

json::Value values() {
    SharedLock hold(g_lock);
    return g_values;
}

bool set(const std::string& key, const json::Value& value, std::string& error) {
    const settings::Setting* s = schema_with_patches().find(key);
    if (!s) {
        error = "no Tome setting \"" + key + "\"";
        return false;
    }
    json::Value coerced;
    if (!settings::coerce(*s, value, coerced, error)) return false;
    {
        ExclusiveLock hold(g_lock);
        g_stored.set(key, coerced);
        refresh(false);
        // Through a temp file, like keybinds.json.
        if (!write_file_atomic(g_file, json::serialize(g_stored))) TOME_WARN("Couldn't write tome/settings.json");
    }
    TOME_INFO("Tome setting %s: %s%s", key.c_str(), settings::display(*s, coerced).c_str(),
              key.rfind(kPatchPrefix, 0) == 0 ? " (from the next start)" : "");
    return true;
}

bool widescreen() {
    return g_widescreen.load(std::memory_order_relaxed);
}

bool update_check_on() {
    return g_update_check.load(std::memory_order_relaxed);
}

bool update_install_on() {
    return g_update_install.load(std::memory_order_relaxed);
}

bool harden_heap_at_start() {
    SharedLock hold(g_lock);
    return g_harden_heap_at_start;
}

bool stored(const std::string& key) {
    SharedLock hold(g_lock);
    return g_stored.find(key) != nullptr;
}

} // namespace tome::tome_settings
