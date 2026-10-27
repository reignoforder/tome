#include "handshake.h"

#include "core/handshake.h"
#include "events.h"
#include "frames.h"
#include "hooks.h"
#include "log.h"
#include "mod_manager.h"
#include "units.h"

#include <windows.h>

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

// The required mods running here, as the handshake compares them.
ModSet local_set() {
    ModSet set;
    for (const ModStatus& s : mod_manager().status()) {
        if (s.running && s.sync == "required") set.push_back({s.id, s.version, s.sync_hash});
    }
    sort(set);
    return set;
}

// --- the lobby -----------------------------------------------------------------------

std::unique_ptr<Lobby> g_lobby;
void* g_lobby_room = nullptr;  // the CGameChatroom g_lobby belongs to
double g_last_poll = 0;

int net_index(void* room) {
    return field<int32_t>(room, g_build->net.chatroom_net_index_offset);
}

void show(void* room, const std::string& text) {
    void* area = field<void*>(room, g_build->net.chatroom_chat_area_offset);
    if (!area) return;
    std::string line = kNotePrefix + text;
    g_add_text(area, line.c_str());
}

void send_line(void* room, int to, const std::string& text) {
    const auto& n = g_build->net;
    std::vector<uint32_t> buffer((n.lobby_event_size + 0x40) / 4, 0);
    g_lobby_init(buffer.data(), static_cast<int>(n.lobby_event_chat));
    set_text_field(buffer.data(), n.lobby_event_text_offset, text);
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

// The lobby for `room`, a new one if the lobby changed.
Lobby& lobby_for(void* room) {
    if (!g_lobby || g_lobby_room != room) {
        g_lobby = std::make_unique<Lobby>();
        g_lobby_room = room;
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
    // What the lobby looks like to Tome, logged when it changes.
    std::string seen = "net index " + std::to_string(index) + ", local id " + std::to_string(local) +
                       (host ? ", hosting" : ", joined") + ", players:";
    for (const LobbyPlayer& p : others) seen += " " + std::to_string(p.id) + " " + p.name;
    static std::string s_last_seen;
    if (seen != s_last_seen) {
        s_last_seen = seen;
        TOME_INFO("Handshake: lobby: %s", seen.c_str());
    }
    Lobby& lobby = lobby_for(room);
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
    TOME_INFO("Handshake: from lobby player %d: %s", from, text.c_str());
    if (!frames::is_class(room, "CGameChatroom")) return;
    // A line can arrive before the first poll: the lobby must know whether it
    // hosts and its own mods first, or it drops a reply or answers "none".
    if (!g_lobby || g_lobby_room != room) poll_lobby();
    Lobby& lobby = lobby_for(room);
    lobby.receive(from, text);
    flush(room);
}

void TOME_CALL before_start_game(tome_mod*, tome_hook_call* call, void*) {
    void* room = reinterpret_cast<void*>(static_cast<uintptr_t>(call->args[0]));
    if (!frames::is_class(room, "CGameChatroom") || !g_is_host(net_index(room))) return;
    poll_lobby();  // with the latest players and mods
    if (!g_lobby || g_lobby_room != room) {
        // The lobby couldn't be read (no local player id): nothing was
        // checked, so a host with required mods doesn't start blind.
        if (local_set().empty()) return;
        call->skip_original = 1;
        TOME_WARN("Handshake: not starting, Tome couldn't read the lobby's players");
        show(room, "|cffff4040The game can't start: Tome couldn't check the players' required mods.|r");
        show(room, "To start without the check, leave the lobby and turn your required mods off in the Mods menu.");
        return;
    }
    std::vector<std::string> problems;
    if (g_lobby->can_start(problems)) {
        TOME_INFO("Handshake: starting; every player's required mods match");
        return;
    }
    call->skip_original = 1;
    TOME_WARN("Handshake: not starting, the players' required mods differ");
    show(room, "|cffff4040The game can't start: every player needs the same required mods.|r");
    for (const std::string& p : problems) {
        TOME_WARN("Handshake:   %s", p.c_str());
        show(room, p);
    }
}

// --- games ---------------------------------------------------------------------------

GameRecord g_record;
bool g_in_game = false;
bool g_replay = false;
bool g_sent = false;
bool g_reported = false;

void send_set(const ModSet& set) {
    const auto& n = g_build->net;
    for (const SyncMod& mod : set) {
        std::vector<uint32_t> buffer((n.sync_size + 0x40) / 4, 0);
        void* action = buffer.data();
        g_sync_init(action);
        set_text_field(action, n.sync_file_offset, kCacheFile);
        set_text_field(action, n.sync_mission_offset, kMission);
        set_text_field(action, n.sync_key_offset, encode_key(mod));
        int32_t total = static_cast<int32_t>(set.size());
        std::memcpy(static_cast<char*>(action) + n.sync_value_offset, &total, sizeof(total));
        g_send_action(action, 0);
        g_sync_value_destroy(static_cast<char*>(action) + n.sync_value_object_offset);
    }
    TOME_INFO("Handshake: sent this game %zu required mod(s): %s", set.size(), describe(set).c_str());
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
    if (text_field(event, n.sync_file_offset) != kCacheFile || text_field(event, n.sync_mission_offset) != kMission) {
        return;
    }
    std::string key = text_field(event, n.sync_key_offset);
    int sender = field<uint8_t>(event, n.sync_sender_offset);
    int total = field<int32_t>(event, n.sync_value_offset);
    SyncMod mod;
    if (!decode_key(key, mod)) {
        TOME_WARN("Handshake: player %d sent an unreadable mod: %s", sender, key.c_str());
        return;
    }
    TOME_INFO("Handshake: player %d has %s (%d in all)", sender, describe(mod).c_str(), total);
    g_record.add(sender, mod, total);
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
    g_glue_get = reinterpret_cast<GlueGetFn>(game_base + build.game.glue_get);
    g_add_text = reinterpret_cast<AddTextFn>(game_base + n.text_area_add_text);
    g_lobby_init = reinterpret_cast<LobbyInitFn>(game_base + n.lobby_event_init);
    g_send_lobby = reinterpret_cast<SendLobbyFn>(game_base + n.send_lobby_event);
    g_player_name = reinterpret_cast<PlayerNameFn>(game_base + n.player_name);
    g_local_player = reinterpret_cast<NetIndexFn>(game_base + n.local_player);
    g_is_host = reinterpret_cast<NetIndexFn>(game_base + n.is_host);
    g_is_replay = reinterpret_cast<IsReplayFn>(game_base + n.is_replay);
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

void on_game_start() {
    if (!g_build) return;
    g_record.reset();
    g_in_game = true;
    g_replay = g_is_replay() != 0;
    g_sent = false;
    g_reported = false;
}

void on_game_tick(double game_time) {
    if (!g_in_game) return;
    // Sent on the first step rather than at the start, once the game runs.
    if (!g_sent) {
        g_sent = true;
        ModSet local = local_set();
        if (!g_replay && !local.empty()) send_set(local);
    }
    if (!g_reported && game_time >= kReportAfter) {
        g_reported = true;
        report();
    }
}

void on_game_end() {
    g_in_game = false;
}

} // namespace tome::handshake
