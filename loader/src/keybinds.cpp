// SPDX-License-Identifier: GPL-3.0-or-later
#include "keybinds.h"

#include "core/json.h"
#include "frames.h"
#include "guard.h"
#include "log.h"
#include "main_thread.h"
#include "mod_handle.h"
#include "mod_menu.h"
#include "popups.h"
#include "unwind.h"

#include <cstring>
#include <map>
#include <utility>

namespace tome::keybinds {
namespace {

struct Handler {
    uint64_t token;
    tome_mod* mod;
    tome_keybind_fn fn;
    void* user;
};

std::wstring g_file;  // tome/keybinds.json

// Tome's own binding, listed first: opens the mod menu (in games, the Esc
// menu with Mods open).
constexpr const char* kTomeMod = "tome";
constexpr const char* kMenuId = "menu";
constexpr const char* kMenuDefault = "Ctrl+F10";

SRWLOCK g_lock = SRWLOCK_INIT;  // the API may be called on any thread
json::Value g_stored;            // tome/keybinds.json as last read or written
std::map<std::pair<std::string, std::string>, Handler> g_handlers;  // by (mod id, keybind id)
uint64_t g_next_token = 1;

std::vector<keys::Binding> g_declared;  // Tome's, then from set_mods every installed mod's, with their defaults
keys::CustomKeys g_custom;              // the player's CustomKeys.txt, if there is one
// list() is rebuilt only when the mods or the player's choices changed.
std::vector<keys::Binding> g_cache;
bool g_cache_valid = false;

// Through a temp file, like state.json.
void write_stored() {
    if (!write_file_atomic(g_file, json::serialize(g_stored))) TOME_WARN("Couldn't write tome/keybinds.json");
}

// Call with g_lock held exclusively.
const std::vector<keys::Binding>& cached_list() {
    if (g_cache_valid) return g_cache;
    g_cache = g_declared;
    keys::apply_overrides(g_cache, g_stored);
    keys::find_conflicts(g_cache, &g_custom);
    g_cache_valid = true;
    return g_cache;
}

keys::Key key_of(WPARAM wparam) {
    keys::Key key;
    key.vk = static_cast<uint8_t>(wparam);
    if (GetKeyState(VK_CONTROL) & 0x8000) key.mods |= keys::kCtrl;
    if (GetKeyState(VK_SHIFT) & 0x8000) key.mods |= keys::kShift;
    if (GetKeyState(VK_MENU) & 0x8000) key.mods |= keys::kAlt;
    return key;
}

// The keyboard hook (main thread): true if the key was used and the game
// shouldn't see it.
bool on_key(UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN) return false;
    if (wparam == 0 || wparam > 0xFF) return false;
    keys::Key key = key_of(wparam);
    bool repeat = (lparam & (1 << 30)) != 0;
    // A popup first, then the menu: a key for a keybind, Esc, Enter in its text boxes.
    if (popups::on_key(key, repeat)) return true;
    if (mod_menu::on_key(key, repeat)) return true;
    if (repeat) return false;  // once per press
    if (keys::is_modifier(key.vk) || frames::typing()) return false;

    bool open_menu = false;
    {
        ExclusiveLock hold(g_lock);
        for (const keys::Binding& b : cached_list()) {
            if (b.mod == kTomeMod && b.id == kMenuId && b.key == key) open_menu = true;
        }
    }
    if (open_menu && mod_menu::open_from_key()) return true;

