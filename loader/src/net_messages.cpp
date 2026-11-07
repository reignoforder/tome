// SPDX-License-Identifier: GPL-3.0-or-later
#include "net_messages.h"

#include "core/handshake.h"
#include "core/log_guard.h"
#include "core/netmsg.h"
#include "frames.h"
#include "guard.h"
#include "handshake.h"
#include "log.h"
#include "mod_handle.h"
#include "mod_manager.h"
#include "unwind.h"

#include <windows.h>

#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace tome::net_messages {
namespace {

// A mod may send this many messages in any second (wall clock), so a mod in
// a loop can't flood every player's game with actions.
constexpr size_t kMaxPerSecond = 16;
static_assert(netmsg::kMaxMessage == TOME_NET_MAX_MESSAGE, "the wire format's limit is the API's");

struct Listener {
    uint64_t token;
    tome_mod* mod;
    tome_net_message_fn fn;
    void* user;
};

// Main thread only; the lock keeps the table consistent for revokers that
// run elsewhere at shutdown. Callbacks run with it released.
SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<Listener> g_listeners;
uint64_t g_next_token = 1;

netmsg::Reassembler g_reassembler;
std::map<std::string, uint32_t> g_next_id;               // per mod id
std::map<std::string, std::deque<ULONGLONG>> g_sent_at;  // per mod id: send times in the last second
std::set<std::string> g_warned;                          // "<mod>\n<subject>" warned once
// Other players' malformed parts, logged a few at a time.
handshake::LogBudget g_bad_log(10);

const char* id_of(const tome_mod* mod) {
    return mod ? mod->id.c_str() : "?";
}

bool warn_once(const tome_mod* mod, const char* subject) {
    return g_warned.insert(std::string(id_of(mod)) + "\n" + subject).second;
}

// Whether `mod` may send another message now; counts it if so.
bool take_rate(const tome_mod* mod) {
    std::deque<ULONGLONG>& times = g_sent_at[mod->id];
    const ULONGLONG now = GetTickCount64();
    while (!times.empty() && now - times.front() >= 1000) times.pop_front();
    if (times.size() >= kMaxPerSecond) return false;
    times.push_back(now);
    return true;
}

tome_result TOME_CALL api_net_send(tome_mod* mod, const void* data, uint32_t size) {
    if (!frames::on_main_thread()) {
        TOME_ERROR("[%s] net_send called off the game's main thread; ignored", id_of(mod));
        return TOME_ERR_WRONG_THREAD;
    }
    if (!mod || (!data && size) || size > netmsg::kMaxMessage) return TOME_ERR_INVALID_ARGUMENT;
    if (!handshake::in_game() || handshake::in_replay()) {
        if (warn_once(mod, "net_send outside a game")) {
            TOME_WARN("[%s] net_send: only in a game, not in menus or replays (said once)", mod->id.c_str());
        }
        return TOME_ERR_UNAVAILABLE;
    }
    // Before the first step the players may not all be in place, so the
    // game lock below isn't final yet (handshake.cpp, check_lock), and the
    // handshake itself sends nothing before then either.
    if (!handshake::game_stepped()) {
        if (warn_once(mod, "net_send before the first step")) {
            TOME_WARN("[%s] net_send: not before the game's first step (from GAME_TICK on, not at GAME_START; "
                      "said once)", mod->id.c_str());
        }
        return TOME_ERR_UNAVAILABLE;
    }
    // In a multiplayer game every player must have the mod to get its
    // messages: required mods are the ones the handshake makes sure of.
    if (mod_manager().game_locked() && !mod->sync_required) {
        if (warn_once(mod, "net_send locked")) {
            TOME_WARN("[%s] net_send refused: in a multiplayer game only a \"sync\": \"required\" mod sends "
                      "messages (said once)", mod->id.c_str());
        }
        return TOME_ERR_UNAVAILABLE;
    }
    if (!take_rate(mod)) {
        if (warn_once(mod, "net_send rate")) {
            TOME_WARN("[%s] net_send: more than %zu messages a second; the rest aren't sent (said once)",
                      mod->id.c_str(), kMaxPerSecond);
        }
        return TOME_ERR_UNAVAILABLE;
    }
    const uint32_t id = g_next_id[mod->id]++;
    const std::vector<netmsg::Part> parts =
        netmsg::encode(mod->id, id, std::string_view(static_cast<const char*>(data), size));
    if (parts.empty()) return TOME_ERR_INVALID_ARGUMENT;  // not a valid mod id: can't happen for a loaded mod
    for (const netmsg::Part& p : parts) {
        if (!handshake::send_sync_action(netmsg::kCacheFile, p.mission, p.key, p.value)) return TOME_ERR_UNAVAILABLE;
    }
    return TOME_OK;
}

// Any thread: a mod's first enable runs on the bootstrap thread. The
// callbacks themselves run on the main thread.
tome_result TOME_CALL api_on_net_message(tome_mod* mod, tome_net_message_fn fn, void* user) {
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

void deliver(int sender, const netmsg::Message& message, bool replay) {
    std::vector<Listener> due;
    {
        SharedLock hold(g_lock);
        for (const Listener& l : g_listeners) {
            if (l.mod->id == message.mod_id) due.push_back(l);
        }
    }
    // In a multiplayer game only required mods send (api_net_send): a
    // message for another mod there comes from a modified client.
    const bool locked = !replay && mod_manager().game_locked();
    tome_net_message m = {};
    m.size = sizeof(m);
    m.player = sender;
    m.data = message.data.data();
    m.data_size = static_cast<uint32_t>(message.data.size());
    m.replay = replay ? 1 : 0;
    for (const Listener& l : due) {
        // A listener revoked by an earlier callback is skipped.
        bool registered = false;
        {
            SharedLock hold(g_lock);
            for (const Listener& r : g_listeners) registered |= r.token == l.token;
        }
        if (!registered) continue;
        if (locked && !l.mod->sync_required) {
            if (warn_once(l.mod, "net message not required")) {
                TOME_WARN("[%s] a message from player %d in a multiplayer game, where this mod (not \"sync\": "
                          "\"required\") can't send any: dropped (said once)",
                          l.mod->id.c_str(), sender);
            }
            continue;
        }
        guard::run(l.mod, "net message callback", message.mod_id.c_str(), [&] { l.fn(l.mod, &m, l.user); });
    }
}

} // namespace

void fill_api(tome_api& api) {
    api.net_send = api_net_send;
    api.on_net_message = api_on_net_message;
}

void receive(int sender, const std::string& mission, const std::string& key, int32_t value, bool replay) {
    netmsg::Message message;
    switch (g_reassembler.add(sender, mission, key, value, message)) {
    case netmsg::Reassembler::Result::Partial: return;
    case netmsg::Reassembler::Result::Complete: deliver(sender, message, replay); return;
    case netmsg::Reassembler::Result::Bad:
        switch (g_bad_log.take(sender)) {
        case handshake::LogBudget::Verdict::Log:
            TOME_WARN("Messages: player %d sent a malformed part (\"%s\", %d); dropped", sender,
                      log::printable(mission, 100).c_str(), static_cast<int>(value));
            break;
        case handshake::LogBudget::Verdict::Exhausted:
            TOME_WARN("Messages: player %d sent too many malformed parts; not logging the rest", sender);
            break;
        case handshake::LogBudget::Verdict::Quiet: break;
        }
        return;
    }
}

void on_game_start() {
    g_reassembler.reset();
    g_bad_log.reset();
}

} // namespace tome::net_messages
