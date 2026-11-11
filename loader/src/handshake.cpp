// SPDX-License-Identifier: GPL-3.0-or-later
#include "handshake.h"

#include "core/game_lock.h"
#include "core/handshake.h"
#include "core/netmsg.h"
#include "events.h"
#include "frames.h"
#include "hooks.h"
#include "log.h"
#include "mod_manager.h"
#include "desync.h"
#include "core/desync.h"
#include "net_messages.h"
#include "patches.h"
#include "units.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

// GCC warns that __thiscall is meant for class methods, but it does apply it
// to function pointer types (see mod_menu.cpp).
#pragma GCC diagnostic ignored "-Wattributes"

namespace tome::handshake {
namespace {

using GlueGetFn = void*(*)();
using AddTextFn = void(__thiscall*)(void* area, const char* text);
using LobbyInitFn = void(__thiscall*)(void* event, int type);
using SendLobbyFn = void(__fastcall*)(int recipient, void* event, int net_index);
using PlayerNameFn = const char*(__fastcall*)(int player, int net_index);
using NetIndexFn = int(__fastcall*)(int net_index);
using IsReplayFn = int(*)();
using SyncInitFn = void(__thiscall*)(void* action);
using SyncValueDestroyFn = void(__thiscall*)(void* value);
using SendActionFn = void(__fastcall*)(void* action, int flags);

const BuildMappings* g_build = nullptr;
GlueGetFn g_glue_get = nullptr;
AddTextFn g_add_text = nullptr;
LobbyInitFn g_lobby_init = nullptr;
SendLobbyFn g_send_lobby = nullptr;
PlayerNameFn g_player_name = nullptr;
NetIndexFn g_local_player = nullptr;
NetIndexFn g_is_host = nullptr;
IsReplayFn g_is_replay = nullptr;
using PlayerNumberFn = uint8_t(__fastcall*)(int player, int net_index);
PlayerNumberFn g_player_number = nullptr;  // null when the build doesn't map it
uintptr_t g_game_base = 0;
SyncInitFn g_sync_init = nullptr;
SyncValueDestroyFn g_sync_value_destroy = nullptr;
SendActionFn g_send_action = nullptr;

constexpr const char* kNotePrefix = "|cffffcc00Tome:|r ";
constexpr double kPollSeconds = 0.5;
// Net player ids tried in a lobby: every byte, since the lookup keeps ids
// from 0x80 up in a second list (and checks the bounds of both).
constexpr int kMaxPlayerId = 0xFF;
constexpr int kMaxPlayerSlots = 12;   // player slots that can hold people
constexpr double kReportAfter = 2.0;  // game seconds until every set has arrived
constexpr size_t kTextField = 0x100;  // the size of each text field in the events

double now_seconds() {
    return static_cast<double>(GetTickCount64()) / 1000.0;
}

template <typename T>
T field(const void* object, uint32_t offset) {
    T value;
    std::memcpy(&value, static_cast<const char*>(object) + offset, sizeof(value));
    return value;
}

// A text field of an event, which may lack its terminator.
std::string text_field(const void* object, uint32_t offset) {
    const char* text = static_cast<const char*>(object) + offset;
    return std::string(text, strnlen(text, kTextField));
}

void set_text_field(void* object, uint32_t offset, std::string_view text) {
    char* out = static_cast<char*>(object) + offset;
    size_t n = std::min(text.size(), kTextField - 1);
    std::memcpy(out, text.data(), n);
    out[n] = '\0';
}

// The required mods running here, and the patches that change what map
// scripts do but aren't in force, as the handshake compares them.
ModSet local_set() {
    ModSet set;
    for (const ModStatus& s : mod_manager().status()) {
        if (s.running && s.sync == "required") set.push_back({s.id, s.version, s.sync_hash});
    }
    for (const patches::Patch* p : patches::all()) {
        if (p->sync && patches::outcome(p->name).state != patches::State::Applied) {
            set.push_back(patch_not_in_force(p->name));
        }
    }
    sort(set);
    return set;
}

// --- the lobby -----------------------------------------------------------------------

std::unique_ptr<Lobby> g_lobby;
void* g_lobby_room = nullptr;  // the CGameChatroom g_lobby belongs to
double g_last_poll = 0;

// The net id of player number 1, the game's creator (its host), on net
// client `index`: in a lobby 128, in a game its player slot. -1 if the build
// doesn't map NetGetPlayerNumber or there's no such player.
int host_net_id(int index) {
    if (!g_player_number) return -1;
    for (int id = 0; id < kMaxPlayerId; ++id) {
        if (g_player_number(id, index) == 1) return id;
    }
    return -1;
}

int net_index(void* room) {
    return field<int32_t>(room, g_build->net.chatroom_net_index_offset);
}

void show(void* room, const std::string& text) {
    void* area = field<void*>(room, g_build->net.chatroom_chat_area_offset);
    if (!area) return;
    std::string line = kNotePrefix + text;
    g_add_text(area, line.c_str());
}

// TOME_DEBUG_LOBBY_AS=<net id> (developer builds): the first reply a
// client sends goes out with its local id set to <net id> for the call, as a
// modified client would send it, to see what sender the host reads
// (tools/e2e-multiplayer.sh). The local client's id byte, or null.
uint8_t* local_id_byte(int index) {
    const auto& n = g_build->net;
    if (!n.client_registry || !n.client_block_size) return nullptr;
    using RegistryFn = char*(__cdecl*)();
    char* entry = reinterpret_cast<RegistryFn>(g_game_base + n.client_registry)();
    char* holder = entry ? field<char*>(entry, n.registry_holder_offset) : nullptr;
    char* client = holder ? field<char*>(holder, n.holder_client_offset) : nullptr;
    if (!client) return nullptr;
    return reinterpret_cast<uint8_t*>(client + index * n.client_block_size + n.client_local_id_offset);
}

int debug_send_as() {
    static int s_as = [] {
        char env[16] = {};
        if (!TOME_DEV || !GetEnvironmentVariableA("TOME_DEBUG_LOBBY_AS", env, sizeof(env))) return -1;
        return std::atoi(env);
    }();
    return s_as;
}

void send_line(void* room, int to, const std::string& text) {
    const auto& n = g_build->net;
    std::vector<uint32_t> buffer((n.lobby_event_size + 0x40) / 4, 0);
    g_lobby_init(buffer.data(), static_cast<int>(n.lobby_event_chat));
    set_text_field(buffer.data(), n.lobby_event_text_offset, text);
    static bool s_spoofed = false;
    const int as = debug_send_as();
    uint8_t* id = !s_spoofed && as >= 0 && text.rfind("[Tome] My mods", 0) == 0 ? local_id_byte(net_index(room)) : nullptr;
    if (id) {
        s_spoofed = true;
        const uint8_t real = *id;
        *id = static_cast<uint8_t>(as);
        g_send_lobby(to, buffer.data(), net_index(room));
        *id = real;
        TOME_INFO("Handshake: debug: sent a reply as net id %d (really %d)", as, real);
        return;
    }
    g_send_lobby(to, buffer.data(), net_index(room));
}

// Sends what the lobby wants sent and shows its notes.
void flush(void* room) {
    for (const Outgoing& o : g_lobby->take_outgoing()) {
        TOME_INFO("Handshake: to lobby player %d: %s", o.to, o.text.c_str());
        send_line(room, o.to, o.text);
    }
    for (const std::string& note : g_lobby->take_notes()) {
        TOME_INFO("Handshake: %s", note.c_str());
        show(room, note);
    }
}

// The lobby screen, or null.
void* current_room() {
    const auto& g = g_build->game;
    auto glue = static_cast<char*>(g_glue_get());
    if (!glue || field<uint32_t>(glue, g.glue_screen_id_offset) != g_build->net.screen_game_chatroom) return nullptr;
    void* room = field<void*>(glue, g.glue_screen_offset);
    return frames::is_class(room, "CGameChatroom") ? room : nullptr;
}

// Log lines for each lobby player's handshake lines: a handshake takes a
// few, so this only stops a modified client from filling tome.log.
LogBudget g_lobby_log(100);

// The lobby for `room`, a new one if the lobby changed.
Lobby& lobby_for(void* room) {
    if (!g_lobby || g_lobby_room != room) {
        g_lobby = std::make_unique<Lobby>();
        g_lobby_room = room;
        g_lobby_log.reset();
        TOME_INFO("Handshake: lobby opened");
    }
    return *g_lobby;
}

void poll_lobby() {
    void* room = current_room();
    if (!room) {
        if (g_lobby) TOME_INFO("Handshake: lobby closed");
        g_lobby.reset();
        g_lobby_room = nullptr;
        return;
    }
    int index = net_index(room);
    int local = g_local_player(index) & 0xFF;
    if (local == 0xFF) return;
    bool host = g_is_host(index) != 0;
    std::vector<LobbyPlayer> others;
    for (int id = 0; id < kMaxPlayerId; ++id) {
        if (id == local) continue;
        const char* name = g_player_name(id, index);
        if (name && *name) others.push_back({id, name});
    }
    // Who hosts: the player with number 1 (net::host_id), when the build
    // maps NetGetPlayerNumber; -1 if unknown.
    const int host_id = host_net_id(index);
    if (host && host_id >= 0 && host_id != local) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            TOME_WARN("Handshake: hosting, but player number 1 is net id %d, not this client's %d: Tome clients "
                      "will count that player's required mods as the host's (they still answer this host)",
                      host_id, local);
        }
    }
    // What the lobby looks like to Tome, logged when it changes.
    std::string seen = "net index " + std::to_string(index) + ", local id " + std::to_string(local) +
                       (host ? ", hosting" : ", joined") +
                       (host_id < 0 || host_id == local ? std::string() : ", host " + std::to_string(host_id)) +
                       ", players:";
    for (const LobbyPlayer& p : others) seen += " " + std::to_string(p.id) + " " + p.name;
    static std::string s_last_seen;
    if (seen != s_last_seen) {
        s_last_seen = seen;
        TOME_INFO("Handshake: lobby: %s", seen.c_str());
    }
    Lobby& lobby = lobby_for(room);
    lobby.set_host(host ? -1 : host_id);  // a host knows itself
    lobby.update(host, local_set(), others, now_seconds());
    flush(room);
}