    std::vector<Handler> due;
    std::vector<std::string> ids;
    {
        ExclusiveLock hold(g_lock);
        for (const keys::Binding& b : cached_list()) {
            if (!(b.key == key)) continue;
            auto it = g_handlers.find({b.mod, b.id});
            if (it == g_handlers.end()) continue;
            due.push_back(it->second);
            ids.push_back(b.id);
        }
    }
    OnUnwind release([&] {
        std::vector<Handler>().swap(due);
        std::vector<std::string>().swap(ids);
    });
    for (size_t i = 0; i < due.size(); ++i) {
        const Handler& h = due[i];
        // An earlier callback may have revoked this one (its mod disabled,
        // or the same id registered again): only run what's still there.
        bool registered = false;
        {
            SharedLock hold(g_lock);
            for (const auto& [slot, current] : g_handlers) registered |= current.token == h.token;
        }
        if (!registered) continue;
        guard::run(h.mod, "keybind callback", ids[i].c_str(), [&] { h.fn(h.mod, ids[i].c_str(), h.user); });
    }
    return !due.empty();
}

bool declared(tome_mod* mod, const char* id) {
    SharedLock hold(g_lock);
    for (const keys::Binding& b : g_declared) {
        if (b.mod == mod->id && b.id == id) return true;
    }
    return false;
}

tome_result TOME_CALL api_on_keybind(tome_mod* mod, const char* id, tome_keybind_fn fn, void* user) {
    if (!mod || !id || !fn) return TOME_ERR_INVALID_ARGUMENT;
    if (!declared(mod, id)) {
        TOME_ERROR("[%s] on_keybind(%s): not declared in tome.mod.json \"keybinds\"", mod->id.c_str(), id);
        return TOME_ERR_NOT_DECLARED;
    }
    auto slot = std::make_pair(mod->id, std::string(id));
    uint64_t token;
    {
        ExclusiveLock hold(g_lock);
        token = g_next_token++;
        g_handlers[slot] = {token, mod, fn, user};
    }
    mod->add_revoker([slot, token] {
        ExclusiveLock hold(g_lock);
        auto it = g_handlers.find(slot);
        if (it != g_handlers.end() && it->second.token == token) g_handlers.erase(it);
    });
    return TOME_OK;
}

size_t TOME_CALL api_keybind_name(tome_mod* mod, const char* id, char* buffer, size_t size) {
    if (buffer && size > 0) buffer[0] = '\0';
    if (!mod || !id) return 0;
    std::string name;
    {
        ExclusiveLock hold(g_lock);
        for (const keys::Binding& b : cached_list()) {
            if (b.mod == mod->id && b.id == id) name = keys::format(b.key);
        }
    }
    if (name.empty()) return 0;
    if (buffer && size > 0) {
        size_t n = name.size() < size - 1 ? name.size() : size - 1;
        std::memcpy(buffer, name.data(), n);
        buffer[n] = '\0';
    }
    return name.size();
}

keys::Binding tome_binding() {
    keys::Binding b;
    b.mod = kTomeMod;
    b.mod_name = "Tome";
    b.id = kMenuId;
    b.label = "Open the mod menu";
    keys::parse(kMenuDefault, b.fallback);
    b.key = b.fallback;
    return b;
}

} // namespace

void init(const Paths& paths) {
    g_file = paths.tome_dir + L"\\keybinds.json";
    std::string text;
    if (read_file(g_file, text)) {
        json::Value root;
        json::ParseResult parsed = json::parse(text, root);
        if (parsed.ok && root.is_object()) {
            ExclusiveLock hold(g_lock);
            g_stored = std::move(root);
            g_cache_valid = false;
        } else {
            TOME_WARN("tome/keybinds.json is invalid (%s); using every mod's default keys",
                      parsed.ok ? "not an object" : parsed.error.c_str());
        }
    }
    if (read_file(paths.game_dir + L"\\CustomKeys.txt", text)) {
        keys::CustomKeys custom = keys::parse_custom_keys(text);
        TOME_INFO("CustomKeys.txt: %zu hotkeys, checked against keybinds", custom.keys.size());
        ExclusiveLock hold(g_lock);
        g_custom = std::move(custom);
        g_cache_valid = false;
    }
    {
        ExclusiveLock hold(g_lock);
        if (g_declared.empty()) g_declared.push_back(tome_binding());
    }
    main_thread::set_key_hook(on_key);
}

void fill_api(tome_api& api) {
    api.on_keybind = api_on_keybind;
    api.keybind_name = api_keybind_name;
}

void set_mods(std::vector<keys::Binding> declared) {
    ExclusiveLock hold(g_lock);
    declared.insert(declared.begin(), tome_binding());
    g_declared = std::move(declared);
    g_cache_valid = false;
}

std::vector<keys::Binding> list() {
    ExclusiveLock hold(g_lock);
    return cached_list();
}

void set(const std::string& mod, const std::string& id, keys::Key key) {
    {
        ExclusiveLock hold(g_lock);
        std::vector<keys::Binding> bindings = cached_list();
        for (keys::Binding& b : bindings) {
            if (b.mod == mod && b.id == id) b.key = key;
        }
        g_stored = keys::overrides(bindings, g_stored);
        g_cache_valid = false;
        write_stored();
    }
    TOME_INFO("Keybind %s/%s: %s", mod.c_str(), id.c_str(), keys::format(key).c_str());
}

bool press_for_test(const std::string& mod, const std::string& id) {
    Handler h;
    {
        SharedLock hold(g_lock);
        auto it = g_handlers.find({mod, id});
        if (it == g_handlers.end()) return false;
        h = it->second;
    }
    guard::run(h.mod, "keybind callback", id.c_str(), [&] { h.fn(h.mod, id.c_str(), h.user); });
    return true;
}

void reset_all() {
    {
        ExclusiveLock hold(g_lock);
        std::vector<keys::Binding> bindings = cached_list();
        for (keys::Binding& b : bindings) b.key = b.fallback;
        g_stored = keys::overrides(bindings, g_stored);
        g_cache_valid = false;
        write_stored();
    }
    TOME_INFO("Keybinds reset to their defaults");
}

} // namespace tome::keybinds
