// SPDX-License-Identifier: GPL-3.0-or-later
#include "lobby_messages.h"

#include "core/handshake.h"
#include "core/log_guard.h"
#include "core/lobbymsg.h"
#include "frames.h"
#include "guard.h"
#include "handshake.h"
#include "log.h"
#include "mod_handle.h"
#include "unwind.h"

#include <windows.h>

#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace tome::lobby_messages {
namespace {

// A mod may send this many messages in any second (wall clock): each is up
// to eleven lines to each player, and the game's lobby chat carries them all.
constexpr size_t kMaxPerSecond = 16;
constexpr uint32_t kMaxReason = 256;
static_assert(lobbymsg::kMaxMessage == TOME_LOBBY_MAX_MESSAGE, "the wire format's limit is the API's");

struct Listener {
    uint64_t token;
    tome_mod* mod;
    tome_lobby_message_fn fn;
    void* user;
};

struct StartHook {
    uint64_t token;
    tome_mod* mod;
    tome_lobby_start_fn fn;
    void* user;
};

// Main thread only; the lock keeps the tables consistent for revokers that
// run elsewhere at shutdown. Callbacks run with it released.
SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<Listener> g_listeners;
std::vector<StartHook> g_start_hooks;
uint64_t g_next_token = 1;

lobbymsg::Reassembler g_reassembler;
std::map<std::string, uint32_t> g_next_id;               // per mod id
std::map<std::string, std::deque<ULONGLONG>> g_sent_at;  // per mod id: send times in the last second
std::set<std::string> g_warned;                          // "<mod>\n<subject>" warned once
handshake::LogBudget g_bad_log(10);  // other players' malformed lines
bool g_in_start = false;             // inside the on_lobby_start callbacks

const char* id_of(const tome_mod* mod) {
    return mod ? mod->id.c_str() : "?";
}

bool warn_once(const tome_mod* mod, const char* subject) {
    return g_warned.insert(std::string(id_of(mod)) + "\n" + subject).second;
}

bool take_rate(const tome_mod* mod) {
    std::deque<ULONGLONG>& times = g_sent_at[mod->id];
    const ULONGLONG now = GetTickCount64();
    while (!times.empty() && now - times.front() >= 1000) times.pop_front();
    if (times.size() >= kMaxPerSecond) return false;
    times.push_back(now);
    return true;
}

// Only required mods talk in the lobby: every player has them (the
// handshake makes sure), so a message never reaches a computer without its
// mod, and the start waits for what they say.
bool required(tome_mod* mod, const char* what) {
    if (mod->sync_required) return true;
    if (warn_once(mod, what)) {
        TOME_WARN("[%s] %s: only for a \"sync\": \"required\" mod (said once)", mod->id.c_str(), what);
    }
    return false;
}

tome_result TOME_CALL api_lobby_send(tome_mod* mod, int32_t to, const void* data, uint32_t size) {
    if (!frames::on_main_thread()) {
        TOME_ERROR("[%s] lobby_send called off the game's main thread; ignored", id_of(mod));
        return TOME_ERR_WRONG_THREAD;
    }
    if (!mod || (!data && size) || size > lobbymsg::kMaxMessage) return TOME_ERR_INVALID_ARGUMENT;
    if (!required(mod, "lobby_send")) return TOME_ERR_UNAVAILABLE;
    std::vector<handshake::LobbyMember> members;
    bool hosting = false;
    if (!handshake::lobby_members(members, hosting)) {
        if (warn_once(mod, "lobby_send outside a lobby")) {
            TOME_WARN("[%s] lobby_send: only in a game's lobby (said once)", mod->id.c_str());
        }
        return TOME_ERR_UNAVAILABLE;
    }
    // Who gets it: people whose Tome hides the lines (anyone else would see
    // them in the chat). A joined player talks to the host only.
    std::vector<int> recipients;
    for (const handshake::LobbyMember& m : members) {
        if (m.local || !m.tome) continue;
        if (!hosting && !m.host) continue;
        if (to == TOME_LOBBY_EVERYONE ? hosting : m.id == to) recipients.push_back(m.id);
    }
    if (recipients.empty() && !(to == TOME_LOBBY_EVERYONE && hosting)) {
        if (warn_once(mod, "lobby_send recipient")) {
            TOME_WARN("[%s] lobby_send: lobby player %d doesn't run Tome as far as this computer knows%s "
                      "(said once)", mod->id.c_str(), static_cast<int>(to), hosting ? "" : ", or isn't the host");
        }
        return TOME_ERR_UNAVAILABLE;
    }
    if (!take_rate(mod)) {
        if (warn_once(mod, "lobby_send rate")) {
            TOME_WARN("[%s] lobby_send: more than %zu messages a second; the rest aren't sent (said once)",
                      mod->id.c_str(), kMaxPerSecond);
        }
        return TOME_ERR_UNAVAILABLE;
    }
    const uint32_t id = g_next_id[mod->id]++;
    const std::vector<std::string> lines =
        lobbymsg::encode(mod->id, id, std::string_view(static_cast<const char*>(data), size));
    if (lines.empty()) return TOME_ERR_INVALID_ARGUMENT;  // not a valid mod id: can't happen for a loaded mod
    for (int r : recipients) {
        for (const std::string& line : lines) {
            if (!handshake::lobby_send_line(r, line)) return TOME_ERR_UNAVAILABLE;
        }
    }
    return TOME_OK;
}

tome_result TOME_CALL api_on_lobby_message(tome_mod* mod, tome_lobby_message_fn fn, void* user) {
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

int32_t TOME_CALL api_lobby_players(tome_lobby_player* out, int32_t max) {
    if (!frames::on_main_thread() || max < 0 || (max > 0 && !out)) return -1;
    std::vector<handshake::LobbyMember> members;
    bool hosting = false;
    if (!handshake::lobby_members(members, hosting)) return -1;
    for (size_t i = 0; i < members.size() && static_cast<int32_t>(i) < max; ++i) {
        const handshake::LobbyMember& m = members[i];
        tome_lobby_player& p = out[i];
        std::memset(&p, 0, sizeof(p));
        p.id = m.id;
        p.is_host = m.host ? 1 : 0;
        p.is_local = m.local ? 1 : 0;
        p.runs_tome = m.tome ? 1 : 0;
        // Cut on a UTF-8 character boundary.
        size_t n = std::min(m.name.size(), sizeof(p.name) - 1);
        while (n > 0 && n < m.name.size() && (static_cast<unsigned char>(m.name[n]) & 0xC0) == 0x80) --n;
        std::memcpy(p.name, m.name.data(), n);
    }
    return static_cast<int32_t>(members.size());
}

tome_result TOME_CALL api_on_lobby_start(tome_mod* mod, tome_lobby_start_fn fn, void* user) {
    if (!mod || !fn) return TOME_ERR_INVALID_ARGUMENT;
    if (!required(mod, "on_lobby_start")) return TOME_ERR_UNAVAILABLE;
    uint64_t token;
    {
        ExclusiveLock hold(g_lock);
        token = g_next_token++;
        g_start_hooks.push_back({token, mod, fn, user});
    }
    mod->add_revoker([token] {
        ExclusiveLock hold(g_lock);
        std::erase_if(g_start_hooks, [token](const StartHook& h) { return h.token == token; });
    });
    return TOME_OK;
}

tome_result TOME_CALL api_lobby_start(tome_mod* mod) {
    if (!frames::on_main_thread()) {
        TOME_ERROR("[%s] lobby_start called off the game's main thread; ignored", id_of(mod));
        return TOME_ERR_WRONG_THREAD;
    }
    if (!mod) return TOME_ERR_INVALID_ARGUMENT;
    if (!required(mod, "lobby_start")) return TOME_ERR_UNAVAILABLE;
    if (g_in_start) {
        TOME_WARN("[%s] lobby_start from inside an on_lobby_start callback; ignored", mod->id.c_str());
        return TOME_ERR_UNAVAILABLE;
    }
    TOME_INFO("[%s] lobby_start: pressing Start", mod->id.c_str());
    return handshake::lobby_press_start() ? TOME_OK : TOME_ERR_UNAVAILABLE;
}

void deliver(int sender, bool from_host, const lobbymsg::Message& message) {
    std::vector<Listener> due;
    {
        SharedLock hold(g_lock);
        for (const Listener& l : g_listeners) {
            if (l.mod->id == message.mod_id && l.mod->sync_required) due.push_back(l);
        }
    }
    tome_lobby_message m = {};
    m.size = sizeof(m);
    m.player = sender;
    m.from_host = from_host ? 1 : 0;
    m.data = message.data.data();
    m.data_size = static_cast<uint32_t>(message.data.size());
    for (const Listener& l : due) {
        bool registered = false;
        {
            SharedLock hold(g_lock);
            for (const Listener& r : g_listeners) registered |= r.token == l.token;
        }
        if (!registered) continue;
        guard::run(l.mod, "lobby message callback", message.mod_id.c_str(), [&] { l.fn(l.mod, &m, l.user); });
    }
}

} // namespace

void fill_api(tome_api& api) {
    api.lobby_send = api_lobby_send;
    api.on_lobby_message = api_on_lobby_message;
    api.lobby_players = api_lobby_players;
    api.on_lobby_start = api_on_lobby_start;
    api.lobby_start = api_lobby_start;
}

bool receive(int from, bool from_host, const std::string& text) {
    lobbymsg::Message message;
    switch (g_reassembler.add(from, text, message)) {
    case lobbymsg::Reassembler::Result::NotOurs: return false;
    case lobbymsg::Reassembler::Result::Partial: return true;
    case lobbymsg::Reassembler::Result::Complete: deliver(from, from_host, message); return true;
    case lobbymsg::Reassembler::Result::Bad:
        switch (g_bad_log.take(from)) {
        case handshake::LogBudget::Verdict::Log:
            TOME_WARN("Lobby messages: player %d sent a malformed line (\"%s\"); dropped", from,
                      log::printable(text, 100).c_str());
            break;
        case handshake::LogBudget::Verdict::Exhausted:
            TOME_WARN("Lobby messages: player %d sent too many malformed lines; not logging the rest", from);
            break;
        case handshake::LogBudget::Verdict::Quiet: break;
        }
        return true;
    }
    return false;
}

bool start_allowed(std::vector<std::string>& reasons) {
    std::vector<StartHook> hooks;
    {
        SharedLock hold(g_lock);
        hooks = g_start_hooks;
    }
    bool allowed = true;
    g_in_start = true;
    for (const StartHook& h : hooks) {
        if (!h.mod->sync_required) continue;
        char reason[kMaxReason] = {};
        int32_t verdict = TOME_START_ALLOW;
        guard::run(h.mod, "lobby start callback", h.mod->id.c_str(),
                   [&] { verdict = h.fn(h.mod, reason, kMaxReason, h.user); });
        if (verdict == TOME_START_ALLOW) continue;
        allowed = false;
        reason[kMaxReason - 1] = '\0';
        TOME_INFO("[%s] refused the start%s%s", h.mod->id.c_str(), reason[0] ? ": " : "", reason);
        if (reason[0]) reasons.push_back(reason);
    }
    g_in_start = false;
    return allowed;
}

void on_lobby_closed() {
    g_reassembler.reset();
    g_bad_log.reset();
}

} // namespace tome::lobby_messages
