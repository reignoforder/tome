// SPDX-License-Identifier: GPL-3.0-or-later
#include "tome_settings.h"

#include "core/tome_settings.h"
#include "log.h"
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

// Call with g_lock held exclusively. Logs what's wrong in the file only
// when `warn` (at startup): a later set() would repeat it.
void refresh(bool warn) {
    std::vector<std::string> warnings;
    g_values = settings::load_values(schema(), g_stored, warnings);
    if (warn) {
        for (const std::string& w : warnings) TOME_WARN("tome/settings.json: %s", w.c_str());
    }
    g_widescreen = get_bool(g_values, kWidescreen);
    g_update_check = get_bool(g_values, kUpdateCheck);
    g_update_install = get_bool(g_values, kUpdateInstall);
}

} // namespace

void init(const Paths& paths) {
    ExclusiveLock hold(g_lock);
    g_file = paths.tome_dir + L"\\settings.json";
    std::string text;
    if (read_file(g_file, text)) {
        json::Value root;
        json::ParseResult parsed = json::parse(text, root);
        if (parsed.ok && root.is_object()) {
            g_stored = std::move(root);
        } else {
            TOME_WARN("tome/settings.json is invalid (%s); using the defaults",
                      parsed.ok ? "not an object" : parsed.error.c_str());
        }
    }
    refresh(true);
    g_harden_heap_at_start = get_bool(g_values, kHardenHeap);
    TOME_INFO("Tome settings: widescreen %s, update check %s, automatic install %s, stop on memory corruption %s",
              g_widescreen ? "on" : "off", !g_stored.find(kUpdateCheck) ? "not asked yet" : g_update_check ? "on" : "off",
              g_update_install ? "on" : "off", g_harden_heap_at_start ? "on" : "off");
}

json::Value values() {
    SharedLock hold(g_lock);
    return g_values;
}

bool set(const std::string& key, const json::Value& value, std::string& error) {
    const settings::Setting* s = schema().find(key);
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
    TOME_INFO("Tome setting %s: %s", key.c_str(), settings::display(*s, coerced).c_str());
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