void TOME_CALL before_present(tome_mod*, tome_hook_call*, void*) {
    if (!frames::on_main_thread()) return;
    double now = now_seconds();
    if (now - g_last_poll < kPollSeconds) return;
    g_last_poll = now;
    poll_lobby();
}

void TOME_CALL before_chat_received(tome_mod*, tome_hook_call* call, void*) {
    void* room = reinterpret_cast<void*>(static_cast<uintptr_t>(call->args[0]));
    const void* event = reinterpret_cast<const void*>(static_cast<uintptr_t>(call->args[1]));
    const auto& n = g_build->net;
    std::string text = text_field(event, n.lobby_event_text_offset);
    Line line;
    if (!decode(text, line)) return;  // people's lines, "[Tome] ..." typed by hand included
    call->skip_original = 1;
    int from = field<uint8_t>(event, n.lobby_event_sender_offset);
    switch (g_lobby_log.take(from)) {
    case LogBudget::Verdict::Log: TOME_INFO("Handshake: from lobby player %d: %s", from, text.c_str()); break;
    case LogBudget::Verdict::Exhausted:
        TOME_WARN("Handshake: lobby player %d sent too many lines; not logging the rest", from);
        break;
    case LogBudget::Verdict::Quiet: break;
    }
    if (!frames::is_class(room, "CGameChatroom")) return;
    // A line can arrive before the first poll: the lobby must know whether it
    // hosts and its own mods first, or it drops a reply or answers "none".
    if (!g_lobby || g_lobby_room != room) poll_lobby();
    Lobby& lobby = lobby_for(room);
    lobby.receive(from, text);
    flush(room);
}

