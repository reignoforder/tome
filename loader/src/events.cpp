// SPDX-License-Identifier: GPL-3.0-or-later
#include "events.h"

#include "core/chat.h"
#include "core/game_clock.h"
#include "core/jass.h"
#include "core/log_view.h"
#include "crash_loop.h"
#include "desync.h"
#include "storm.h"
#include "frames.h"
#include "game_build.h"
#include "guard.h"
#include "handshake.h"
#include "hooks.h"
#include "log.h"
#include "main_thread.h"
#include "mod_manager.h"
#include "popups.h"
#include "restart.h"
#include "timers.h"
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
constexpr int kEventCount = TOME_EVENT_MODS_CHANGED;

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
// Game time since the game started: whole microseconds, summed the same way
// on every computer (core/game_clock.h; timers and desync rounds use it).
game_clock::Micros g_game_time = 0;
std::string g_map_name, g_map_path, g_map_description;
units::Selection g_selection;  // the local selection as of the last frame
bool g_paused = false;
units::Results g_results;      // victories, defeats and leaves reported this game
units::ResourceWatch g_resources;  // players' resources as of the last game tick
double g_time_of_day = -1;         // as of the last game tick; -1 for none
int g_reset_depth = 0;             // inside CGameWar3::Reset (its BEFORE ran, its AFTER not yet)
LARGE_INTEGER g_frequency, g_last_tick;
std::atomic<uint32_t> g_frames{0};  // PresentFrame calls; read by debug_input's thread
// The current map script's locals that shadow a global of another type (the
// memhack pattern), found at map load; main thread only.
std::vector<tome::jass::ShadowFinding> g_script_shadows;

