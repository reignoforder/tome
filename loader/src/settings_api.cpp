// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings_api.h"

#include "core/json.h"
#include "guard.h"
#include "log.h"
#include "main_thread.h"
#include "mod_handle.h"
#include "mod_manager.h"
#include "mod_menu.h"
#include "unwind.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace tome::settings_api {
namespace {

struct Listener {
    uint64_t token;
    tome_mod* mod;
    tome_setting_fn fn;
    void* user;
};

SRWLOCK g_lock = SRWLOCK_INIT;  // on_setting_changed may be called on any thread
std::vector<Listener> g_listeners;
uint64_t g_next_token = 1;

json::Value value_of(tome_mod* mod, const char* key) {
    if (!mod || !key) return {};
    return mod_manager().setting(mod->id, key);
}

size_t copy_out(const std::string& text, char* buffer, size_t size) {
    if (buffer && size > 0) {
        size_t n = text.size() < size - 1 ? text.size() : size - 1;
        std::memcpy(buffer, text.data(), n);
        buffer[n] = '\0';
    }
    return text.size();
}

int32_t TOME_CALL api_setting_bool(tome_mod* mod, const char* key, int32_t fallback) {
    json::Value v = value_of(mod, key);
    return v.is_bool() ? (v.as_bool() ? 1 : 0) : fallback;
}

int32_t TOME_CALL api_setting_int(tome_mod* mod, const char* key, int32_t fallback) {
    json::Value v = value_of(mod, key);
    if (!v.is_number()) return fallback;
    double n = std::round(v.as_number());
    if (n < INT32_MIN) return INT32_MIN;
    if (n > INT32_MAX) return INT32_MAX;
    return static_cast<int32_t>(n);
}

double TOME_CALL api_setting_number(tome_mod* mod, const char* key, double fallback) {
    json::Value v = value_of(mod, key);
    return v.is_number() ? v.as_number() : fallback;
}

size_t TOME_CALL api_setting_string(tome_mod* mod, const char* key, char* buffer, size_t size) {
    json::Value v = value_of(mod, key);
    return copy_out(v.is_string() ? v.as_string() : std::string(), buffer, size);
}

tome_result set(tome_mod* mod, const char* key, const json::Value& value) {
    if (!mod || !key) return TOME_ERR_INVALID_ARGUMENT;
    std::string error;
    tome_result result = mod_manager().set_setting(mod->id, key, value, error);
    if (result != TOME_OK) TOME_ERROR("[%s] set_setting(%s): %s", mod->id.c_str(), key, error.c_str());
    return result;
}

tome_result TOME_CALL api_set_setting_bool(tome_mod* mod, const char* key, int32_t value) {
    return set(mod, key, json::Value::boolean(value != 0));
}

tome_result TOME_CALL api_set_setting_int(tome_mod* mod, const char* key, int32_t value) {
    return set(mod, key, json::Value::number(value));
}

tome_result TOME_CALL api_set_setting_number(tome_mod* mod, const char* key, double value) {
    return set(mod, key, json::Value::number(value));
}

tome_result TOME_CALL api_set_setting_string(tome_mod* mod, const char* key, const char* value) {
    if (!value) return TOME_ERR_INVALID_ARGUMENT;
    return set(mod, key, json::Value::string(value));
}

tome_result TOME_CALL api_on_setting_changed(tome_mod* mod, tome_setting_fn fn, void* user) {
    if (!mod || !fn) return TOME_ERR_INVALID_ARGUMENT;
    uint64_t token;
    {
        ExclusiveLock hold(g_lock);
        token = g_next_token++;
        g_listeners.push_back({token, mod, fn, user});
    }
    mod->add_revoker([token] {
        ExclusiveLock hold(g_lock);
        std::erase_if(g_listeners, [token](const Listener& l) { return l.token == token; });
    });
    return TOME_OK;
}

// Runs the listeners of mod `id` for each key, on the main thread.
void notify(const std::string& id, const std::vector<std::string>& keys) {
    main_thread::post([id, keys] {
        mod_menu::on_mods_changed();
        std::vector<Listener> due;
        {
            SharedLock hold(g_lock);
            for (const Listener& l : g_listeners) {
                if (l.mod->id == id) due.push_back(l);
            }
        }
        OnUnwind release_due([&] { std::vector<Listener>().swap(due); });
        for (const Listener& l : due) {
            for (const std::string& key : keys) {
                // Skip listeners revoked by an earlier callback.
                bool registered = false;
                {
                    SharedLock hold(g_lock);
                    for (const Listener& r : g_listeners) registered |= r.token == l.token;
                }
                if (!registered) break;
                guard::run(l.mod, "setting callback", key.c_str(), [&] { l.fn(l.mod, key.c_str(), l.user); });
            }
        }
    });
}

} // namespace

void fill_api(tome_api& api) {
    api.setting_bool = api_setting_bool;
    api.setting_int = api_setting_int;
    api.setting_number = api_setting_number;
    api.setting_string = api_setting_string;
    api.set_setting_bool = api_set_setting_bool;
    api.set_setting_int = api_set_setting_int;
    api.set_setting_number = api_set_setting_number;
    api.set_setting_string = api_set_setting_string;
    api.on_setting_changed = api_on_setting_changed;
    mod_manager().set_settings_sink(notify);
}

} // namespace tome::settings_api