// A second press of Start overrides a refusal (core/handshake.h). One for
// every lobby: a refusal in another room has other problems, which don't match.
StartOverride g_override;

constexpr const char* kOverrideHint = "Press Start again within 10 seconds to start anyway.";
static_assert(StartOverride::kWindow == 10.0, "kOverrideHint names the window");

void TOME_CALL before_start_game(tome_mod*, tome_hook_call* call, void*) {
    void* room = reinterpret_cast<void*>(static_cast<uintptr_t>(call->args[0]));
    if (!frames::is_class(room, "CGameChatroom") || !g_is_host(net_index(room))) return;
    poll_lobby();  // with the latest players and mods
    if (!g_lobby || g_lobby_room != room) {
        // The lobby couldn't be read (no local player id): nothing was
        // checked, so a host with required mods doesn't start blind.
        if (local_set().empty()) return;
        if (g_override.press({"the lobby couldn't be read"}, now_seconds())) {
            TOME_WARN("Handshake: starting anyway (the host pressed Start again); the lobby couldn't be read");
            show(room, "|cffff4040Starting without the check: the game may desync.|r");
            return;
        }
        call->skip_original = 1;
        TOME_WARN("Handshake: not starting, Tome couldn't read the lobby's players");
        show(room, "|cffff4040The game can't start: Tome couldn't check the players' required mods.|r");
        show(room, kOverrideHint);
        return;
    }
    std::vector<std::string> problems;
    bool only_patches = false;
    if (g_lobby->can_start(problems, &only_patches)) {
        g_override.reset();
        TOME_INFO("Handshake: starting; every player's required mods match");
        return;
    }
    if (g_override.press(problems, now_seconds())) {
        TOME_WARN("Handshake: starting anyway (the host pressed Start again); the players' %s differ",
                  only_patches ? "built-in patches" : "required mods");
        for (const std::string& p : problems) TOME_WARN("Handshake:   %s", p.c_str());
        show(room, only_patches ? "|cffffcc00Starting anyway: a map that uses what those patches close may desync.|r"
                                : "|cffff4040Starting anyway: the game may desync.|r");
        return;
    }
    call->skip_original = 1;
    TOME_WARN("Handshake: not starting, the players' %s differ", only_patches ? "built-in patches" : "required mods");
    show(room, only_patches
                   ? "|cffffcc00The game can't start yet: players differ in Tome's built-in patches (only maps that "
                     "use the exploits they close can desync).|r"
                   : "|cffff4040The game can't start: every player needs the same required mods.|r");
    for (const std::string& p : problems) {
        TOME_WARN("Handshake:   %s", p.c_str());
        show(room, p);
    }
    show(room, kOverrideHint);
}