tome_event_info make_info(int32_t event, double delta) {
    tome_event_info info = {};
    info.size = sizeof(info);
    info.event = event;
    bool has_map = event == TOME_EVENT_MAP_LOAD || event == TOME_EVENT_GAME_START || event == TOME_EVENT_GAME_END;
    info.map_name = has_map ? g_map_name.c_str() : "";
    info.map_path = has_map ? g_map_path.c_str() : "";
    info.map_description = has_map ? g_map_description.c_str() : "";
    info.script_shadows = has_map ? static_cast<uint32_t>(g_script_shadows.size()) : 0;
    info.delta = delta;
    info.frame = event == TOME_EVENT_TICK ? g_frames.load(std::memory_order_relaxed) : 0;
    info.game_time =
        event == TOME_EVENT_GAME_TICK || event == TOME_EVENT_GAME_END ? game_clock::to_seconds(g_game_time) : 0;
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
    case TOME_EVENT_SPELL_CHANNEL: return "(spell channel)";
    case TOME_EVENT_SPELL_CAST: return "(spell cast)";
    case TOME_EVENT_SPELL_EFFECT: return "(spell effect)";
    case TOME_EVENT_SPELL_FINISH: return "(spell finish)";
    case TOME_EVENT_SPELL_END_CAST: return "(spell end cast)";
    case TOME_EVENT_UNIT_DAMAGED: return "(unit damaged)";
    case TOME_EVENT_UNIT_ACQUIRED_TARGET: return "(unit acquired target)";
    case TOME_EVENT_UNIT_DECAY: return "(unit decay)";
    case TOME_EVENT_UNIT_DETECTED: return "(unit detected)";
    case TOME_EVENT_UNIT_HIDDEN: return "(unit hidden)";
    case TOME_EVENT_ESC_PRESSED: return "(Esc pressed)";
    case TOME_EVENT_ARROW_KEY: return "(arrow key)";
    case TOME_EVENT_DESTRUCTABLE_DEATH: return "(destructable death)";
    case TOME_EVENT_ITEM_DEATH: return "(item death)";
    case TOME_EVENT_DIALOG_CLICK: return "(dialog click)";
    case TOME_EVENT_SKILL_MENU: return "(skill menu)";
    case TOME_EVENT_BUILD_MENU: return "(build menu)";
    case TOME_EVENT_LEVEL_END: return "(level end)";
    case TOME_EVENT_UNIT_CREATED: return "(unit created)";
    case TOME_EVENT_UNIT_REMOVED: return "(unit removed)";
    case TOME_EVENT_HERO_XP: return "(hero experience)";
    case TOME_EVENT_PLAYER_RESOURCE: return "(player resource)";
    case TOME_EVENT_DAY: return "(day)";
    case TOME_EVENT_NIGHT: return "(night)";
    case TOME_EVENT_MODS_CHANGED: return "(mods changed)";
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
    timers_api::on_game_end();
    g_map_loaded = false;
    g_game_started = false;
    g_selection = {};
    g_paused = false;
    g_results.reset();
    g_resources.reset();
    g_time_of_day = -1;
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

// For tome.log ("Screen: custom game"), which tests wait for.
const char* screen_name(int32_t event) {
    switch (event) {
    case TOME_UI_SINGLE_PLAYER: return "single player";
    case TOME_UI_CUSTOM_GAME: return "custom game";
    case TOME_UI_OPTIONS: return "options";
    case TOME_UI_CAMPAIGN: return "campaign";
    case TOME_UI_CUSTOM_CAMPAIGN: return "custom campaign";
    case TOME_UI_LOAD_SAVED_GAME: return "load saved game";
    case TOME_UI_VIEW_REPLAY: return "view replay";
    case TOME_UI_LAN: return "LAN";
    case TOME_UI_BATTLE_NET: return "Battle.net";
    case TOME_UI_LOADING: return "loading";
    case TOME_UI_SCORE_SCREEN: return "score screen";
    }
    return "?";
}

// --- chat ------------------------------------------------------------------------------

void say(const std::string& text) {
    display_text("|cffffcc00Tome:|r " + text, 10.0f);
}

void tome_command(const std::string& args) {
    chat::TomeCommand cmd = chat::parse_tome(args);
    // /tome logs reads tome.log before this command's own line goes in, so
    // that line doesn't take one of the `count` shown. Enough of the file to
    // find `count` warnings among the lines around them.
    std::string tail;
    if (cmd.action == chat::TomeAction::Logs) tail = log::read_tail(256 * 1024);
    TOME_INFO("Chat command: /tome %s", args.c_str());
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
        say("/tome list | enable <id> | disable <id> | reload [id] | logs [warn|error] [count] | perf");
        return;
    case chat::TomeAction::Perf: {
        std::vector<profile::Named> mods;
        for (const ModStatus& s : mod_manager().status()) {
            if (s.running) mods.push_back({s.id, s.profile});
        }
        std::vector<std::string> lines = profile::perf_lines(std::move(mods), 8);
        if (lines.empty()) say("no running mod has run any code yet");
        for (const std::string& line : lines) say(line);
        return;
    }
    case chat::TomeAction::Logs: {
        std::vector<std::string> lines = log_view::format(tail, cmd.filter, cmd.count);
        if (lines.empty()) {
            say(cmd.filter == chat::LogFilter::All    ? "tome.log is empty or can't be read"
                : cmd.filter == chat::LogFilter::Warn ? "no recent warnings or errors in tome.log"
                                                      : "no recent errors in tome.log");
        }
        for (const std::string& line : lines) display_text(line, 20.0f);
        TOME_INFO("/tome logs: showed %zu lines", lines.size());
        return;
    }
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
        tome_command(cmd.args);  // logs the command itself
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

MapScriptHook g_map_script_hook = nullptr;
constexpr size_t kMaxMapScript = 64u << 20;

// Built-in map-script guard (docs/known-bugs.md): warns when the loading map's
// war3map.j declares a `local` that shadows a `global` of a different type --
// the local/global name-shadowing typecast ("memhack"/ACE) pattern. The
// compiler fix (memhack-guard) neutralizes it; this makes a downloaded virus
// map visible in the log regardless. Reads war3map.j from the mounted map
// archive, which is available by the loading screen.
void check_map_script() {
    g_script_shadows.clear();
    std::string script;
    std::vector<std::string_view> prelude;
    // Storm is otherwise only bound once a mod mounts assets; init is idempotent.
    // The game also accepts the script under scripts\ (protected maps use it).
    if (!tome::storm::init() ||
        ((!tome::storm::read_file("war3map.j", script, kMaxMapScript) || script.empty()) &&
         (!tome::storm::read_file("scripts\\war3map.j", script, kMaxMapScript) || script.empty()))) {
        if (g_map_script_hook) g_map_script_hook(nullptr, prelude);
        return;
    }
    // The map's script is compiled after these, so it can shadow their
    // globals too. Read through Storm, a map's own copies come first.
    std::string common, blizzard;
    if (tome::storm::read_file("Scripts\\common.j", common, kMaxMapScript)) prelude.push_back(common);
    if (tome::storm::read_file("Scripts\\Blizzard.j", blizzard, kMaxMapScript)) prelude.push_back(blizzard);
    if (g_map_script_hook) g_map_script_hook(&script, prelude);
    TOME_DEBUG("Map script guard: scanning war3map.j (%zu bytes, %zu prelude script(s))", script.size(),
               prelude.size());
    std::vector<tome::jass::ShadowFinding>& findings = g_script_shadows;
    if (!tome::jass::scan_shadow_exploit(script, findings, prelude)) return;
    TOME_WARN("Map script guard: %zu local/global type-shadow(s) in war3map.j "
              "(the JASS typecast/memhack pattern; see docs/known-bugs.md)",
              findings.size());
    for (size_t i = 0; i < findings.size() && i < 10; ++i) {
        TOME_WARN("  line %d: local '%s' (%s) shadows global of type %s",
                  findings[i].line, findings[i].name.c_str(),
                  findings[i].local_type.c_str(), findings[i].global_type.c_str());
    }
}

// The loading screen is the one point every way into a game passes (menus,
// -loadfile, LAN), after the natives are bound (CGameWar3::Reset) and before
// the map's files are read and its script runs. The script itself is already
// compiled: the game compiles it when the map is picked (twice with
// -loadfile), so the scan above only reports, and the memhack guard works in
// the compiler instead.
void map_load() {
    end_game();  // a game that never went back to a menu
    g_reset_depth = 0;  // in case a Reset was abandoned (a crash guard further out) before its AFTER ran
    g_map_name.clear();
    g_map_path.clear();
    g_map_description.clear();
    g_results.reset();
    read_map(current_game());
    g_map_loaded = true;
    g_game_started = false;
    g_game_time = 0;
    TOME_INFO("Map loading: %s (%s)", g_map_name.c_str(), g_map_path.c_str());
    check_map_script();
    // Before the map reads its other files, so "reload": "map" changes apply
    // to them (not to the scripts, compiled already).
    mod_manager().on_map_load();
    fire(TOME_EVENT_MAP_LOAD);
}

// --- "Screen ready" (developer builds, for the e2e test) ---------------------------
//
// A glue screen ignores clicks while it slides in. Its animation runs on UI
// timers (UITimer::Fire) aimed at objects inside the screen, one tick per
// frame; once they have started and then stopped for kReadyFrames frames,
// the screen takes input, and "Screen ready: <name>" is logged. A screen
// that doesn't slide in (the score screen) never logs it: wait for its
// "Screen: <name>" and then for a settled UI (debug_script's settle). Counted in
// frames, not time, so it holds on any machine. Only developer builds hook
// UITimer::Fire.

constexpr uint32_t kReadyFrames = 10;
constexpr uintptr_t kScreenSpan = 0x400;  // the animated parts sit at +0x168..+0x274

// SetScreen calls that made a new screen. SetScreen frees the old screen
// before it creates the new one, which may get the same address: a screen
// is its pointer and this count.
uint32_t g_screen_generation = 0;
bool g_screen_recreating = false;  // inside a SetScreen that makes a new screen

struct ScreenWatch {
    void* screen = nullptr;
    uint32_t generation = 0;
    // After SetScreen the old screen stays current while it slides out
    // (with the new screen's id): it's never the one that becomes ready.
    void* leaving = nullptr;
    uint32_t leaving_generation = 0;
    bool ticked = false;   // its animation has started
    uint32_t last_tick = 0;  // g_frames at its last tick
    bool ready = false;
};
ScreenWatch g_watch;

void* current_screen() {
    auto glue = static_cast<char*>(g_glue_get());
    return glue ? *reinterpret_cast<void**>(glue + g_build->game.glue_screen_offset) : nullptr;
}

std::string current_screen_name() {
    auto glue = static_cast<char*>(g_glue_get());
    if (!glue) return "?";
    uint32_t id = *reinterpret_cast<uint32_t*>(glue + g_build->game.glue_screen_id_offset);
    if (id == g_build->game.screen_main_menu) return "main menu";
    if (id == g_build->net.screen_game_chatroom) return "lobby";
    if (id == g_build->game.screen_lan_create) return "create LAN game";
    if (int32_t event = ui_event_for_screen(id)) return screen_name(event);
    return "screen " + std::to_string(id);
}

std::atomic<uint32_t> g_last_ui_tick{0};  // g_frames at the last UI timer of any kind
std::atomic<uint32_t> g_control_clicks{0};  // CControl::Click calls that reached a listener

// CControl::Click(this = the control, the button) returns at once for a
// control without a click registration (disabled, or one still sliding in):
// that click was dropped, and debug_script's until: has to repeat it.
void TOME_CALL before_control_click(tome_mod*, tome_hook_call* call, void*) {
    auto control = reinterpret_cast<void*>(static_cast<uintptr_t>(call->args[0]));
    if (frames::takes_clicks(control)) g_control_clicks.fetch_add(1, std::memory_order_relaxed);
}

void TOME_CALL before_ui_timer(tome_mod*, tome_hook_call* call, void*) {
    g_last_ui_tick.store(g_frames.load(std::memory_order_relaxed), std::memory_order_relaxed);
    auto timer = reinterpret_cast<const char*>(call->args[1]);
    auto target = reinterpret_cast<uintptr_t>(*reinterpret_cast<void* const*>(timer + 0x8));
    if (!g_watch.screen) return;
    auto screen = reinterpret_cast<uintptr_t>(g_watch.screen);
    if (target >= screen && target < screen + kScreenSpan) {
        g_watch.ticked = true;
        g_watch.last_tick = g_frames;
    }
}

// Each frame (PresentFrame), on the main thread.
void watch_screen() {
    void* screen = current_screen();
    bool leaving = screen && screen == g_watch.leaving && g_screen_generation == g_watch.leaving_generation;
    if (screen != g_watch.screen || g_screen_generation != g_watch.generation) {
        ScreenWatch next;
        next.screen = screen;
        next.generation = g_screen_generation;
        if (leaving) {
            next.leaving = screen;
            next.leaving_generation = g_screen_generation;
        }
        g_watch = next;
        return;
    }
    if (screen && !leaving && g_watch.ticked && !g_watch.ready &&
        g_frames - g_watch.last_tick >= kReadyFrames) {
        g_watch.ready = true;
        TOME_INFO("Screen ready: %s", current_screen_name().c_str());
    }
}

// The screen current before the change: when SetScreen returns, it may still
// be (sliding out), or the new one may be current already (the first main menu).
// SetScreen(screen, force) makes a new screen unless force is 0 and the
// current id is screen already; then the current screen stays (and stays
// "leaving"). A new screen bumps g_screen_generation, so it isn't taken for
// the leaving one even at the same address.
void TOME_CALL before_set_screen(tome_mod*, tome_hook_call* call, void*) {
    auto glue = static_cast<char*>(g_glue_get());
    uint32_t id = glue ? *reinterpret_cast<uint32_t*>(glue + g_build->game.glue_screen_id_offset) : 0;
    g_screen_recreating = !glue || call->args[1] != 0 || call->args[0] != id;
    g_watch.leaving = current_screen();
    g_watch.leaving_generation = g_screen_generation;
}

void TOME_CALL after_set_screen(tome_mod*, tome_hook_call* call, void*) {
    if (g_screen_recreating) {
        g_screen_recreating = false;
        ++g_screen_generation;
    }
    int32_t event = ui_event_for_screen(call->args[0]);
    if (event) TOME_INFO("Screen: %s", screen_name(event));
    if (event == TOME_UI_SCORE_SCREEN) end_game();  // the game is over once its score screen shows
    if (event == TOME_UI_LOADING) map_load();
    if (!event) return;
    auto glue = static_cast<char*>(g_glue_get());
    void* screen = glue ? *reinterpret_cast<void**>(glue + g_build->game.glue_screen_offset) : nullptr;
    if (screen) frames::on_screen(event, screen);
}

// Reset runs when a game is left, but also when a lobby is created or left
// and while a saved game loads, so it only ends a game that started. It
// removes every unit left (CUnit::Remove): that's teardown, not
// UNIT_REMOVED, even when no game started (a load that was abandoned after
// creating units leaves g_map_loaded set), hence g_reset_depth.
void TOME_CALL before_reset(tome_mod*, tome_hook_call*, void*) {
    ++g_reset_depth;
    end_game();
}

void TOME_CALL after_reset(tome_mod*, tome_hook_call*, void*) {
    if (g_reset_depth > 0) --g_reset_depth;
}

void TOME_CALL after_enter_game(tome_mod*, tome_hook_call*, void*) {
    if (!g_map_loaded || g_game_started) return;
    g_game_started = true;
    ++g_game_serial;
    g_game_time = 0;
    g_selection = units::read_selection();
    g_paused = false;
    g_resources.reset();
    g_time_of_day = -1;
    read_map(current_game());  // a saved game's map info is only complete now
    TOME_INFO("Game started: %s", g_map_name.c_str());
    crash_loop::on_game_start();
    if (!g_script_shadows.empty()) {
        // The player didn't choose to trust this map's script; say so where
        // they'll see it, not only in tome.log. The guard has blocked it.
        bool shown = display_text("|cffffcc00Tome:|r This map's script gives " +
                                      std::to_string(g_script_shadows.size()) +
                                      " local variable(s) a global's name and another type, the trick memhack "
                                      "maps use to read and write game memory. Tome blocks it; details in tome.log.",
                                  20.0f);
        if (shown) TOME_INFO("Map script guard: told the player");
    }
    if (void* game_ui = g_game_ui_get(0, 0)) frames::on_screen(TOME_UI_GAME, game_ui);
    handshake::on_game_start();
    timers_api::on_game_start();
    desync_api::on_game_start();
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
    g_frames.fetch_add(1, std::memory_order_relaxed);
    if (TOME_DEV && g_glue_get) watch_screen();
    if (g_tick_handlers.load(std::memory_order_relaxed) > 0) fire(TOME_EVENT_TICK, delta);
    timers_api::on_frame(delta);
    if (g_game_started) check_selection();
    popups::on_frame();
}

// Resources change in many places (mining, spending, bounties, upkeep,
// triggers), so they're compared every game tick while a mod listens: 16
// players, 4 values. Without listeners nothing is read, and the next first
// reading only sets the baseline.
void check_resources() {
    if (!has_handlers(TOME_EVENT_PLAYER_RESOURCE)) {
        g_resources.reset();
        return;
    }
    for (int32_t slot = 0; slot < units::ResourceWatch::kSlots; ++slot) {
        for (int32_t resource : units::kWatchedResources) {
            int32_t value, previous;
            if (!units::player_resource(slot, resource, value)) continue;
            if (!g_resources.update(slot, resource, value, previous)) continue;
            std::string name = units::player_name(slot);
            OnUnwind release_name([&] { std::string().swap(name); });
            tome_event_info info = make_info(TOME_EVENT_PLAYER_RESOURCE, 0);
            info.player = slot;
            info.player_name = name.c_str();
            info.resource = resource;
            info.amount = value;
            info.previous = previous;
            fire_info(info);
        }
    }
}

void check_time_of_day() {
    if (!has_handlers(TOME_EVENT_DAY) && !has_handlers(TOME_EVENT_NIGHT)) {
        g_time_of_day = -1;
        return;
    }
    double now = units::time_of_day();
    int32_t change = units::day_change(g_time_of_day, now);
    g_time_of_day = now;
    if (!change) return;
    tome_event_info info = make_info(change > 0 ? TOME_EVENT_DAY : TOME_EVENT_NIGHT, 0);
    info.amount = now;
    fire_info(info);
}

void TOME_CALL before_game_tick(tome_mod*, tome_hook_call* call, void*) {
    if (!g_game_started) return;
    float step;
    std::memcpy(&step,
                reinterpret_cast<const char*>(static_cast<uintptr_t>(call->args[0])) +
                    g_build->game.game_tick_step_offset,
                sizeof(step));
    // Summed as integers: a float sum would round by the FPU's precision
    // setting, which can differ between players.
    const game_clock::Micros before = g_game_time;
    g_game_time += game_clock::from_seconds(step);
    handshake::on_game_tick(game_clock::to_seconds(g_game_time));
    if (g_game_tick_handlers.load(std::memory_order_relaxed) > 0) fire(TOME_EVENT_GAME_TICK, step);
    timers_api::on_game_step(g_game_time);
    desync_api::on_game_step(before, g_game_time);
    check_resources();
    check_time_of_day();
}

// After the main loop, before the game tears its systems down. The game was
// normally left already (Reset runs on the way out); this catches an exit
// that skipped it. Then the mods get disabled and unloaded.
void TOME_CALL before_shutdown(tome_mod*, tome_hook_call*, void*) {
    end_game();
    mod_manager().shutdown();
    crash_loop::on_shutdown();
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
enum class Special { None, HeroLevel, PreviousOwner, ThisPlayer };

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

void set_item(tome_event_info& info, void* item) {
    info.item = item;
    info.item_handle = units::handle_of(item);
    set_object(info, units::item_type(item));
}

void set_destructable(tome_event_info& info, void* destructable) {
    info.destructable = destructable;
    info.destructable_handle = units::handle_of(destructable);
    set_object(info, units::destructable_type(destructable));
}

// A player event's player (this) by name; `player_object` is a CPlayerWar3.
void set_player(tome_event_info& info, std::string& name, const void* player_object) {
    info.player = units::player_slot(player_object);
    name = units::player_name(info.player);
    info.player_name = name.c_str();
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
    if (spec.item >= 0) set_item(info, arg_ptr(call, spec.item));
    if (spec.type >= 0) set_object(info, call->args[spec.type]);
    if (spec.level >= 0) info.level = static_cast<int32_t>(call->args[spec.level]);
    if (spec.special == Special::HeroLevel) info.level = units::hero_level(unit_object);
    if (spec.special == Special::PreviousOwner) info.player = static_cast<int32_t>(call->args[2] & 0xFFFF);
    std::string name;
    OnUnwind release_name([&] { std::string().swap(name); });
    if (spec.special == Special::ThisPlayer) set_player(info, name, arg_ptr(call, 0));
    fire_info(info);
}

// A target widget: a unit as other (described into `unit`), an item or a
// destructable.
void set_target(tome_event_info& info, tome_unit& unit, const units::Widget& target) {
    if (target.unit) {
        units::describe_unit(target.unit, unit);
        info.other = &unit;
    }
    if (target.item) set_item(info, target.item);
    if (target.destructable) set_destructable(info, target.destructable);
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
    tome_event_info info = make_info(TOME_EVENT_UNIT_ORDER, 0);
    info.unit = &unit;
    set_target(info, target, o.target);
    info.order_id = o.id;
    info.order_type = o.type;
    info.x = o.x;
    info.y = o.y;
    fire_info(info);
}

// CUnit::OnSpell* (this = the caster, args[1] = the CAbility), before they
// check whether a trigger listens. user is the tome_event.
void TOME_CALL before_spell(tome_mod*, tome_hook_call* call, void* user) {
    auto event = static_cast<int32_t>(reinterpret_cast<intptr_t>(user));
    if (!g_map_loaded || !has_handlers(event)) return;
    void* unit_object = arg_ptr(call, 0);
    if (!unit_object) return;
    units::Spell spell = units::read_spell(arg_ptr(call, 1));
    tome_unit unit, target;
    units::describe_unit(unit_object, unit);
    tome_event_info info = make_info(event, 0);
    info.unit = &unit;
    set_target(info, target, spell.target);
    set_object(info, spell.ability_id);  // after the target: an item target sets it too
    info.x = spell.x;
    info.y = spell.y;
    fire_info(info);
}

// CUnit::OnDamaged(this = the target, float* damage, CUnit* source).
void TOME_CALL before_damaged(tome_mod*, tome_hook_call* call, void*) {
    if (!g_map_loaded || !has_handlers(TOME_EVENT_UNIT_DAMAGED)) return;
    void* unit_object = arg_ptr(call, 0);
    auto damage = static_cast<const float*>(arg_ptr(call, 1));
    void* source_object = arg_ptr(call, 2);
    if (!unit_object || !damage) return;
    tome_unit unit, source;
    units::describe_unit(unit_object, unit);
    if (source_object) units::describe_unit(source_object, source);
    tome_event_info info = make_info(TOME_EVENT_UNIT_DAMAGED, 0);
    info.unit = &unit;
    info.other = source_object ? &source : nullptr;
    info.amount = *damage;
    fire_info(info);
}

// CUnit::OnAcquireTarget(this = the unit, CWidget* target).
void TOME_CALL before_acquire(tome_mod*, tome_hook_call* call, void*) {
    if (!g_map_loaded || !has_handlers(TOME_EVENT_UNIT_ACQUIRED_TARGET)) return;
    void* unit_object = arg_ptr(call, 0);
    if (!unit_object) return;
    tome_unit unit, target;
    units::describe_unit(unit_object, unit);
    tome_event_info info = make_info(TOME_EVENT_UNIT_ACQUIRED_TARGET, 0);
    info.unit = &unit;
    set_target(info, target, units::classify_widget(arg_ptr(call, 1)));
    fire_info(info);
}

// CPlayerWar3::OnEndCinematic (this = the player who pressed Esc).
void TOME_CALL before_end_cinematic(tome_mod*, tome_hook_call* call, void*) {
    if (!g_map_loaded || !has_handlers(TOME_EVENT_ESC_PRESSED)) return;
    std::string name;
    OnUnwind release_name([&] { std::string().swap(name); });
    tome_event_info info = make_info(TOME_EVENT_ESC_PRESSED, 0);
    set_player(info, name, arg_ptr(call, 0));
    fire_info(info);
}

// CPlayerWar3::OnArrowKey(this = the player, the JASS event id + 0x80200).
void TOME_CALL before_arrow_key(tome_mod*, tome_hook_call* call, void*) {
    if (!g_map_loaded || !has_handlers(TOME_EVENT_ARROW_KEY)) return;
    tome_event_info info = make_info(TOME_EVENT_ARROW_KEY, 0);
    if (!units::arrow_key(call->args[1], info.key, info.pressed)) return;
    std::string name;
    OnUnwind release_name([&] { std::string().swap(name); });
    set_player(info, name, arg_ptr(call, 0));
    fire_info(info);
}

// CWidget::OnDeath (this = any widget). Units have TOME_EVENT_UNIT_DEATH,
// with the killer, from their own function.
void TOME_CALL before_widget_death(tome_mod*, tome_hook_call* call, void*) {
    if (!g_map_loaded) return;
    units::Widget widget = units::classify_widget(arg_ptr(call, 0));
    int32_t event = widget.item ? TOME_EVENT_ITEM_DEATH : widget.destructable ? TOME_EVENT_DESTRUCTABLE_DEATH : 0;
    if (!event || !has_handlers(event)) return;
    tome_event_info info = make_info(event, 0);
    if (widget.item) set_item(info, widget.item);
    if (widget.destructable) set_destructable(info, widget.destructable);
    fire_info(info);
}

// CScriptDialog::OnClick(this = the dialog, the button, the player's slot).
void TOME_CALL before_dialog_click(tome_mod*, tome_hook_call* call, void*) {
    if (!g_map_loaded || !has_handlers(TOME_EVENT_DIALOG_CLICK)) return;
    tome_event_info info = make_info(TOME_EVENT_DIALOG_CLICK, 0);
    info.dialog = units::handle_of(arg_ptr(call, 0));
    info.button = units::handle_of(arg_ptr(call, 1));
    info.player = static_cast<int32_t>(call->args[2] & 0xFF);
    std::string name = units::player_name(info.player);
    OnUnwind release_name([&] { std::string().swap(name); });
    info.player_name = name.c_str();
    fire_info(info);
}

// The network action handlers for the command card's skill and build menus
// (args[0] = the action, its sender's slot at CNetEvent.sender). user is the
// tome_event.
void TOME_CALL before_menu_action(tome_mod*, tome_hook_call* call, void* user) {
    auto event = static_cast<int32_t>(reinterpret_cast<intptr_t>(user));
    const char* action = static_cast<const char*>(arg_ptr(call, 0));
    if (!g_game_started || !action || !has_handlers(event)) return;
    tome_event_info info = make_info(event, 0);
    info.player = static_cast<uint8_t>(action[g_build->play.net_event_sender_offset]);
    std::string name = units::player_name(info.player);
    OnUnwind release_name([&] { std::string().swap(name); });
    info.player_name = name.c_str();
    fire_info(info);
}

void TOME_CALL before_end_level(tome_mod*, tome_hook_call*, void*) {
    if (g_game_started) fire(TOME_EVENT_LEVEL_END);
}

void fire_unit(int32_t event, const void* unit_object) {
    if (!g_map_loaded || g_reset_depth > 0 || !unit_object || !has_handlers(event)) return;
    tome_unit unit;
    units::describe_unit(unit_object, unit);
    tome_event_info info = make_info(event, 0);
    info.unit = &unit;
    fire_info(info);
}

// CreateUnitObject returns the new unit.
void TOME_CALL after_create_unit(tome_mod*, tome_hook_call* call, void*) {
    fire_unit(TOME_EVENT_UNIT_CREATED, reinterpret_cast<const void*>(static_cast<uintptr_t>(call->result)));
}

void TOME_CALL before_remove_unit(tome_mod*, tome_hook_call* call, void*) {
    fire_unit(TOME_EVENT_UNIT_REMOVED, arg_ptr(call, 0));
}

// CAbilityHero::AddXP (this = the hero ability): the experience before the
// call, kept until the AFTER handler compares. Nothing is added at the
// maximum level, and the amount may be scaled, so the difference is what
// the hero gained. A stack, in case one call leads to another (the game
// simulation runs on the main thread).
int32_t g_xp_before[8];
size_t g_xp_depth = 0;

void TOME_CALL before_add_xp(tome_mod*, tome_hook_call* call, void*) {
    if (g_xp_depth < std::size(g_xp_before)) g_xp_before[g_xp_depth] = units::hero_xp(arg_ptr(call, 0));
    ++g_xp_depth;
}

void TOME_CALL after_add_xp(tome_mod*, tome_hook_call* call, void*) {
    if (g_xp_depth == 0) return;
    --g_xp_depth;
    if (g_xp_depth >= std::size(g_xp_before)) return;
    int32_t before = g_xp_before[g_xp_depth];
    const void* ability = arg_ptr(call, 0);
    void* hero = units::ability_owner(ability);
    int32_t gained = units::hero_xp(ability) - before;
    if (!g_map_loaded || !hero || gained <= 0 || !has_handlers(TOME_EVENT_HERO_XP)) return;
    tome_unit unit;
    units::describe_unit(hero, unit);
    tome_event_info info = make_info(TOME_EVENT_HERO_XP, 0);
    info.unit = &unit;
    info.amount = gained;
    info.level = units::hero_level(hero);
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

void* event_user(int32_t event) {
    return reinterpret_cast<void*>(static_cast<intptr_t>(event));
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
    {"CPlayerWar3::OnUnitDecay", {.event = TOME_EVENT_UNIT_DECAY}},
    // this is the detecting player, not the unit's owner.
    {"CPlayerWar3::OnUnitDetected", {.event = TOME_EVENT_UNIT_DETECTED, .special = Special::ThisPlayer}},
    {"CPlayerWar3::OnUnitHidden", {.event = TOME_EVENT_UNIT_HIDDEN, .special = Special::ThisPlayer}},
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

int32_t TOME_CALL api_map_script_shadows(tome_script_shadow* out, uint32_t item_size, int32_t max) {
    if (max < 0 || (max > 0 && (!out || item_size == 0))) return 0;
    if (!frames::on_main_thread()) {
        TOME_ERROR("map_script_shadows called off the game's main thread (thread %lu); ignored",
                   GetCurrentThreadId());
        return 0;
    }
    // A mod built against a later tome.h may pass a larger struct.
    size_t copy = std::min<size_t>(item_size, sizeof(tome_script_shadow));
    for (int32_t i = 0; i < max && i < static_cast<int32_t>(g_script_shadows.size()); ++i) {
        const auto& f = g_script_shadows[i];
        tome_script_shadow shadow = {};
        shadow.size = static_cast<uint32_t>(copy);
        shadow.name = f.name.c_str();
        shadow.local_type = f.local_type.c_str();
        shadow.global_type = f.global_type.c_str();
        shadow.line = f.line;
        auto slot = reinterpret_cast<char*>(out) + static_cast<size_t>(i) * item_size;
        std::memset(slot, 0, item_size);
        std::memcpy(slot, &shadow, copy);
    }
    return static_cast<int32_t>(g_script_shadows.size());
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
    g_game_ui_get = game_ui_get(game_base, build);
    g_display_text = reinterpret_cast<DisplayTextFn>(game_base + build.game.game_ui_display_text);
    g_string_text = reinterpret_cast<StringTextFn>(game_base + build.jass.string_get_text);
    units::init(game_base, build);
    QueryPerformanceFrequency(&g_frequency);
    for (auto& list : g_handlers) list = std::make_shared<RegList>();

    hooks::init(game_base, build.data);
    std::vector<HookSpec> specs = {
        {"CGlueMgr::SetScreen", TOME_HOOK_AFTER, after_set_screen},
        {"CGameWar3::Reset", TOME_HOOK_BEFORE, before_reset},
        {"CGameWar3::Reset", TOME_HOOK_AFTER, after_reset},
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
        {"CUnit::OnSpellChannel", TOME_HOOK_BEFORE, before_spell, event_user(TOME_EVENT_SPELL_CHANNEL)},
        {"CUnit::OnSpellCast", TOME_HOOK_BEFORE, before_spell, event_user(TOME_EVENT_SPELL_CAST)},
        {"CUnit::OnSpellEffect", TOME_HOOK_BEFORE, before_spell, event_user(TOME_EVENT_SPELL_EFFECT)},
        {"CUnit::OnSpellFinish", TOME_HOOK_BEFORE, before_spell, event_user(TOME_EVENT_SPELL_FINISH)},
        {"CUnit::OnSpellEndCast", TOME_HOOK_BEFORE, before_spell, event_user(TOME_EVENT_SPELL_END_CAST)},
        {"CUnit::OnDamaged", TOME_HOOK_BEFORE, before_damaged},
        {"CUnit::OnAcquireTarget", TOME_HOOK_BEFORE, before_acquire},
        {"CPlayerWar3::OnEndCinematic", TOME_HOOK_BEFORE, before_end_cinematic},
        {"CPlayerWar3::OnArrowKey", TOME_HOOK_BEFORE, before_arrow_key},
        {"CWidget::OnDeath", TOME_HOOK_BEFORE, before_widget_death},
        {"CScriptDialog::OnClick", TOME_HOOK_BEFORE, before_dialog_click},
        {"OnNetShowSkillMenu", TOME_HOOK_BEFORE, before_menu_action, event_user(TOME_EVENT_SKILL_MENU)},
        {"OnNetShowBuildMenu", TOME_HOOK_BEFORE, before_menu_action, event_user(TOME_EVENT_BUILD_MENU)},
        {"HandleManager::FireEndLevel", TOME_HOOK_BEFORE, before_end_level},
        {"CreateUnitObject", TOME_HOOK_AFTER, after_create_unit},
        {"CUnit::Remove", TOME_HOOK_BEFORE, before_remove_unit},
        {"CAbilityHero::AddXP", TOME_HOOK_BEFORE, before_add_xp},
        {"CAbilityHero::AddXP", TOME_HOOK_AFTER, after_add_xp},
    };
    if (TOME_DEV) {
        specs.push_back({"UITimer::Fire", TOME_HOOK_BEFORE, before_ui_timer});
        specs.push_back({"CGlueMgr::SetScreen", TOME_HOOK_BEFORE, before_set_screen});
        specs.push_back({"CControl::Click", TOME_HOOK_BEFORE, before_control_click});
    }
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
             "spells, damage, experience, players, resources, keys, menus, dialogs, levels and saved games";
    return true;
}

uint32_t frames() {
    return g_frames.load(std::memory_order_relaxed);
}

uint32_t control_clicks() {
    return g_control_clicks.load(std::memory_order_relaxed);
}

uint32_t frames_since_ui_tick() {
    return frames() - g_last_ui_tick.load(std::memory_order_relaxed);
}

void fill_api(tome_api& api) {
    api.on_event = api_on_event;
    api.register_command = api_register_command;
    api.display_text = api_display_text;
    api.map_script_shadows = api_map_script_shadows;
}

void dispatch_for_test(tome_event_info info) {
    if (info.size == 0) info = make_info(info.event, 0);
    for (const char** text : {&info.map_name, &info.map_path, &info.map_description, &info.player_name, &info.text,
                              &info.object_name, &info.save_name}) {
        if (!*text) *text = "";
    }
    // Timers follow the test's frames and steps, as they follow the game's.
    static game_clock::Micros test_game_time = 0;
    if (info.event == TOME_EVENT_GAME_START) {
        timers_api::on_game_start();
        test_game_time = 0;
    }
    fire_info(info);
    if (info.event == TOME_EVENT_TICK) timers_api::on_frame(info.delta);
    if (info.event == TOME_EVENT_GAME_TICK) {
        test_game_time += game_clock::from_seconds(info.delta);
        timers_api::on_game_step(test_game_time);
    }
    if (info.event == TOME_EVENT_GAME_END) timers_api::on_game_end();
}

void fire_mods_changed() {
    fire(TOME_EVENT_MODS_CHANGED);
}

tome_event_info default_info_for_test(int32_t event) {
    return make_info(event, 0);
}

void set_map_script_hook(MapScriptHook hook) {
    g_map_script_hook = hook;
}

bool display_text(const std::string& text, float seconds) {
    if (!g_ready || !g_game_started) return false;
    void* game_ui = g_game_ui_get(0, 0);
    if (!game_ui) return false;
    g_display_text(game_ui, 0.0f, 0.0f, text.c_str(), seconds, 0xFFFFFFFF);
    return true;
}

} // namespace tome::events
