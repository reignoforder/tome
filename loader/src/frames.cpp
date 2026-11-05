// SPDX-License-Identifier: GPL-3.0-or-later
#include "frames.h"

#include "core/frames.h"
#include "core/path_guard.h"
#include "core/rtti.h"
#include "game_build.h"
#include "guard.h"
#include "log.h"
#include "mod_manager.h"
#include "natives.h"
#include "overlay.h"
#include "paths.h"
#include "popups.h"
#include "storm.h"
#include "unwind.h"

#include <windows.h>

#include <cstring>
#include <functional>
#include <map>
#include <utility>
#include <vector>

// GCC warns that __thiscall is meant for class methods, but it does apply it
// to free functions and function pointer types (see mod_menu.cpp).
#pragma GCC diagnostic ignored "-Wattributes"

namespace tome::frames {
namespace {

using LoadTocFilesFn = int(__fastcall*)(const char* toc, void* table_a, void* table_b, void* status);
using GetFrameByNameFn = void*(__fastcall*)(const char* name, int context);
// The first stack argument is unused in 1.27b; the second goes to the
// frame's constructor (priority) and the third to its name registration.
using CreateFrameFn = void*(__fastcall*)(const char* name, void* parent, int unused, int priority, int context);
using SetPointFn = void(__thiscall*)(void* layout, int point, void* relative, int relative_point, float x, float y,
                                     int update);
using SpriteInstanceFn = void*(__thiscall*)(void* sprite);
using SetAnimationFn = void(__fastcall*)(void* instance, const int* tags, int count, int flags);
using SetTextFn = void(__thiscall*)(void* text_frame, const char* text);
using FrameMethodFn = void(__thiscall*)(void* frame);
using StringTextFn = const char*(__thiscall*)(const void* jass_string);
using SetListenerFn = void(__thiscall*)(void* control, int command, void* listener, int enable);
using SetFocusFn = void(__thiscall*)(void* frame, int focus, int, void* other);
using SetWidthFn = void(__thiscall*)(void* layout, float width);
using ControlClickFn = void(__thiscall*)(void* control, int button);
using HasEventFn = int(__thiscall*)(void* object, uint32_t event);
using EditGetTextFn = const char*(__thiscall*)(void* edit_box);
using EditSetTextFn = void(__thiscall*)(void* edit_box, const char* text, int notify);
using GlueGetFn = void*(*)();

bool g_ready = false;
uint32_t g_main_thread = 0;
uintptr_t g_image_begin = 0, g_image_end = 0;
const BuildMappings* g_build = nullptr;

LoadTocFilesFn g_load_toc_files = nullptr;
void* g_fdf_table_a = nullptr;
void* g_fdf_table_b = nullptr;
void* g_fdf_status = nullptr;
GetFrameByNameFn g_get_frame = nullptr;
CreateFrameFn g_create_frame = nullptr;
SetPointFn g_set_point = nullptr;
SpriteInstanceFn g_sprite_instance = nullptr;
SetAnimationFn g_set_animation = nullptr;
SetTextFn g_set_text = nullptr;
StringTextFn g_string_text = nullptr;
SetListenerFn g_set_listener = nullptr;
SetFocusFn g_set_focus = nullptr;
SetWidthFn g_set_width = nullptr;
ControlClickFn g_control_click = nullptr;
HasEventFn g_has_event = nullptr;
uint32_t g_click_event = 0;
EditGetTextFn g_edit_get_text = nullptr;
EditSetTextFn g_edit_set_text = nullptr;
void** g_focused_frame = nullptr;
GlueGetFn g_glue_get = nullptr;
GameUiGetFn g_game_ui_get = nullptr;

// Frames handed out through the API or to JASS, with their names (main thread).
ui::FrameTable g_table;

// --- classes -----------------------------------------------------------------

bool read_image(const void*, uint32_t address, void* out, uint32_t size) {
    if (address < g_image_begin || address + size > g_image_end || address + size < address) return false;
    std::memcpy(out, reinterpret_cast<const void*>(static_cast<uintptr_t>(address)), size);
    return true;
}

std::map<std::pair<uintptr_t, std::string>, bool> g_class_cache;

// Whether a frame's class is `name` or derives from it. Frames are game
// objects, so their vtables live in game.dll's image; anything else is no frame.
bool is_a(void* frame, const char* name) {
    if (!frame) return false;
    uintptr_t vtable = *static_cast<uintptr_t*>(frame);
    auto key = std::make_pair(vtable, std::string(name));
    auto it = g_class_cache.find(key);
    if (it != g_class_cache.end()) return it->second;
    rtti::Memory memory{read_image, nullptr};
    bool result = rtti::derives_from(memory, static_cast<uint32_t>(vtable), name);
    g_class_cache[key] = result;
    return result;
}

std::string class_of(void* frame) {
    if (!frame) return "null";
    rtti::Memory memory{read_image, nullptr};
    std::string name = rtti::class_name(memory, static_cast<uint32_t>(*static_cast<uintptr_t*>(frame)));
    return name.empty() ? "unknown" : name;
}

void call_frame_method(void* frame, uint32_t offset) {
    auto vtable = *reinterpret_cast<uintptr_t**>(frame);
    reinterpret_cast<FrameMethodFn>(vtable[offset / sizeof(uintptr_t)])(frame);
}

// --- screens, UI callbacks and clicks ------------------------------------------

void* g_menu = nullptr;       // the CMainMenu from the last Setup
void* g_menu_root = nullptr;  // its "MainMenuFrame", to tell whether it still exists
unsigned g_menu_map_loads = 0;  // natives::map_load_count() at the last Setup

bool main_menu_live() {
    return g_menu && g_menu_root && get("MainMenuFrame") == g_menu_root;
}

constexpr int kUiEventCount = TOME_UI_GAME;

// The latest instance of each screen (by tome_ui_event). Main thread only.
struct Screen {
    void* root = nullptr;
    unsigned generation = 0;  // counts the times it was built
};
Screen g_screens[kUiEventCount + 1];

bool valid_ui_event(int32_t event) {
    return event >= TOME_UI_MAIN_MENU && event <= kUiEventCount;
}

// Whether the screen from the last on_screen() still exists.
bool screen_live(int32_t event) {
    if (event == TOME_UI_MAIN_MENU) return main_menu_live();
    void* root = g_screens[event].root;
    if (!root) return false;
    if (event == TOME_UI_GAME) return g_game_ui_get(0, 0) == root;
    auto glue = static_cast<char*>(g_glue_get());
    return glue && *reinterpret_cast<void**>(glue + g_build->game.glue_screen_offset) == root;
}

struct UiHook {
    uint64_t token;
    tome_mod* mod;
    int32_t event;
    tome_ui_fn fn;
    void* user;
    unsigned called_generation;  // the screen generation it last ran for
};

SRWLOCK g_hooks_lock = SRWLOCK_INIT;  // on_ui may be called off the main thread
std::vector<UiHook> g_hooks;
uint64_t g_next_token = 1;

const char* ui_event_name(int32_t event) {
    static const char* const names[] = {
        nullptr, "(main menu)", "(single player)", "(custom game)", "(options)", "(campaign)",
        "(custom campaign)", "(load saved game)", "(view replay)", "(LAN)", "(Battle.net)",
        "(loading screen)", "(score screen)", "(game)",
    };
    return valid_ui_event(event) ? names[event] : nullptr;
}

// Runs every callback for `event` that hasn't run for the current instance
// of that screen yet. Main thread only. The list is copied, so callbacks may
// register more.
void run_ui_hooks(int32_t event) {
    if (!screen_live(event)) return;
    const unsigned generation = g_screens[event].generation;
    std::vector<UiHook> due;
    {
        ExclusiveLock hold(g_hooks_lock);
        for (UiHook& hook : g_hooks) {
            if (hook.event == event && hook.called_generation != generation) {
                hook.called_generation = generation;
                due.push_back(hook);
            }
        }
    }
    OnUnwind release_due([&] { std::vector<UiHook>().swap(due); });
    for (const UiHook& hook : due) {
        // Skip hooks revoked by an earlier callback in this round.
        bool still_registered = false;
        {
            SharedLock hold(g_hooks_lock);
            for (const UiHook& h : g_hooks) still_registered |= h.token == hook.token;
        }
        if (still_registered) {
            guard::run(hook.mod, "UI callback", ui_event_name(hook.event),
                       [&] { hook.fn(hook.mod, hook.event, hook.user); });
        }
    }
}

// --- clicks --------------------------------------------------------------------
// Controls report clicks to CObservers registered with CControl::SetListener,
// passing the registration's command id. Tome registers one observer of its
// own, a CObserver with OnEvent replaced, for every button a mod asks for,
// so clicks work on any screen without hooking each screen's event handler.

struct Click {
    uint64_t token;
    tome_mod* mod;
    tome_click_fn fn;
    void* user;
    void* button;
    std::function<void()> tome_fn;  // set for Tome's own buttons instead of mod and fn
};

// By command id. Main thread, except that a mod's revoker can run on the
// watcher thread when there's no game window (main_thread::post runs inline).
std::map<int, Click> g_clicks;
SRWLOCK g_clicks_lock = SRWLOCK_INIT;
int g_next_command = 1;
uint64_t g_next_click_token = 1;

// A CObserver: vtable, reference count, registration list. The count starts
// high and the object is never freed, so the game's references to it never
// drop it to zero.
struct Observer {
    const uintptr_t* vtable;
    volatile LONG refs;
    void* registrations;
};
constexpr size_t kObserverSlots = 6;
uintptr_t g_observer_vtable[1 + kObserverSlots];  // [-1]: the RTTI locator, as in the game's
Observer g_observer = {nullptr, 0x10000000, nullptr};

bool frame_alive(void* frame) {
    const ui::FrameRecord* record = g_table.by_frame(reinterpret_cast<uintptr_t>(frame));
    return record && get(record->name.c_str(), record->context) == frame;
}

int __thiscall observer_on_event(Observer*, void* event) {
    int command = *reinterpret_cast<int*>(static_cast<char*>(event) + 8);
    Click click;
    {
        SharedLock hold(g_clicks_lock);
        auto it = g_clicks.find(command);
        if (it == g_clicks.end() || !frame_alive(it->second.button)) return 0;
        click = it->second;
    }
    if (click.tome_fn) {
        click.tome_fn();
    } else {
        guard::run(click.mod, "click callback", nullptr,
                   [&] { click.fn(click.mod, static_cast<tome_frame*>(click.button), click.user); });
    }
    // A clicked button keeps the keyboard focus, and in a game it would take
    // the next Enter instead of the chat box. Hand the keyboard back.
    if (screen_live(TOME_UI_GAME) && *g_focused_frame == click.button) g_set_focus(click.button, 0, 0, nullptr);
    return 1;
}

void init_observer(uintptr_t game_base) {
    const auto* game_vtable = reinterpret_cast<const uintptr_t*>(game_base + g_build->ui.observer_vtable);
    std::memcpy(g_observer_vtable, game_vtable - 1, sizeof(g_observer_vtable));
    g_observer_vtable[1 + g_build->ui.observer_event_offset / sizeof(uintptr_t)] =
        reinterpret_cast<uintptr_t>(&observer_on_event);
    g_observer.vtable = g_observer_vtable + 1;
}

// --- mod FDF files -------------------------------------------------------------

std::map<std::string, std::string> g_loaded_fdf;  // "<mod>\n<path>" -> contents; main thread only

// --- API functions ---------------------------------------------------------------

const char* mod_name(tome_mod* mod) {
    return mod ? mod->id.c_str() : "?";
}

// TOME_OK if the game's frame functions may be called now.
tome_result check_main_thread(const char* what) {
    if (!g_ready) return TOME_ERR_UNAVAILABLE;
    if (!on_main_thread()) {
        TOME_ERROR("%s called off the game's main thread (thread %lu); ignored", what, GetCurrentThreadId());
        return TOME_ERR_WRONG_THREAD;
    }
    return TOME_OK;
}

tome_result TOME_CALL api_on_ui(tome_mod* mod, int32_t event, tome_ui_fn fn, void* user) {
    if (!mod || !fn || !valid_ui_event(event)) return TOME_ERR_INVALID_ARGUMENT;
    if (!g_ready) return TOME_ERR_UNAVAILABLE;
    uint64_t token;
    {
        ExclusiveLock hold(g_hooks_lock);
        token = g_next_token++;
        g_hooks.push_back({token, mod, event, fn, user, 0});
    }
    mod->add_revoker([token] {
        ExclusiveLock hold(g_hooks_lock);
        for (auto it = g_hooks.begin(); it != g_hooks.end(); ++it) {
            if (it->token == token) {
                g_hooks.erase(it);
                break;
            }
        }
    });
    // Off the main thread (the first mod load), catch_up() runs it later.
    if (on_main_thread()) run_ui_hooks(event);
    return TOME_OK;
}

tome_result TOME_CALL api_load_fdf(tome_mod* mod, const char* path) {
    if (!mod || !path || !ui::is_valid_mod_fdf_path(path)) {
        TOME_ERROR("[%s] load_fdf(%s): needs a relative .fdf path inside the mod's folder", mod_name(mod),
                   path ? path : "null");
        return TOME_ERR_INVALID_ARGUMENT;
    }
    if (tome_result r = check_main_thread("load_fdf"); r != TOME_OK) return r;

    std::string contents;
    if (!read_file(mod->dir_w + L"\\" + widen(path), contents)) {
        TOME_ERROR("[%s] load_fdf(%s): can't read the file", mod->id.c_str(), path);
        return TOME_ERR_FAILED;
    }
    std::string key = mod->id + "\n" + path;
    auto loaded = g_loaded_fdf.find(key);
    if (loaded != g_loaded_fdf.end()) {
        if (loaded->second != contents) {
            TOME_WARN("[%s] %s changed; the game keeps the definitions it already has until a restart",
                      mod->id.c_str(), path);
        }
        return TOME_OK;
    }

    // Into the generated overlay, which the game reads like its own archives.
    std::string fdf_path = ui::mod_fdf_archive_path(mod->id, path);
    std::string toc_path = fdf_path + ".toc";
    if (!overlay::put_generated({{fdf_path, contents}, {toc_path, ui::toc_for(fdf_path)}})) {
        TOME_ERROR("[%s] load_fdf(%s): the overlay archive couldn't be mounted", mod->id.c_str(), path);
        return TOME_ERR_FAILED;
    }
    if (!load_toc(toc_path.c_str())) {
        TOME_ERROR("[%s] load_fdf(%s): the game couldn't parse it", mod->id.c_str(), path);
        return TOME_ERR_FAILED;
    }
    g_loaded_fdf[key] = contents;
    TOME_INFO("[%s] loaded frame definitions from %s", mod->id.c_str(), path);
    return TOME_OK;
}

tome_result TOME_CALL api_load_toc(const char* path) {
    if (!path || !*path) return TOME_ERR_INVALID_ARGUMENT;
    if (tome_result r = check_main_thread("load_toc"); r != TOME_OK) return r;
    return load_toc(path) ? TOME_OK : TOME_ERR_FAILED;
}

tome_frame* TOME_CALL api_frame_get(const char* name, int32_t context) {
    if (!name || !*name) return nullptr;
    if (check_main_thread("frame_get") != TOME_OK) return nullptr;
    void* frame = get(name, context);
    g_table.remember(reinterpret_cast<uintptr_t>(frame), name, context);
    return static_cast<tome_frame*>(frame);
}

tome_frame* TOME_CALL api_frame_create(tome_mod* mod, const char* name, tome_frame* parent, int32_t priority,
                                       int32_t context) {
    if (!mod || !name || !*name || !parent) return nullptr;
    if (check_main_thread("frame_create") != TOME_OK) return nullptr;
    // Two live frames under one name and context corrupt the game's name
    // table (a crash when their screen is destroyed).
    if (get(name, context)) {
        TOME_ERROR("[%s] frame_create(%s, context %ld): that frame already exists; get it with frame_get or use "
                   "another context",
                   mod->id.c_str(), name, static_cast<long>(context));
        return nullptr;
    }
    void* frame = create(name, parent, priority, context);
    if (!frame) {
        TOME_ERROR("[%s] frame_create(%s): no such frame definition", mod->id.c_str(), name);
        return nullptr;
    }
    if (get(name, context) != frame) {
        TOME_WARN("[%s] frame_create(%s, context %ld): not registered under that name", mod->id.c_str(), name,
                  static_cast<long>(context));
    }
    g_table.remember(reinterpret_cast<uintptr_t>(frame), name, context);
    // The parent owns the frame, so there's nothing to free; hide it while
    // that screen still exists.
    std::string frame_name = name;
    mod->add_revoker([frame, frame_name, context] {
        if (on_main_thread() && get(frame_name.c_str(), context) == frame) set_visible(frame, false);
    });
    return static_cast<tome_frame*>(frame);
}

tome_result TOME_CALL api_frame_set_point(tome_frame* frame, int32_t point, tome_frame* relative,
                                          int32_t relative_point, float x, float y) {
    if (!frame || !relative || point < 0 || point >= ui::kFramePointCount || relative_point < 0 ||
        relative_point >= ui::kFramePointCount) {
        return TOME_ERR_INVALID_ARGUMENT;
    }
    if (tome_result r = check_main_thread("frame_set_point"); r != TOME_OK) return r;
    return set_point(frame, point, relative, relative_point, x, y) ? TOME_OK : TOME_ERR_INVALID_ARGUMENT;
}

tome_result TOME_CALL api_frame_set_text(tome_frame* frame, const char* text) {
    if (!frame || !text) return TOME_ERR_INVALID_ARGUMENT;
    if (tome_result r = check_main_thread("frame_set_text"); r != TOME_OK) return r;
    if (!set_text(frame, text)) {
        TOME_ERROR("frame_set_text: a %s isn't a text frame", class_of(frame).c_str());
        return TOME_ERR_INVALID_ARGUMENT;
    }
    return TOME_OK;
}

tome_result TOME_CALL api_frame_set_visible(tome_frame* frame, int32_t visible) {
    if (!frame) return TOME_ERR_INVALID_ARGUMENT;
    if (tome_result r = check_main_thread("frame_set_visible"); r != TOME_OK) return r;
    set_visible(frame, visible != 0);
    return TOME_OK;
}

tome_result TOME_CALL api_frame_set_animation(tome_frame* sprite, const int32_t* tags, int32_t count) {
    if (!sprite || count < 0 || (count > 0 && !tags)) return TOME_ERR_INVALID_ARGUMENT;
    if (tome_result r = check_main_thread("frame_set_animation"); r != TOME_OK) return r;
    if (!set_animation(sprite, reinterpret_cast<const int*>(tags), count)) {
        TOME_ERROR("frame_set_animation: a %s isn't a sprite frame with a model", class_of(sprite).c_str());
        return TOME_ERR_INVALID_ARGUMENT;
    }
    return TOME_OK;
}

// Registers Tome's observer on `button` and returns the command id its
// clicks report. Forgets buttons that are gone; the same button keeps its id.
int click_command(void* button) {
    int command = 0;
    {
        ExclusiveLock hold(g_clicks_lock);
        for (auto it = g_clicks.begin(); it != g_clicks.end();) {
            if (!frame_alive(it->second.button)) {
                it = g_clicks.erase(it);
                continue;
            }
            if (it->second.button == button) command = it->first;
            ++it;
        }
    }
    if (!command) command = g_next_command++;
    g_set_listener(button, command, &g_observer, 1);
    return command;
}

tome_result TOME_CALL api_frame_on_click(tome_mod* mod, tome_frame* button, tome_click_fn fn, void* user) {
    if (!mod || !button || !fn) return TOME_ERR_INVALID_ARGUMENT;
    if (tome_result r = check_main_thread("frame_on_click"); r != TOME_OK) return r;
    if (!is_a(button, "CControl")) {
        TOME_ERROR("[%s] frame_on_click: a %s isn't a button", mod->id.c_str(), class_of(button).c_str());
        return TOME_ERR_INVALID_ARGUMENT;
    }
    if (!frame_alive(button)) {
        TOME_ERROR("[%s] frame_on_click: get or create the button through Tome first", mod->id.c_str());
        return TOME_ERR_INVALID_ARGUMENT;
    }
    int command = click_command(button);
    uint64_t token = g_next_click_token++;
    {
        ExclusiveLock hold(g_clicks_lock);
        g_clicks[command] = {token, mod, fn, user, button, nullptr};
    }
    mod->add_revoker([command, token] {
        ExclusiveLock hold(g_clicks_lock);
        auto it = g_clicks.find(command);
        if (it != g_clicks.end() && it->second.token == token) g_clicks.erase(it);
    });
    return TOME_OK;
}

tome_frame* TOME_CALL api_ui_root(int32_t event) {
    if (!valid_ui_event(event) || check_main_thread("ui_root") != TOME_OK || !screen_live(event)) return nullptr;
    void* root = event == TOME_UI_MAIN_MENU ? g_menu : g_screens[event].root;
    return static_cast<tome_frame*>(root);
}

// --- JASS natives ------------------------------------------------------------------
// 1.31's names, but frames are integer ids (FrameTable) instead of the
// framehandle type, which 1.27b doesn't have. Ids are checked on every use.

const char* jass_string(const void* value) {
    return value ? g_string_text(value) : nullptr;
}

void* jass_frame(int32_t id) {
    const ui::FrameRecord* record = g_table.by_id(id);
    if (!record) return nullptr;
    void* frame = reinterpret_cast<void*>(record->frame);
    return get(record->name.c_str(), record->context) == frame ? frame : nullptr;
}

// native BlzGetFrameByName takes string name, integer createContext returns integer
int32_t native_get_frame_by_name(const void* name, int32_t context) {
    const char* text = jass_string(name);
    if (!text || !*text) return 0;
    return g_table.remember(reinterpret_cast<uintptr_t>(get(text, context)), text, context);
}

// native BlzCreateFrame takes string name, integer owner, integer priority, integer createContext returns integer
int32_t native_create_frame(const void* name, int32_t owner, int32_t priority, int32_t context) {
    const char* text = jass_string(name);
    void* parent = jass_frame(owner);
    if (!text || !*text || !parent || get(text, context)) return 0;  // see api_frame_create
    return g_table.remember(reinterpret_cast<uintptr_t>(create(text, parent, priority, context)), text, context);
}

// native BlzFrameSetPoint takes integer frame, integer point, integer relative, integer relativePoint, real x, real y returns nothing
int32_t native_frame_set_point(int32_t frame, int32_t point, int32_t relative, int32_t relative_point,
                               const float* x, const float* y) {
    void* f = jass_frame(frame);
    void* rel = jass_frame(relative);
    if (f && rel && point >= 0 && point < ui::kFramePointCount && relative_point >= 0 &&
        relative_point < ui::kFramePointCount) {
        set_point(f, point, rel, relative_point, *x, *y);
    }
    return 0;
}

// native BlzFrameSetText takes integer frame, string text returns nothing
int32_t native_frame_set_text(int32_t frame, const void* text) {
    void* f = jass_frame(frame);
    const char* value = jass_string(text);
    if (f) set_text(f, value ? value : "");
    return 0;
}

// native BlzFrameSetVisible takes integer frame, boolean visible returns nothing
int32_t native_frame_set_visible(int32_t frame, int32_t visible) {
    if (void* f = jass_frame(frame)) set_visible(f, visible != 0);
    return 0;
}

// native BlzLoadTOCFile takes string TOCFile returns boolean
// The TOC's path and the FDF paths it lists come from the map: each is
// refused like the built-in file natives' paths (patches/path_sanitizer.cpp)
// if it leaves the data folder. Logged once, so a map can't flood the log.
int32_t native_load_toc_file(const void* path) {
    const char* text = jass_string(path);
    if (!text || !*text) return 0;
    std::string toc;
    if (path_guard::is_unsafe(text) || !storm::init() || !storm::read_file(text, toc) ||
        path_guard::toc_is_unsafe(toc)) {
        static bool logged = false;
        if (!logged) TOME_WARN("BlzLoadTOCFile: refused a TOC outside the data folder or listing a file outside it");
        logged = true;
        return 0;
    }
    return load_toc(text) ? 1 : 0;
}

struct NativeDef {
    const char* declaration;
    void* fn;
};

void declare_natives() {
    const NativeDef defs[] = {
        {"native BlzGetFrameByName takes string name, integer createContext returns integer",
         reinterpret_cast<void*>(&native_get_frame_by_name)},
        {"native BlzCreateFrame takes string name, integer owner, integer priority, integer createContext "
         "returns integer",
         reinterpret_cast<void*>(&native_create_frame)},
        {"native BlzFrameSetPoint takes integer frame, integer point, integer relative, integer relativePoint, "
         "real x, real y returns nothing",
         reinterpret_cast<void*>(&native_frame_set_point)},
        {"native BlzFrameSetText takes integer frame, string text returns nothing",
         reinterpret_cast<void*>(&native_frame_set_text)},
        {"native BlzFrameSetVisible takes integer frame, boolean visible returns nothing",
         reinterpret_cast<void*>(&native_frame_set_visible)},
        {"native BlzLoadTOCFile takes string TOCFile returns boolean", reinterpret_cast<void*>(&native_load_toc_file)},
    };
    for (const NativeDef& def : defs) {
        std::string error;
        if (!natives::declare("tome", def.declaration, error)) continue;
        std::string name = def.declaration + 7;  // after "native "
        natives::attach("tome", name.substr(0, name.find(' ')), def.fn);
    }
}

} // namespace

void init(uintptr_t game_base, const BuildMappings& build, uint32_t main_thread_id) {
    const auto& ui = build.ui;
    g_build = &build;
    g_main_thread = main_thread_id;
    g_image_begin = game_base;
    g_image_end = game_base + build.data->size_of_image;
    g_load_toc_files = reinterpret_cast<LoadTocFilesFn>(game_base + ui.load_toc_files);
    g_fdf_table_a = reinterpret_cast<void*>(game_base + ui.fdf_table_a);
    g_fdf_table_b = reinterpret_cast<void*>(game_base + ui.fdf_table_b);
    g_fdf_status = reinterpret_cast<void*>(game_base + ui.fdf_default_status);
    g_get_frame = reinterpret_cast<GetFrameByNameFn>(game_base + ui.get_frame_by_name);
    g_create_frame = reinterpret_cast<CreateFrameFn>(game_base + ui.create_frame);
    g_set_point = reinterpret_cast<SetPointFn>(game_base + ui.layout_set_point);
    g_sprite_instance = reinterpret_cast<SpriteInstanceFn>(game_base + ui.sprite_get_instance);
    g_set_animation = reinterpret_cast<SetAnimationFn>(game_base + ui.model_set_animation);
    g_set_text = reinterpret_cast<SetTextFn>(game_base + ui.text_frame_set_text);
    g_string_text = reinterpret_cast<StringTextFn>(game_base + build.jass.string_get_text);
    g_set_listener = reinterpret_cast<SetListenerFn>(game_base + ui.control_set_listener);
    g_set_focus = reinterpret_cast<SetFocusFn>(game_base + ui.frame_set_focus);
    g_focused_frame = reinterpret_cast<void**>(game_base + ui.focused_frame);
    g_set_width = reinterpret_cast<SetWidthFn>(game_base + ui.layout_set_width);
    g_control_click = reinterpret_cast<ControlClickFn>(game_base + ui.control_click);
    g_has_event = reinterpret_cast<HasEventFn>(game_base + ui.observer_has_event);
    g_click_event = ui.control_click_event;
    g_edit_get_text = reinterpret_cast<EditGetTextFn>(game_base + ui.edit_get_text);
    g_edit_set_text = reinterpret_cast<EditSetTextFn>(game_base + ui.edit_set_text);
    g_glue_get = reinterpret_cast<GlueGetFn>(game_base + build.game.glue_get);
    g_game_ui_get = game_ui_get(game_base, build);
    init_observer(game_base);
    g_ready = true;
    declare_natives();
}

bool ready() {
    return g_ready;
}

bool on_main_thread() {
    return GetCurrentThreadId() == g_main_thread;
}

void set_main_thread(uint32_t main_thread_id) {
    g_main_thread = main_thread_id;
}

void fill_api(tome_api& api) {
    // Each one returns TOME_ERR_UNAVAILABLE (or NULL) until init has run.
    api.on_ui = api_on_ui;
    api.load_fdf = api_load_fdf;
    api.load_toc = api_load_toc;
    api.frame_get = api_frame_get;
    api.frame_create = api_frame_create;
    api.frame_set_point = api_frame_set_point;
    api.frame_set_text = api_frame_set_text;
    api.frame_set_visible = api_frame_set_visible;
    api.frame_set_animation = api_frame_set_animation;
    api.frame_on_click = api_frame_on_click;
    api.ui_root = api_ui_root;
}

void* get(const char* name, int context) {
    return g_get_frame(name, context);
}

void* create(const char* definition, void* parent, int priority, int context) {
    return g_create_frame(definition, parent, 0, priority, context);
}

bool set_point(void* frame, int point, void* relative, int relative_point, float x, float y) {
    if (!frame || !relative) return false;
    uint32_t offset = g_build->ui.layout_offset;
    g_set_point(static_cast<char*>(frame) + offset, point, static_cast<char*>(relative) + offset, relative_point, x,
                y, 1);
    return true;
}

bool set_width(void* frame, float width) {
    if (!frame) return false;
    g_set_width(static_cast<char*>(frame) + g_build->ui.layout_offset, width);
    return true;
}

bool on_click(void* button, const char* name, int context, std::function<void()> fn) {
    if (!g_ready || !button || !is_a(button, "CControl")) return false;
    g_table.remember(reinterpret_cast<uintptr_t>(button), name, context);
    int command = click_command(button);
    ExclusiveLock hold(g_clicks_lock);
    g_clicks[command] = {g_next_click_token++, nullptr, nullptr, nullptr, button, std::move(fn)};
    return true;
}

bool edit_text(void* frame, std::string& out) {
    if (!is_a(frame, "CEditBox")) return false;
    const char* text = g_edit_get_text(frame);
    out = text ? text : "";
    return true;
}

bool set_edit_text(void* frame, const char* text) {
    if (!is_a(frame, "CEditBox")) return false;
    g_edit_set_text(frame, text, 0);
    return true;
}

void* focused() {
    return g_ready ? *g_focused_frame : nullptr;
}

void clear_focus() {
    if (void* f = focused()) g_set_focus(f, 0, 0, nullptr);
}

bool set_focus(void* frame) {
    if (!g_ready || !frame) return false;
    g_set_focus(frame, 1, 0, nullptr);
    return focused() == frame;
}

bool click(void* control) {
    if (!g_ready || !is_a(control, "CControl")) return false;
    g_control_click(control, 1);  // the left button, as the game's shortcut path passes
    return true;
}

bool takes_clicks(void* control) {
    return g_ready && control && g_has_event(control, g_click_event) != 0;
}

bool game_showing() {
    return g_ready && screen_live(TOME_UI_GAME);
}

bool is_class(void* object, const char* name) {
    return is_a(object, name);
}

bool typing() {
    if (!g_ready) return false;
    void* focused = *g_focused_frame;
    return focused && (is_a(focused, "CEditBox") || is_a(focused, "CChatEditBar"));
}

bool set_text(void* frame, const char* text) {
    if (!is_a(frame, "CTextFrame")) return false;
    g_set_text(frame, text);
    return true;
}

void set_visible(void* frame, bool visible) {
    if (frame) call_frame_method(frame, visible ? g_build->ui.frame_show_offset : g_build->ui.frame_hide_offset);
}

bool set_animation(void* sprite, const int* tags, int count) {
    if (!is_a(sprite, "CSpriteFrame")) return false;
    void* instance = g_sprite_instance(sprite);
    if (!instance) return false;
    g_set_animation(instance, tags, count, 0);
    return true;
}

bool load_toc(const char* path) {
    return g_load_toc_files(path, g_fdf_table_a, g_fdf_table_b, g_fdf_status) != 0;
}

void on_main_menu_setup(void* menu) {
    if (!g_ready) return;
    g_menu = menu;
    g_menu_root = get("MainMenuFrame");
    ++g_screens[TOME_UI_MAIN_MENU].generation;
    g_menu_map_loads = natives::map_load_count();
    TOME_DEBUG("Main menu %p, MainMenuFrame %p", menu, g_menu_root);
    run_ui_hooks(TOME_UI_MAIN_MENU);
    popups::on_screen();
}

void on_screen(int32_t ui_event, void* root) {
    if (!g_ready || !valid_ui_event(ui_event) || ui_event == TOME_UI_MAIN_MENU) return;
    g_screens[ui_event].root = root;
    ++g_screens[ui_event].generation;
    run_ui_hooks(ui_event);
    popups::on_screen();
}

bool main_menu_showing() {
    // A map loaded since the menu was built means a game is running (or its
    // score screen is up), whether or not the menu's frames still exist.
    return g_ready && main_menu_live() && natives::map_load_count() == g_menu_map_loads;
}

void* popup_parent(bool& in_game) {
    in_game = false;
    if (!g_ready) return nullptr;
    if (screen_live(TOME_UI_GAME)) {
        in_game = true;
        return g_screens[TOME_UI_GAME].root;
    }
    // The glue manager's current screen first: the main menu's frames may
    // outlive it while another menu screen shows.
    for (int32_t event = TOME_UI_MAIN_MENU + 1; event < TOME_UI_GAME; ++event) {
        if (screen_live(event)) return event == TOME_UI_LOADING ? nullptr : g_screens[event].root;
    }
    return main_menu_showing() ? get("ControlLayer") : nullptr;
}

void catch_up() {
    if (!g_ready || !on_main_thread()) return;
    for (int32_t event = TOME_UI_MAIN_MENU; event <= kUiEventCount; ++event) run_ui_hooks(event);
}

} // namespace tome::frames