// --- games ---------------------------------------------------------------------------

GameRecord g_record;
// One log line for each mod in a set, and a few to spare.
LogBudget g_game_log(kMaxMods + 8);
bool g_in_game = false;
bool g_replay = false;
bool g_sent = false;
bool g_reported = false;

void send_action(std::string_view file, std::string_view mission, std::string_view key, int32_t value) {
    const auto& n = g_build->net;
    std::vector<uint32_t> buffer((n.sync_size + 0x40) / 4, 0);
    void* action = buffer.data();
    g_sync_init(action);
    set_text_field(action, n.sync_file_offset, file);
    set_text_field(action, n.sync_mission_offset, mission);
    set_text_field(action, n.sync_key_offset, key);
    std::memcpy(static_cast<char*>(action) + n.sync_value_offset, &value, sizeof(value));
    g_send_action(action, 0);
    g_sync_value_destroy(static_cast<char*>(action) + n.sync_value_object_offset);
}

void send_set(const ModSet& set) {
    for (const SyncMod& mod : set) send_action(kCacheFile, kMission, encode_key(mod), static_cast<int32_t>(set.size()));
    TOME_INFO("Handshake: sent this game %zu required mod(s): %s", set.size(), describe(set).c_str());
}

// The host's player slot in the current game (host_slot()), found on first
// use or at its first step (in single player, the local player's); -1
// without the mapping, or not found.
int g_host_slot = -1;
bool g_host_found = false;  // find_host_slot ran in this game

void find_host_slot() {
    g_host_found = true;
    g_host_slot = -1;
    // A live game is net client 0; a replay plays on 1 (net client +0x610).
    const int index = g_replay ? 1 : 0;
    const int id = host_net_id(index);
    if (id < 0 || id >= kMaxPlayerSlots) {
        int humans = 0;
        for (int slot = 0; slot < kMaxPlayerSlots; ++slot) humans += units::is_human(slot);
        if (g_player_number && humans > 1) {
            TOME_INFO("Handshake: no host found in this game (player number 1 is %d)", id);
        }
        return;
    }
    g_host_slot = id;
    const char* net_name = g_player_name(id, index);
    TOME_INFO("Handshake: the host is player %d (%s; net name %s)", id, units::player_name(id).c_str(),
              net_name ? net_name : "none");
}

