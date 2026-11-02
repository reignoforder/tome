// SPDX-License-Identifier: GPL-3.0-or-later
#include "intermod.h"

#include "core/intermod.h"
#include "frames.h"
#include "guard.h"
#include "log.h"
#include "mod_handle.h"
#include "unwind.h"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace tome::intermod_api {
namespace {

struct ServiceListener {
    uint64_t token;
    tome_mod* mod;
    std::string name;
    tome_service_fn fn;
    void* user;
};

struct Subscription {
    uint64_t token;
    tome_mod* mod;
    std::string topic;
    tome_message_fn fn;
    void* user;
};

// Everything is touched on the main thread only; the lock keeps the tables
// consistent for the revokers, which may run elsewhere during shutdown.
// Callbacks always run with it released.
SRWLOCK g_lock = SRWLOCK_INIT;
intermod::Services g_services;
std::vector<ServiceListener> g_listeners;
std::vector<Subscription> g_subscriptions;
std::map<std::string, std::shared_ptr<int>> g_tags;  // owner/name -> tag of the revoker
std::vector<std::string> g_publishing;  // topics being delivered, to refuse loops
uint64_t g_next_token = 1;

const char* id_of(tome_mod* mod) {
    return mod ? mod->id.c_str() : "?";
}

bool check_thread(tome_mod* mod, const char* what, tome_result& result) {
    if (frames::on_main_thread()) return true;
    TOME_ERROR("[%s] %s called off the game's main thread; ignored", id_of(mod), what);
    result = TOME_ERR_WRONG_THREAD;
    return false;
}

// `except` (may be NULL) is the mod whose own listeners are skipped: the
// provider isn't told about its own service going away.
void notify(const std::string& name, bool available, const tome_mod* except = nullptr) {
    std::vector<ServiceListener> due;
    {
        SharedLock hold(g_lock);
        for (const ServiceListener& l : g_listeners) {
            if (l.name == name && l.mod != except) due.push_back(l);
        }
    }
    for (const ServiceListener& l : due) {
        // A listener revoked by an earlier callback is skipped.
        bool registered = false;
        {
            SharedLock hold(g_lock);
            for (const ServiceListener& r : g_listeners) registered |= r.token == l.token;
        }
        if (!registered) continue;
        guard::run(l.mod, "service callback", name.c_str(), [&] { l.fn(l.mod, name.c_str(), available, l.user); });
    }
}

tome_result TOME_CALL api_service_register(tome_mod* mod, const char* name, uint32_t version, const void* table) {
    tome_result wrong = TOME_OK;
    if (!check_thread(mod, "service_register", wrong)) return wrong;
    if (!mod || !name) return TOME_ERR_INVALID_ARGUMENT;
    intermod::Result r;
    {
        ExclusiveLock hold(g_lock);
        r = g_services.add(mod->id, name, version, table);
    }
    if (r == intermod::Result::Invalid) {
        TOME_ERROR("[%s] service_register(%s): needs a valid name (a-z, 0-9, . - _; up to 64) and a table",
                   mod->id.c_str(), name);
        return TOME_ERR_INVALID_ARGUMENT;
    }
    if (r == intermod::Result::Taken) {
        TOME_ERROR("[%s] service_register(%s): another mod offers it", mod->id.c_str(), name);
        return TOME_ERR_NOT_OWNER;
    }
    TOME_INFO("[%s] offers service %s (version %u)", mod->id.c_str(), name, version);
    std::string service = name;
    if (r == intermod::Result::Ok) {
        auto tag = std::make_shared<int>(0);
        {
            ExclusiveLock hold(g_lock);
            g_tags[mod->id + "/" + service] = tag;
        }
        mod->add_revoker(
            [mod, service, tag] {
                bool removed;
                {
                    ExclusiveLock hold(g_lock);
                    removed = g_services.remove(mod->id, service);
                    g_tags.erase(mod->id + "/" + service);
                }
                // Revokers can run off the main thread at shutdown; mods' callbacks must not.
                if (removed && frames::on_main_thread()) notify(service, false, mod);
            },
            tag.get());
        notify(service, true, mod);
    }
    return TOME_OK;
}

tome_result TOME_CALL api_service_unregister(tome_mod* mod, const char* name) {
    tome_result wrong = TOME_OK;
    if (!check_thread(mod, "service_unregister", wrong)) return wrong;
    if (!mod || !name) return TOME_ERR_INVALID_ARGUMENT;
    bool removed;
    std::shared_ptr<int> tag;
    {
        ExclusiveLock hold(g_lock);
        removed = g_services.remove(mod->id, name);
        auto it = g_tags.find(mod->id + "/" + name);
        if (it != g_tags.end()) {
            tag = it->second;
            g_tags.erase(it);
        }
    }
    if (!removed) return TOME_ERR_INVALID_ARGUMENT;
    if (tag) mod->drop_revokers({tag.get()});
    notify(name, false, mod);
    return TOME_OK;
}

const void* TOME_CALL api_service_get(tome_mod* mod, const char* name, uint32_t version) {
    if (!mod || !name) return nullptr;
    SharedLock hold(g_lock);
    std::optional<intermod::Service> s = g_services.find(name, version);
    return s ? s->table : nullptr;
}

tome_result TOME_CALL api_on_service(tome_mod* mod, const char* name, tome_service_fn fn, void* user) {
    tome_result wrong = TOME_OK;
    if (!check_thread(mod, "on_service", wrong)) return wrong;
    if (!mod || !fn || !intermod::valid_name(name ? name : "")) return TOME_ERR_INVALID_ARGUMENT;
    uint64_t token;
    bool present;
    {
        ExclusiveLock hold(g_lock);
        token = g_next_token++;
        g_listeners.push_back({token, mod, name, fn, user});
        present = g_services.find(name, 0).has_value();
    }
    mod->add_revoker([token] {
        ExclusiveLock hold(g_lock);
        std::erase_if(g_listeners, [token](const ServiceListener& l) { return l.token == token; });
    });
    if (present) {
        std::string service = name;
        guard::run(mod, "service callback", name, [&] { fn(mod, service.c_str(), 1, user); });
    }
    return TOME_OK;
}

tome_result TOME_CALL api_subscribe(tome_mod* mod, const char* topic, tome_message_fn fn, void* user) {
    tome_result wrong = TOME_OK;
    if (!check_thread(mod, "subscribe", wrong)) return wrong;
    if (!mod || !fn || !intermod::valid_name(topic ? topic : "")) return TOME_ERR_INVALID_ARGUMENT;
    uint64_t token;
    {
        ExclusiveLock hold(g_lock);
        token = g_next_token++;
        g_subscriptions.push_back({token, mod, topic, fn, user});
    }
    mod->add_revoker([token] {
        ExclusiveLock hold(g_lock);
        std::erase_if(g_subscriptions, [token](const Subscription& s) { return s.token == token; });
    });
    return TOME_OK;
}

int32_t TOME_CALL api_publish(tome_mod* mod, const char* topic, const void* data, size_t size) {
    tome_result wrong = TOME_OK;
    if (!check_thread(mod, "publish", wrong)) return 0;
    if (!mod || !intermod::valid_name(topic ? topic : "") || (!data && size > 0)) return 0;

    std::string name = topic;
    std::vector<Subscription> due;
    {
        ExclusiveLock hold(g_lock);
        if (std::find(g_publishing.begin(), g_publishing.end(), name) != g_publishing.end()) {
            TOME_ERROR("[%s] publish(%s): already being delivered; ignored", mod->id.c_str(), topic);
            return 0;
        }
        g_publishing.push_back(name);
        for (const Subscription& s : g_subscriptions) {
            if (s.topic == name && s.mod != mod) due.push_back(s);
        }
    }
    OnUnwind done([&] {
        ExclusiveLock hold(g_lock);
        std::erase(g_publishing, name);
    });

    int32_t reached = 0;
    for (const Subscription& s : due) {
        bool registered = false;
        {
            SharedLock hold(g_lock);
            for (const Subscription& r : g_subscriptions) registered |= r.token == s.token;
        }
        if (!registered) continue;
        if (guard::run(s.mod, "message callback", topic, [&] { s.fn(s.mod, name.c_str(), data, size, s.user); })) {
            ++reached;
        }
    }
    return reached;
}

} // namespace

void fill_api(tome_api& api) {
    api.service_register = api_service_register;
    api.service_unregister = api_service_unregister;
    api.service_get = api_service_get;
    api.on_service = api_on_service;
    api.subscribe = api_subscribe;
    api.publish = api_publish;
}

} // namespace tome::intermod_api
