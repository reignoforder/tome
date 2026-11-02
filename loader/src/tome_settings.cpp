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

// Call with g_lock held exclusively. Logs what's wrong in the file only
// when `warn` (at startup): a later set() would repeat it.
void refresh(bool warn) {
    std::vector<std::string> warnings;
    g_values = settings::load_values(schema(), g_stored, warnings);
    if (warn) {
        for (const std::string& w : warnings) TOME_WARN("tome/settings.json: %s", w.c_str());
    }
    g_widescreen = get_bool(g_values, kWidescreen);
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
    TOME_INFO("Tome settings: widescreen %s", g_widescreen ? "on" : "off");
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

} // namespace tome::tome_settings