void report() {
    ModSet local = local_set();
    std::vector<GamePlayer> players;
    for (int slot = 0; slot < kMaxPlayerSlots; ++slot) {
        if (units::is_human(slot)) players.push_back({slot, units::player_name(slot)});
    }
    for (const auto& [slot, set] : g_record.sets()) {
        TOME_INFO("Handshake: player %d's required mods: %s", slot, describe(set).c_str());
    }
    std::vector<std::string> lines = game_report(local, units::local_slot(), players, g_record, g_replay);
    if (lines.empty()) {
        TOME_INFO("Handshake: every player%s has the same required mods (%s)", g_replay ? " in the replay" : "",
                  describe(local).c_str());
        return;
    }
    std::string intro = g_replay ? "This replay was recorded with other required mods than yours; it may not play "
                                   "back correctly."
                                 : "The players' required mods differ; the game may desync.";
    TOME_WARN("Handshake: %s", intro.c_str());
    events::display_text(kNotePrefix + intro, 30.0f);
    for (const std::string& line : lines) {
        TOME_WARN("Handshake:   %s", line.c_str());
        events::display_text(kNotePrefix + line, 30.0f);
    }
}

void TOME_CALL before_sync_integer(tome_mod*, tome_hook_call* call, void*) {
    const void* event = reinterpret_cast<const void*>(static_cast<uintptr_t>(call->args[0]));
    const auto& n = g_build->net;
    const std::string file = text_field(event, n.sync_file_offset);
    if (file == netmsg::kCacheFile) {
        // A mod's message (net_messages.cpp). The game would store each
        // part in a game cache; nothing reads it, so it doesn't run.
        call->skip_original = 1;
        call->result = 1;
        net_messages::receive(field<uint8_t>(event, n.sync_sender_offset), text_field(event, n.sync_mission_offset),
                              text_field(event, n.sync_key_offset), field<int32_t>(event, n.sync_value_offset),
                              g_replay);
        return;
    }
    if (file == desync::kCacheFile) {
        // A mod's state checksum (desync.cpp); nothing reads the cache either.
        call->skip_original = 1;
        call->result = 1;
        desync_api::receive(field<uint8_t>(event, n.sync_sender_offset), text_field(event, n.sync_mission_offset),
                            text_field(event, n.sync_key_offset), g_replay);
        return;
    }
    if (file != kCacheFile || text_field(event, n.sync_mission_offset) != kMission) return;
    std::string key = text_field(event, n.sync_key_offset);
    int sender = field<uint8_t>(event, n.sync_sender_offset);
    int total = field<int32_t>(event, n.sync_value_offset);
    SyncMod mod;
    const bool readable = decode_key(key, mod);
    switch (g_game_log.take(sender)) {
    case LogBudget::Verdict::Log:
        if (!readable) {
            TOME_WARN("Handshake: player %d sent an unreadable mod: %s", sender, key.substr(0, 100).c_str());
        } else if (total > kMaxMods) {
            TOME_WARN("Handshake: player %d sent a set of %d mods; more than %d are ignored", sender, total, kMaxMods);
        } else {
            TOME_INFO("Handshake: player %d has %s (%d in all)", sender, describe(mod).c_str(), total);
        }
        break;
    case LogBudget::Verdict::Exhausted:
        TOME_WARN("Handshake: player %d sent too many mod actions; not logging the rest", sender);
        break;
    case LogBudget::Verdict::Quiet: break;
    }
    if (readable) g_record.add(sender, mod, total);
}

struct HookSpec {
    const char* name;
    int32_t phase;
    tome_hook_fn fn;
};

} // namespace

