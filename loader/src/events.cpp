#include "events.h"

#include "core/chat.h"
#include "frames.h"
#include "guard.h"
#include "handshake.h"
#include "hooks.h"
#include "log.h"
#include "main_thread.h"
#include "mod_manager.h"
#include "popups.h"
#include "restart.h"
#include "units.h"
#include "unwind.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

// GCC warns that __thiscall is meant for class methods, but it does apply it
// to function pointer types (see mod_menu.cpp).
#pragma GCC diagnostic ignored "-Wattributes"

namespace tome::events {
namespace {

using GlueGetFn = void*(*)();
using GameUiGetFn = void*(__fastcall*)(int create, int forget);
using DisplayTextFn = void(__thiscall*)(void* game_ui, float x, float y, const char* text, float duration,
                                        uint32_t color);
using StringTextFn = const char*(__thiscall*)(const void* string);

bool g_ready = false;
uintptr_t g_base = 0;
const BuildMappings* g_build = nullptr;
GlueGetFn g_glue_get = nullptr;
GameUiGetFn g_game_ui_get = nullptr;
DisplayTextFn g_display_text = nullptr;
StringTextFn g_string_text = nullptr;

// --- registrations ---------------------------------------------------------------
// A registration stays alive in every snapshot that holds it; `revoked` is
// set (on the main thread, where they run) before the mod's DLL can go.

struct Registration {
    tome_mod* mod = nullptr;
    void* fn = nullptr;
    void* user = nullptr;
    bool revoked = false;
};
using RegPtr = std::shared_ptr<Registration>;
using RegList = std::vector<RegPtr>;

// The highest tome_event: g_handlers has a slot for each, and on_event refuses
// anything above it. Raise it with every new event.
constexpr int kEventCount = TOME_EVENT_UNIT_OWNER_CHANGED;

SRWLOCK g_lock = SRWLOCK_INIT;  // registration may happen on any thread
std::shared_ptr<const RegList> g_handlers[kEventCount + 1];  // copy-on-write, by event
std::map<std::string, RegPtr> g_commands;                      // by lowercase name
std::atomic<int> g_tick_handlers{0};       // TICK
std::atomic<int> g_game_tick_handlers{0};  // GAME_TICK

std::shared_ptr<const RegList> snapshot(int32_t event) {
    if (event < 0 || event > kEventCount) return nullptr;
    SharedLock hold(g_lock);
    return g_handlers[event];
}

// --- game state (main thread) --------------------------------------------------------

bool g_map_loaded = false;  // from the loading screen until the end of its game
bool g_game_started = false;
unsigned g_game_serial = 0;  // counts started games
double g_game_time = 0;      // game seconds since the game started
std::string g_map_name, g_map_path, g_map_description;
units::Selection g_selection;  // the local selection as of the last frame
bool g_paused = false;
units::Results g_results;      // victories, defeats and leaves reported this game
LARGE_INTEGER g_frequency, g_last_tick;
uint32_t g_frames = 0;

tome_event_info make_info(int32_t event, double delta) {
    tome_event_info info = {};
    info.size = sizeof(info);
    info.event = event;
    bool has_map = event == TOME_EVENT_MAP_LOAD || event == TOME_EVENT_GAME_START || event == TOME_EVENT_GAME_END;
    info.map_name = has_map ? g_map_name.c_str() : "";
    info.map_path = has_map ? g_map_path.c_str() : "";
    info.map_description = has_map ? g_map_description.c_str() : "";
    info.delta = delta;
    info.frame = event == TOME_EVENT_TICK ? g_frames : 0;
    info.game_time = event == TOME_EVENT_GAME_TICK || event == TOME_EVENT_GAME_END ? g_game_time : 0;
    info.units = nullptr;
    info.leader = -1;
    info.player = -1;
    info.player_name = "";
    info.text = "";
    info.object_name = "";
    info.save_name = "";
    return info;
}

const char* event_name(int32_t event) {
    switch (event) {
    case TOME_EVENT_MAP_LOAD: return "(map load)";
    case TOME_EVENT_GAME_START: return "(game start)";
    case TOME_EVENT_GAME_END: return "(game end)";
    case TOME_EVENT_TICK: return "(tick)";
    case TOME_EVENT_GAME_TICK: return "(game tick)";
    case TOME_EVENT_SELECTION: return "(selection)";
    case TOME_EVENT_CHAT: return "(chat)";
    case TOME_EVENT_PAUSE: return "(pause)";
    case TOME_EVENT_RESUME: return "(resume)";
    case TOME_EVENT_UNIT_DEATH: return "(unit death)";
    case TOME_EVENT_UNIT_TRAINED: return "(unit trained)";
    case TOME_EVENT_CONSTRUCTION_FINISHED: return "(construction finished)";
    case TOME_EVENT_UPGRADE_FINISHED: return "(upgrade finished)";
    case TOME_EVENT_RESEARCH_FINISHED: return "(research finished)";
    case TOME_EVENT_HERO_LEVEL: return "(hero level)";
    case TOME_EVENT_UNIT_SUMMONED: return "(unit summoned)";
    case TOME_EVENT_ITEM_PICKUP: return "(item pickup)";
    case TOME_EVENT_ITEM_DROP: return "(item drop)";
    case TOME_EVENT_ITEM_USE: return "(item use)";
    case TOME_EVENT_PLAYER_VICTORY: return "(player victory)";
    case TOME_EVENT_PLAYER_DEFEAT: return "(player defeat)";
    case TOME_EVENT_PLAYER_LEFT: return "(player left)";
    case TOME_EVENT_GAME_SAVE: return "(game save)";
    case TOME_EVENT_GAME_LOADED: return "(game loaded)";
    case TOME_EVENT_UNIT_ATTACKED: return "(unit attacked)";
    case TOME_EVENT_UNIT_RESCUED: return "(unit rescued)";
    case TOME_EVENT_CONSTRUCTION_STARTED: return "(construction started)";
    case TOME_EVENT_CONSTRUCTION_CANCELLED: return "(construction cancelled)";
    case TOME_EVENT_UPGRADE_STARTED: return "(upgrade started)";
    case TOME_EVENT_UPGRADE_CANCELLED: return "(upgrade cancelled)";
    case TOME_EVENT_TRAINING_STARTED: return "(training started)";
    case TOME_EVENT_TRAINING_CANCELLED: return "(training cancelled)";
    case TOME_EVENT_RESEARCH_STARTED: return "(research started)";
    case TOME_EVENT_RESEARCH_CANCELLED: return "(research cancelled)";
    case TOME_EVENT_HERO_SKILL: return "(hero skill)";
    case TOME_EVENT_HERO_REVIVABLE: return "(hero revivable)";
    case TOME_EVENT_HERO_REVIVE_STARTED: return "(hero revive started)";
    case TOME_EVENT_HERO_REVIVE_CANCELLED: return "(hero revive cancelled)";
    case TOME_EVENT_HERO_REVIVED: return "(hero revived)";
    case TOME_EVENT_UNIT_ORDER: return "(unit order)";
    case TOME_EVENT_UNIT_LOADED: return "(unit loaded)";
    case TOME_EVENT_UNIT_SOLD: return "(unit sold)";
    case TOME_EVENT_ITEM_SOLD: return "(item sold)";
    case TOME_EVENT_ITEM_PAWNED: return "(item pawned)";
    case TOME_EVENT_UNIT_OWNER_CHANGED: return "(unit owner changed)";
    default: return nullptr;
    }
}

// Calls one handler under a guard; a crash only abandons that handler.
void call_handler(const Registration& r, const tome_event_info& info) {
    guard::run(r.mod, "event handler", event_name(info.event),
               [&] { reinterpret_cast<tome_event_fn>(r.fn)(r.mod, &info, r.user); });
}

bool has_handlers(int32_t event) {
    auto list = snapshot(event);
    return list && !list->empty();
}

// `info` from make_info, with the event's own fields filled in.
void fire_info(const tome_event_info& info) {
    auto list = snapshot(info.event);
    if (!list || list->empty()) return;
    // Dropped if a crash guard further out abandons this round.
    OnUnwind release_list([&] { list.reset(); });
    for (const RegPtr& r : *list) {
        if (!r->revoked) call_handler(*r, info);
    }
}

void fire(int32_t event, double delta = 0) {
    if (has_handlers(event)) fire_info(make_info(event, delta));
}

std::string string_text(const char* rc_string) {
    const char* text = g_string_text(rc_string);
    return text ? text : "";
}

// The map of a CGameWar3, from its map info.
void read_map(const void* game) {
    if (!game) return;
    auto info = *reinterpret_cast<const char* const*>(static_cast<const char*>(game) +
                                                       g_build->game.game_map_info_offset);
    if (!info) return;
    g_map_name = string_text(info + g_build->game.map_name_offset);
    g_map_path = string_text(info + g_build->game.map_path_offset);
    g_map_description = string_text(info + g_build->play.map_description_offset);
}

const void* current_game() {
    return *reinterpret_cast<const void* const*>(g_base + g_build->game.game_instance);
}

// Ends a game that started. A load that never got there (a failed load, or
// the Reset a saved game's loader runs partway through) ends nothing.
void end_game() {
    if (!g_game_started) return;
    read_map(current_game());
    TOME_INFO("Game ended: %s", g_map_name.c_str());
    handshake::on_game_end();
    fire(TOME_EVENT_GAME_END);
    g_map_loaded = false;
    g_game_started = false;
    g_selection = {};
    g_paused = false;
    g_results.reset();
}

int32_t ui_event_for_screen(uint32_t screen) {
    const auto& g = g_build->game;
    const std::pair<uint32_t, int32_t> table[] = {
        {g.screen_single_player, TOME_UI_SINGLE_PLAYER}, {g.screen_custom_game, TOME_UI_CUSTOM_GAME},
        {g.screen_options, TOME_UI_OPTIONS},             {g.screen_campaign, TOME_UI_CAMPAIGN},
        {g.screen_custom_campaign, TOME_UI_CUSTOM_CAMPAIGN}, {g.screen_load_saved_game, TOME_UI_LOAD_SAVED_GAME},
        {g.screen_view_replay, TOME_UI_VIEW_REPLAY},     {g.screen_lan, TOME_UI_LAN},
        {g.screen_battle_net, TOME_UI_BATTLE_NET},       {g.screen_loading, TOME_UI_LOADING},
        {g.screen_score_screen, TOME_UI_SCORE_SCREEN},
    };
    for (const auto& [id, event] : table) {
        if (id == screen) return event;
    }
    return 0;  // the main menu (its own Setup hook) or a screen mods can't ask for
}

// --- chat ------------------------------------------------------------------------------

void say(const std::string& text) {
    display_text("|cffffcc00Tome:|r " + text, 10.0f);
}

void tome_command(const std::string& args) {
    chat::TomeCommand cmd = chat::parse_tome(args);
    auto find = [](const std::string& id) {
        for (const ModStatus& s : mod_manager().status()) {
            if (s.id == id) return true;
        }
        return false;
    };
    switch (cmd.action) {
    case chat::TomeAction::Invalid:
        say(cmd.error);
        return;
    case chat::TomeAction::Help:
        say("/tome list | enable <id> | disable <id> | reload [id]");
        return;
    case chat::TomeAction::List: {
        std::vector<ModStatus> mods = mod_manager().status();
        if (mods.empty()) say("no mods installed");
        for (const ModStatus& s : mods) {
            std::string line = (s.id.empty() ? s.folder : s.id) + " " + s.version + ": " + s.state;
            if (!s.reason.empty()) line += " (" + s.reason + ")";
            say(line);
        }
        return;
    }
    case chat::TomeAction::Enable:
    case chat::TomeAction::Disable:
        if (!find(cmd.id)) {
            say("no mod \"" + cmd.id + "\"; see /tome list");
            return;
        }
        break;
    case chat::TomeAction::Reload:
        if (!cmd.id.empty() && !find(cmd.id)) {
            say("no mod \"" + cmd.id + "\"; see /tome list");
            return;
        }
        break;
    }
    // Outside the game's chat handling, from the message loop, like any other
    // change (the mod being changed may have handlers on this very call).
    main_thread::post([cmd] {
        if (cmd.action == chat::TomeAction::Reload) {
            std::vector<std::string> ids = mod_manager().reload(cmd.id);
            std::vector<ModStatus> mods = mod_manager().status();
            std::string list, waiting;
            for (const std::string& id : ids) {
                bool locked = std::any_of(mods.begin(), mods.end(),
                                          [&](const ModStatus& s) { return s.id == id && !s.locked.empty(); });
                std::string& to = locked ? waiting : list;
                to += (to.empty() ? "" : ", ") + id;
            }
            if (ids.empty()) say("nothing to reload (no running mod with a DLL)");
            if (!list.empty()) say("reloaded " + list);
            if (!waiting.empty()) say(waiting + ": locked during this multiplayer game; reloads when it ends");
            return;
        }
        bool enable = cmd.action == chat::TomeAction::Enable;
        std::vector<std::string> dependents = mod_manager().set_enabled(cmd.id, enable);
        if (!dependents.empty()) {
            std::string list;
            for (const std::string& id : dependents) list += (list.empty() ? "" : ", ") + id;
            say("also disabled " + list + " (they depend on " + cmd.id + ")");
        }
        for (const ModStatus& s : mod_manager().status()) {
            if (s.id != cmd.id) continue;
            if (s.running == enable) {
                say(std::string(enable ? "enabled " : "disabled ") + cmd.id);
            } else if (!s.locked.empty()) {
                say(cmd.id + ": " + s.locked);
            } else {
                say(cmd.id + " is " + s.state + (s.reason.empty() ? "" : ": " + s.reason));
            }
        }
    });
}

// True if the line was a command Tome or a mod handled.
bool handle_chat(const char* text) {
    chat::Command cmd;
    if (!text || !chat::parse(text, cmd)) return false;
    // Dropped if a crash guard further out abandons the call.
    OnUnwind release_cmd([&] {
        std::string().swap(cmd.name);
        std::string().swap(cmd.args);
    });
    if (cmd.name == "tome") {
        TOME_INFO("Chat command: /tome %s", cmd.args.c_str());
        tome_command(cmd.args);
        return true;
    }
    RegPtr reg;
    {
        SharedLock hold(g_lock);
        auto it = g_commands.find(cmd.name);
        if (it != g_commands.end()) reg = it->second;
    }
    if (!reg || reg->revoked) return false;
    OnUnwind release_reg([&] { reg.reset(); });
    TOME_INFO("Chat command: /%s %s (%s)", cmd.name.c_str(), cmd.args.c_str(), reg->mod->id.c_str());
    std::string detail = "/" + cmd.name;
    OnUnwind release_detail([&] { std::string().swap(detail); });
    guard::run(reg->mod, "chat command", detail.c_str(),
               [&] { reinterpret_cast<tome_command_fn>(reg->fn)(reg->mod, cmd.args.c_str(), reg->user); });
    return true;  // even if it crashed: the line was the mod's
}

// --- hook handlers ---------------------------------------------------------------------
// Tome's own handlers: no mod, no gate.

// The loading screen is the one point every way into a game passes (menus,
// -loadfile, LAN), after the natives are bound (CGameWar3::Reset) and before
// the map's script and files are read.
void map_load() {
    end_game();  // a game that never went back to a menu
    g_map_name.clear();
    g_map_path.clear();
    g_map_description.clear();
    g_results.reset();
    read_map(current_game());
    g_map_loaded = true;
    g_game_started = false;
    g_game_time = 0;
    TOME_INFO("Map loading: %s (%s)", g_map_name.c_str(), g_map_path.c_str());
    // Before the map reads any file, so "reload": "map" changes apply to it.
    mod_manager().on_map_load();
    fire(TOME_EVENT_MAP_LOAD);
}

void TOME_CALL after_set_screen(tome_mod*, tome_hook_call* call, void*) {
    int32_t event = ui_event_for_screen(call->args[0]);
    if (event == TOME_UI_SCORE_SCREEN) end_game();  // the game is over once its score screen shows
    if (event == TOME_UI_LOADING) map_load();
    if (!event) return;
    auto glue = static_cast<char*>(g_glue_get());
    void* screen = glue ? *reinterpret_cast<void**>(glue + g_build->game.glue_screen_offset) : nullptr;
    if (screen) frames::on_screen(event, screen);
}

// Reset runs when a game is left, but also when a lobby is created or left
// and while a saved game loads, so it only ends a game that started.
void TOME_CALL before_reset(tome_mod*, tome_hook_call*, void*) {
    end_game();
}

void TOME_CALL after_enter_game(tome_mod*, tome_hook_call*, void*) {
    if (!g_map_loaded || g_game_started) return;
    g_game_started = true;
    ++g_game_serial;
    g_game_time = 0;
    g_selection = units::read_selection();
    g_paused = false;
    read_map(current_game());  // a saved game's map info is only complete now
    TOME_INFO("Game started: %s", g_map_name.c_str());
    if (void* game_ui = g_game_ui_get(0, 0)) frames::on_screen(TOME_UI_GAME, game_ui);
    handshake::on_game_start();
    fire(TOME_EVENT_GAME_START);
}

// The selection changes in many places (clicks, drag boxes, control groups,
// Tab, triggers, units dying), so it's compared once per frame instead of
// hooking each of them: a dozen pointers at most.
void check_selection() {
    units::Selection now = units::read_selection();
    if (now == g_selection) return;
    g_selection = std::move(now);
    if (!has_handlers(TOME_EVENT_SELECTION)) return;
    std::vector<tome_unit> list;
    // Dropped if a crash guard further out abandons the round.
    OnUnwind release_list([&] { std::vector<tome_unit>().swap(list); });
    tome_event_info info = make_info(TOME_EVENT_SELECTION, 0);
    units::describe(g_selection, list, info.leader);
    info.units = list.data();
    info.unit_count = static_cast<uint32_t>(list.size());
    fire_info(info);
}

void TOME_CALL before_present(tome_mod*, tome_hook_call*, void*) {
    if (!frames::on_main_thread()) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double delta = g_frames ? double(now.QuadPart - g_last_tick.QuadPart) / double(g_frequency.QuadPart) : 0.0;
    g_last_tick = now;
    ++g_frames;
    if (g_tick_handlers.load(std::memory_order_relaxed) > 0) fire(TOME_EVENT_TICK, delta);
    if (g_game_started) check_selection();
    popups::on_frame();
}

void TOME_CALL before_game_tick(tome_mod*, tome_hook_call* call, void*) {
    if (!g_game_started) return;
    float step;
    std::memcpy(&step,
                reinterpret_cast<const char*>(static_cast<uintptr_t>(call->args[0])) +
                    g_build->game.game_tick_step_offset,
                sizeof(step));
    g_game_time += step;
    handshake::on_game_tick(g_game_time);
    if (g_game_tick_handlers.load(std::memory_order_relaxed) > 0) fire(TOME_EVENT_GAME_TICK, step);
}

// After the main loop, before the game tears its systems down. The game was
// normally left already (Reset runs on the way out); this catches an exit
// that skipped it. Then the mods get disabled and unloaded.
void TOME_CALL before_shutdown(tome_mod*, tome_hook_call*, void*) {
    end_game();
    mod_manager().shutdown();
    restart::on_shutdown();
}

void TOME_CALL before_chat_message(tome_mod*, tome_hook_call* call, void*) {
    if (!g_game_started || !has_handlers(TOME_EVENT_CHAT)) return;
    const char* text = reinterpret_cast<const char*>(static_cast<uintptr_t>(call->args[2]));
    if (!text) return;
    auto player = static_cast<int32_t>(call->args[1]);
    std::string name = units::player_name(player);
    OnUnwind release_name([&] { std::string().swap(name); });
    tome_event_info info = make_info(TOME_EVENT_CHAT, 0);
    info.player = player;
    info.player_name = name.c_str();
    info.text = text;
    info.recipient = units::chat_recipient(static_cast<int32_t>(call->args[3]));
    fire_info(info);
}

// SetPaused is the only writer of the two pause fields, so reading them
// afterwards tells whether this call paused or resumed the game (it can do
// neither: a player's resume doesn't end a trigger's pause).
void TOME_CALL after_set_paused(tome_mod*, tome_hook_call* call, void*) {
    if (!g_game_started) return;
    auto game_ui = reinterpret_cast<const char*>(static_cast<uintptr_t>(call->args[0]));
    int32_t by_player, by_trigger;
    std::memcpy(&by_player, game_ui + g_build->play.paused_by_player_offset, sizeof(by_player));
    std::memcpy(&by_trigger, game_ui + g_build->play.paused_by_trigger_offset, sizeof(by_trigger));
    bool paused = by_player || by_trigger;
    if (paused == g_paused) return;
    g_paused = paused;
    int32_t player = units::pause_player(call->args[2], call->args[5]);
    TOME_INFO("Game %s (player %d)", paused ? "paused" : "resumed", player);
    int32_t event = paused ? TOME_EVENT_PAUSE : TOME_EVENT_RESUME;
    if (!has_handlers(event)) return;
    std::string name = units::player_name(player);
    OnUnwind release_name([&] { std::string().swap(name); });
    tome_event_info info = make_info(event, 0);
    info.player = player;
    info.player_name = name.c_str();
    fire_info(info);
}

// --- unit, player and saved game events --------------------------------------------
// The game's own functions for its JASS events (CPlayerWar3::On*), hooked
// before they check whether a trigger listens. this (args[0]) is the
// player whose event it is.

// Which argument (index into args, -1 for none) holds what.
enum class Special { None, HeroLevel, PreviousOwner };

struct UnitEventSpec {
    int32_t event;
    int unit = 1;
    int other = -1;
    int third = -1;
    int item = -1;
    int type = -1;   // object_id
    int level = -1;
    Special special = Special::None;
};

void* arg_ptr(const tome_hook_call* call, int index) {
    return index < 0 ? nullptr : reinterpret_cast<void*>(static_cast<uintptr_t>(call->args[index]));
}

void set_object(tome_event_info& info, uint32_t id) {
    info.object_id = id;
    info.object_name = units::object_name(id);
}

void TOME_CALL before_unit_event(tome_mod*, tome_hook_call* call, void* user) {
    const auto& spec = *static_cast<const UnitEventSpec*>(user);
    // From the loading screen on: a map's script runs its setup (creating
    // units, levelling heroes) while it loads.
    if (!g_map_loaded || !has_handlers(spec.event)) return;
    void* unit_object = arg_ptr(call, spec.unit);
    if (!unit_object) return;
    void* other_object = arg_ptr(call, spec.other);
    void* third_object = arg_ptr(call, spec.third);
    tome_unit unit, other, third;
    units::describe_unit(unit_object, unit);
    if (other_object) units::describe_unit(other_object, other);
    if (third_object) units::describe_unit(third_object, third);
    tome_event_info info = make_info(spec.event, 0);
    info.unit = &unit;
    info.other = other_object ? &other : nullptr;
    info.third = third_object ? &third : nullptr;
    if (spec.item >= 0) {
        info.item = arg_ptr(call, spec.item);
        set_object(info, units::item_type(info.item));
    }
    if (spec.type >= 0) set_object(info, call->args[spec.type]);
    if (spec.level >= 0) info.level = static_cast<int32_t>(call->args[spec.level]);
    if (spec.special == Special::HeroLevel) info.level = units::hero_level(unit_object);
    if (spec.special == Special::PreviousOwner) info.player = static_cast<int32_t>(call->args[2] & 0xFFFF);
    fire_info(info);
}

// CUnit::On*Order (this = the unit, args[1] = the order), before they check
// whether a trigger listens. user is the tome_order_type.
void TOME_CALL before_order(tome_mod*, tome_hook_call* call, void* user) {
    if (!g_map_loaded || !has_handlers(TOME_EVENT_UNIT_ORDER)) return;
    void* unit_object = arg_ptr(call, 0);
    void* order = arg_ptr(call, 1);
    if (!unit_object || !order) return;
    units::Order o = units::read_order(order, static_cast<int32_t>(reinterpret_cast<intptr_t>(user)));
    tome_unit unit, target;
    units::describe_unit(unit_object, unit);
    if (o.target_unit) units::describe_unit(o.target_unit, target);
    tome_event_info info = make_info(TOME_EVENT_UNIT_ORDER, 0);
    info.unit = &unit;
    info.other = o.target_unit ? &target : nullptr;
    info.order_id = o.id;
    info.order_type = o.type;
    info.x = o.x;
    info.y = o.y;
    if (o.target_item) {
        info.item = o.target_item;
        set_object(info, units::item_type(o.target_item));
    }
    fire_info(info);
}

void report_result(int32_t event, int32_t player) {
    if (player < 0 || !g_results.first(event, player)) return;
    std::string name = units::player_name(player);
    OnUnwind release_name([&] { std::string().swap(name); });
    TOME_INFO("Player %d (%s) %s", player, name.c_str(),
              event == TOME_EVENT_PLAYER_VICTORY ? "won" : event == TOME_EVENT_PLAYER_DEFEAT ? "lost" : "left");
    if (!has_handlers(event)) return;
    tome_event_info info = make_info(event, 0);
    info.player = player;
    info.player_name = name.c_str();
    fire_info(info);
}

// RemovePlayer(player, result) only applies while the player is still in
// the game (CPlayerWar3.game_state 1). Victory and defeat; ties and neutral
// results aren't reported.
void TOME_CALL before_remove_player(tome_mod*, tome_hook_call* call, void*) {
    const void* player = arg_ptr(call, 0);
    if (!g_map_loaded || !player || units::player_state(player) != 1) return;
    if (call->args[1] == 0) report_result(TOME_EVENT_PLAYER_VICTORY, units::player_slot(player));
    if (call->args[1] == 1) report_result(TOME_EVENT_PLAYER_DEFEAT, units::player_slot(player));
}

void TOME_CALL before_leave(tome_mod*, tome_hook_call* call, void*) {
    if (g_map_loaded) report_result(TOME_EVENT_PLAYER_LEFT, units::player_slot(arg_ptr(call, 0)));
}

void TOME_CALL before_game_save(tome_mod*, tome_hook_call* call, void*) {
    const char* name = static_cast<const char*>(arg_ptr(call, 0));
    if (!g_game_started || !name || !*name) return;
    TOME_INFO("Saving the game: %s", name);
    tome_event_info info = make_info(TOME_EVENT_GAME_SAVE, 0);
    info.save_name = name;
    fire_info(info);
}

// Inside the saved game loader, once the game's state is read, before
// EnterGame. The load request left the saved game's path in the new game
// (CGameWar3.load_path); without one, the map's path stands in.
void TOME_CALL before_game_loaded(tome_mod*, tome_hook_call*, void*) {
    const char* game = static_cast<const char*>(current_game());
    if (!g_map_loaded || !game) return;
    read_map(game);
    std::string path;
    OnUnwind release_path([&] { std::string().swap(path); });
    uint32_t is_save;
    std::memcpy(&is_save, game + g_build->play.load_is_save_offset, sizeof(is_save));
    if (is_save) path = string_text(game + g_build->play.load_path_offset);
    if (path.empty()) path = g_map_path;
    TOME_INFO("Saved game loaded: %s", path.c_str());
    tome_event_info info = make_info(TOME_EVENT_GAME_LOADED, 0);
    info.save_name = path.c_str();
    fire_info(info);
}

void* order_user(int32_t type) {
    return reinterpret_cast<void*>(static_cast<intptr_t>(type));
}

// The game's event functions and which argument is which (see the
// mapping's signatures; args[0] is this).
struct UnitHook {
    const char* name;
    UnitEventSpec spec;
};

const UnitHook kUnitHooks[] = {
    {"CPlayerWar3::OnUnitDeath", {.event = TOME_EVENT_UNIT_DEATH, .other = 2}},
    {"CPlayerWar3::OnTrainFinish", {.event = TOME_EVENT_UNIT_TRAINED, .unit = 2, .other = 1}},
    {"CPlayerWar3::OnConstructFinish", {.event = TOME_EVENT_CONSTRUCTION_FINISHED}},
    {"CPlayerWar3::OnUpgradeFinish", {.event = TOME_EVENT_UPGRADE_FINISHED}},
    {"CPlayerWar3::OnResearchFinish", {.event = TOME_EVENT_RESEARCH_FINISHED, .type = 2}},
    {"CPlayerWar3::OnHeroLevel", {.event = TOME_EVENT_HERO_LEVEL, .special = Special::HeroLevel}},
    {"CPlayerWar3::OnSummon", {.event = TOME_EVENT_UNIT_SUMMONED, .unit = 2, .other = 1}},
    {"CPlayerWar3::OnPickupItem", {.event = TOME_EVENT_ITEM_PICKUP, .item = 2}},
    {"CPlayerWar3::OnDropItem", {.event = TOME_EVENT_ITEM_DROP, .item = 2}},
    {"CPlayerWar3::OnUseItem", {.event = TOME_EVENT_ITEM_USE, .item = 2}},
    {"CPlayerWar3::OnUnitAttacked", {.event = TOME_EVENT_UNIT_ATTACKED, .unit = 2, .other = 1}},
    {"CPlayerWar3::OnUnitRescued", {.event = TOME_EVENT_UNIT_RESCUED, .unit = 2, .other = 1}},
    {"CPlayerWar3::OnConstructStart", {.event = TOME_EVENT_CONSTRUCTION_STARTED}},
    {"CPlayerWar3::OnConstructCancel", {.event = TOME_EVENT_CONSTRUCTION_CANCELLED}},
    {"CPlayerWar3::OnUpgradeStart", {.event = TOME_EVENT_UPGRADE_STARTED, .type = 2}},
    {"CPlayerWar3::OnUpgradeCancel", {.event = TOME_EVENT_UPGRADE_CANCELLED, .type = 2}},
    {"CPlayerWar3::OnTrainStart", {.event = TOME_EVENT_TRAINING_STARTED, .type = 2}},
    {"CPlayerWar3::OnTrainCancel", {.event = TOME_EVENT_TRAINING_CANCELLED, .type = 2}},
    {"CPlayerWar3::OnResearchStart", {.event = TOME_EVENT_RESEARCH_STARTED, .type = 2}},
    {"CPlayerWar3::OnResearchCancel", {.event = TOME_EVENT_RESEARCH_CANCELLED, .type = 2}},
    {"CPlayerWar3::OnHeroSkill", {.event = TOME_EVENT_HERO_SKILL, .type = 2, .level = 3}},
    {"CPlayerWar3::OnHeroRevivable", {.event = TOME_EVENT_HERO_REVIVABLE}},
    {"CPlayerWar3::OnHeroReviveStart", {.event = TOME_EVENT_HERO_REVIVE_STARTED}},
    {"CPlayerWar3::OnHeroReviveCancel", {.event = TOME_EVENT_HERO_REVIVE_CANCELLED}},
    {"CPlayerWar3::OnHeroReviveFinish", {.event = TOME_EVENT_HERO_REVIVED}},
    {"CPlayerWar3::OnUnitSell", {.event = TOME_EVENT_UNIT_SOLD, .unit = 2, .other = 1, .third = 3}},
    {"CPlayerWar3::OnItemSell", {.event = TOME_EVENT_ITEM_SOLD, .other = 2, .item = 3}},
    {"CPlayerWar3::OnItemPawn", {.event = TOME_EVENT_ITEM_PAWNED, .other = 2, .item = 3}},
    {"CPlayerWar3::OnUnitChangeOwner", {.event = TOME_EVENT_UNIT_OWNER_CHANGED, .special = Special::PreviousOwner}},
    // A CUnit function (this = the loaded unit): its player function is
    // only called when a trigger listens.
    {"CUnit::OnLoaded", {.event = TOME_EVENT_UNIT_LOADED, .unit = 0, .other = 1}},
};

void TOME_CALL before_slash_command(tome_mod*, tome_hook_call* call, void*) {
    if (!handle_chat(reinterpret_cast<const char*>(static_cast<uintptr_t>(call->args[1])))) return;
    call->skip_original = 1;
    call->result = 1;  // handled: the game closes the chat box and sends nothing
}

// --- API -------------------------------------------------------------------------------

bool valid_event(int32_t event) {
    return event >= TOME_EVENT_MAP_LOAD && event <= kEventCount;
}

void remove_handler(int32_t event, const RegPtr& reg) {
    ExclusiveLock hold(g_lock);
    reg->revoked = true;
    auto list = std::make_shared<RegList>(*g_handlers[event]);
    std::erase(*list, reg);
    g_handlers[event] = std::move(list);
    if (event == TOME_EVENT_TICK) --g_tick_handlers;
    if (event == TOME_EVENT_GAME_TICK) --g_game_tick_handlers;
}

// A GAME_START handler registered while a game runs gets that game's start,
// from the message loop (after tome_mod_enable returns), unless the game has
// ended or the registration was revoked by then.
void catch_up_game_start(const RegPtr& reg) {
    main_thread::post([reg, serial = g_game_serial] {
        if (reg->revoked || !g_game_started || g_game_serial != serial) return;
        call_handler(*reg, make_info(TOME_EVENT_GAME_START, 0));
    });
}

tome_result TOME_CALL api_on_event(tome_mod* mod, int32_t event, tome_event_fn fn, void* user) {
    if (!mod || !fn || !valid_event(event)) return TOME_ERR_INVALID_ARGUMENT;
    if (!g_ready) return TOME_ERR_UNAVAILABLE;
    auto reg = std::make_shared<Registration>();
    reg->mod = mod;
    reg->fn = reinterpret_cast<void*>(fn);
    reg->user = user;
    {
        ExclusiveLock hold(g_lock);
        auto list = std::make_shared<RegList>(*g_handlers[event]);
        list->push_back(reg);
        g_handlers[event] = std::move(list);
        if (event == TOME_EVENT_TICK) ++g_tick_handlers;
        if (event == TOME_EVENT_GAME_TICK) ++g_game_tick_handlers;
    }
    mod->add_revoker([event, reg] { remove_handler(event, reg); });
    // Mid-game (main thread only: the game state isn't read anywhere else).
    if (event == TOME_EVENT_GAME_START && frames::on_main_thread() && g_game_started) catch_up_game_start(reg);
    return TOME_OK;
}

tome_result TOME_CALL api_register_command(tome_mod* mod, const char* name, tome_command_fn fn, void* user) {
    if (!mod || !name || !fn) return TOME_ERR_INVALID_ARGUMENT;
    if (!chat::valid_name(name) || chat::reserved(name)) {
        TOME_ERROR("[%s] register_command(%s): not a usable command name", mod->id.c_str(), name);
        return TOME_ERR_INVALID_ARGUMENT;
    }
    if (!g_ready) return TOME_ERR_UNAVAILABLE;
    std::string key = chat::lowercase(name);
    auto reg = std::make_shared<Registration>();
    reg->mod = mod;
    reg->fn = reinterpret_cast<void*>(fn);
    reg->user = user;
    ExclusiveLock hold(g_lock);
    auto it = g_commands.find(key);
    tome_mod* owner = it != g_commands.end() ? it->second->mod : nullptr;
    if (owner && owner != mod) {
        hold.unlock();
        TOME_ERROR("[%s] register_command(%s): /%s belongs to %s", mod->id.c_str(), name, key.c_str(),
                   owner->id.c_str());
        return TOME_ERR_NOT_OWNER;
    }
    if (owner) it->second->revoked = true;  // registering again replaces it
    g_commands[key] = reg;
    hold.unlock();
    mod->add_revoker([key, reg] {
        ExclusiveLock hold(g_lock);
        reg->revoked = true;
        auto it = g_commands.find(key);
        if (it != g_commands.end() && it->second == reg) g_commands.erase(it);
    });
    return TOME_OK;
}

tome_result TOME_CALL api_display_text(const char* text, float seconds) {
    if (!text || !(seconds > 0)) return TOME_ERR_INVALID_ARGUMENT;
    if (!g_ready) return TOME_ERR_UNAVAILABLE;
    if (!frames::on_main_thread()) {
        TOME_ERROR("display_text called off the game's main thread (thread %lu); ignored", GetCurrentThreadId());
        return TOME_ERR_WRONG_THREAD;
    }
    return display_text(text, seconds) ? TOME_OK : TOME_ERR_UNAVAILABLE;
}

struct HookSpec {
    const char* name;
    int32_t phase;
    tome_hook_fn fn;
    void* user = nullptr;
};

} // namespace

bool install(uintptr_t game_base, const BuildMappings& build, std::string& report) {
    g_base = game_base;
    g_build = &build;
    g_glue_get = reinterpret_cast<GlueGetFn>(game_base + build.game.glue_get);
    g_game_ui_get = reinterpret_cast<GameUiGetFn>(game_base + build.game.game_ui_get);
    g_display_text = reinterpret_cast<DisplayTextFn>(game_base + build.game.game_ui_display_text);
    g_string_text = reinterpret_cast<StringTextFn>(game_base + build.jass.string_get_text);
    units::init(game_base, build);
    QueryPerformanceFrequency(&g_frequency);
    for (auto& list : g_handlers) list = std::make_shared<RegList>();

    hooks::init(game_base, build.data);
    std::vector<HookSpec> specs = {
        {"CGlueMgr::SetScreen", TOME_HOOK_AFTER, after_set_screen},
        {"CGameWar3::Reset", TOME_HOOK_BEFORE, before_reset},
        {"CGlueMgr::EnterGame", TOME_HOOK_AFTER, after_enter_game},
        {"PresentFrame", TOME_HOOK_BEFORE, before_present},
        {"OnNetGameTick", TOME_HOOK_BEFORE, before_game_tick},
        {"GameShutdown", TOME_HOOK_BEFORE, before_shutdown},
        {"CGameUI::HandleSlashCommand", TOME_HOOK_BEFORE, before_slash_command},
        {"CGameUI::DisplayChatMessage", TOME_HOOK_BEFORE, before_chat_message},
        {"CGameUI::SetPaused", TOME_HOOK_AFTER, after_set_paused},
        {"CUnit::OnOrder", TOME_HOOK_BEFORE, before_order, order_user(TOME_ORDER_IMMEDIATE)},
        {"CUnit::OnPointOrder", TOME_HOOK_BEFORE, before_order, order_user(TOME_ORDER_POINT)},
        {"CUnit::OnTargetOrder", TOME_HOOK_BEFORE, before_order, order_user(TOME_ORDER_TARGET)},
        {"CPlayerWar3::RemovePlayer", TOME_HOOK_BEFORE, before_remove_player},
        {"CPlayerWar3::OnLeave", TOME_HOOK_BEFORE, before_leave},
        {"CGameWar3::SaveGame", TOME_HOOK_BEFORE, before_game_save},
        {"HandleManager::FireGameLoaded", TOME_HOOK_BEFORE, before_game_loaded},
    };
    for (const UnitHook& h : kUnitHooks) {
        specs.push_back({h.name, TOME_HOOK_BEFORE, before_unit_event, const_cast<UnitEventSpec*>(&h.spec)});
    }
    // Resolve every target first, so a failure leaves no half-working set.
    std::vector<hooks::Target*> targets(specs.size());
    for (size_t i = 0; i < specs.size(); ++i) {
        std::string error;
        targets[i] = hooks::find_or_install(specs[i].name, error);
        if (!targets[i]) {
            report = std::string(specs[i].name) + ": " + error;
            return false;
        }
    }
    for (size_t i = 0; i < specs.size(); ++i) {
        hooks::Handler handler;
        handler.phase = specs[i].phase;
        // Before mods' handlers, and after them in the AFTER phase.
        handler.priority = specs[i].phase == TOME_HOOK_AFTER ? INT32_MIN : INT32_MAX;
        handler.fn = specs[i].fn;
        handler.user = specs[i].user;
        hooks::add(targets[i], handler);
    }
    g_ready = true;
    report = "hooked screens, map load, game start and end, frames, game ticks, shutdown, chat, pause, units, "
             "players and saved games";
    return true;
}

void fill_api(tome_api& api) {
    api.on_event = api_on_event;
    api.register_command = api_register_command;
    api.display_text = api_display_text;
}

bool display_text(const std::string& text, float seconds) {
    if (!g_ready || !g_game_started) return false;
    void* game_ui = g_game_ui_get(0, 0);
    if (!game_ui) return false;
    g_display_text(game_ui, 0.0f, 0.0f, text.c_str(), seconds, 0xFFFFFFFF);
    return true;
}

} // namespace tome::events