bool install(uintptr_t game_base, const BuildMappings& build, std::string& report) {
    g_build = &build;
    const auto& n = build.net;
    g_game_base = game_base;
    g_glue_get = reinterpret_cast<GlueGetFn>(game_base + build.game.glue_get);
    g_add_text = reinterpret_cast<AddTextFn>(game_base + n.text_area_add_text);
    g_lobby_init = reinterpret_cast<LobbyInitFn>(game_base + n.lobby_event_init);
    g_send_lobby = reinterpret_cast<SendLobbyFn>(game_base + n.send_lobby_event);
    g_player_name = reinterpret_cast<PlayerNameFn>(game_base + n.player_name);
    g_local_player = reinterpret_cast<NetIndexFn>(game_base + n.local_player);
    g_is_host = reinterpret_cast<NetIndexFn>(game_base + n.is_host);
    g_is_replay = reinterpret_cast<IsReplayFn>(game_base + n.is_replay);
    if (n.get_player_number) g_player_number = reinterpret_cast<PlayerNumberFn>(game_base + n.get_player_number);
    g_sync_init = reinterpret_cast<SyncInitFn>(game_base + n.sync_init);
    g_sync_value_destroy = reinterpret_cast<SyncValueDestroyFn>(game_base + n.sync_value_destroy);
    g_send_action = reinterpret_cast<SendActionFn>(game_base + n.send_action);

    const HookSpec specs[] = {
        {"CGameChatroom::OnChatReceived", TOME_HOOK_BEFORE, before_chat_received},
        {"CGameChatroom::StartGame", TOME_HOOK_BEFORE, before_start_game},
        {"PresentFrame", TOME_HOOK_BEFORE, before_present},
        {"OnNetSyncStoredInteger", TOME_HOOK_BEFORE, before_sync_integer},
    };
    // Resolve every target first, so a failure leaves no half-working set.
    hooks::Target* targets[std::size(specs)];
    for (size_t i = 0; i < std::size(specs); ++i) {
        std::string error;
        targets[i] = hooks::find_or_install(specs[i].name, error);
        if (!targets[i]) {
            report = std::string(specs[i].name) + ": " + error;
            return false;
        }
    }
    for (size_t i = 0; i < std::size(specs); ++i) {
        hooks::Handler handler;
        handler.phase = specs[i].phase;
        handler.priority = INT32_MAX;  // before mods' handlers
        handler.fn = specs[i].fn;
        hooks::add(targets[i], handler);
    }
    report = "hooked the lobby's chat and Start button, and the sync action";
    return true;
}

namespace {

// Locks required mods if this game is a multiplayer game or a replay
// (core/game_lock.h). Checked at the start and again on the first step, in
// case the players weren't all in place yet.
void check_lock() {
    if (mod_manager().game_locked()) return;
    int humans = 0;
    for (int slot = 0; slot < kMaxPlayerSlots; ++slot) humans += units::is_human(slot);
    if (game_lock::applies(g_replay, humans)) mod_manager().set_game_lock(true);
}

} // namespace

void on_game_start() {
    if (!g_build) return;
    net_messages::on_game_start();
    g_record.reset();
    g_game_log.reset();
    g_in_game = true;
    g_replay = g_is_replay() != 0;
    g_sent = false;
    g_reported = false;
    g_host_found = false;
    g_host_slot = -1;
    check_lock();
}

void on_game_tick(double game_time) {
    if (!g_in_game) return;
    // Sent on the first step rather than at the start, once the game runs.
    if (!g_sent) {
        g_sent = true;
        check_lock();
        ModSet local = local_set();
        if (!g_replay && !local.empty()) send_set(local);
        // Again if a mod asked at the start and it wasn't found then.
        if (!g_host_found || g_host_slot < 0) find_host_slot();
    }
    if (!g_reported && game_time >= kReportAfter) {
        g_reported = true;
        report();
    }
}

void on_game_end() {
    g_in_game = false;
    g_host_slot = -1;
    mod_manager().set_game_lock(false);
}

bool in_game() {
    return g_in_game;
}

bool in_replay() {
    return g_in_game && g_replay;
}

bool game_stepped() {
    return g_in_game && g_sent;
}

bool send_sync_action(std::string_view file, std::string_view mission, std::string_view key, int32_t value) {
    if (!g_build || !g_in_game || g_replay) return false;
    send_action(file, mission, key, value);
    return true;
}

int host_slot() {
    if (!g_in_game || !g_build) return -1;
    if (!g_host_found) find_host_slot();
    return g_host_slot;
}

} // namespace tome::handshake
