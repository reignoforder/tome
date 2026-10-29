#include "api.h"

#include "events.h"
#include "frames.h"
#include "hooks.h"
#include "intermod.h"
#include "keybinds.h"
#include "log.h"
#include "mod_manager.h"
#include "natives.h"
#include "popups.h"
#include "settings_api.h"
#include "storm.h"
#include "units.h"
#include "unwind.h"

#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

namespace tome {
namespace {

std::string g_build_name;
std::string g_file_version;
tome_game_info g_game_info;
std::vector<tome_symbol> g_symbols;  // the running build's mapping database
tome_api g_api;

void TOME_CALL api_log(tome_mod* mod, tome_log_level level, const char* message) {
    const char* id = mod ? mod->id.c_str() : "?";
    if (!message) message = "(null)";
    switch (level) {
    case TOME_LOG_DEBUG: TOME_DEBUG("[%s] %s", id, message); break;
    case TOME_LOG_INFO:  TOME_INFO("[%s] %s", id, message); break;
    case TOME_LOG_WARN:  TOME_WARN("[%s] %s", id, message); break;
    default:             TOME_ERROR("[%s] %s", id, message); break;
    }
}

const tome_game_info* TOME_CALL api_game_info() {
    return &g_game_info;
}

const char* TOME_CALL api_mod_id(tome_mod* mod) {
    return mod ? mod->id.c_str() : "";
}

const char* TOME_CALL api_mod_version(tome_mod* mod) {
    return mod ? mod->version.c_str() : "";
}

const char* TOME_CALL api_mod_dir(tome_mod* mod) {
    return mod ? mod->dir.c_str() : "";
}

const char* TOME_CALL api_config_dir(tome_mod* mod) {
    if (!mod) return "";
    CreateDirectoryW(mod->config_dir_w.c_str(), nullptr);
    return mod->config_dir.c_str();
}

tome_result TOME_CALL api_register_native(tome_mod* mod, const char* name, void* fn) {
    if (!mod || !name || !fn) return TOME_ERR_INVALID_ARGUMENT;
    switch (natives::attach(mod->id, name, fn, mod)) {
    case natives::AttachResult::Ok:
        break;
    case natives::AttachResult::NotDeclared:
        TOME_ERROR("[%s] register_native(%s): not declared in tome.mod.json \"natives\"", mod->id.c_str(), name);
        return TOME_ERR_NOT_DECLARED;
    case natives::AttachResult::NotOwner:
        TOME_ERROR("[%s] register_native(%s): declared by another mod", mod->id.c_str(), name);
        return TOME_ERR_NOT_OWNER;
    }
    std::string owner = mod->id, native = name;
    mod->add_revoker([owner, native] { natives::detach(owner, native); });
    return TOME_OK;
}

const tome_symbol* TOME_CALL api_resolve(const char* name) {
    if (!name) return nullptr;
    for (const tome_symbol& sym : g_symbols) {
        if (std::strcmp(sym.name, name) == 0) return &sym;
    }
    return nullptr;
}

size_t TOME_CALL api_read_game_file(const char* name, void* buffer, size_t size) {
    std::string data;
    if (!name || !storm::init() || !storm::read_file(name, data)) return 0;
    if (buffer && size) std::memcpy(buffer, data.data(), data.size() < size ? data.size() : size);
    return data.size();
}

bool valid_phase(int32_t phase) {
    return phase == TOME_HOOK_BEFORE || phase == TOME_HOOK_REPLACE || phase == TOME_HOOK_AFTER;
}

tome_result TOME_CALL api_hook(tome_mod* mod, const char* name, int32_t phase, int32_t priority, tome_hook_fn fn,
                               void* user) {
    if (!mod || !name || !fn || !valid_phase(phase)) return TOME_ERR_INVALID_ARGUMENT;
    if (hooks::is_reserved(name)) {
        TOME_ERROR("[%s] hook(%s): reserved by a built-in Tome patch; mods can't hook it", mod->id.c_str(), name);
        return TOME_ERR_NOT_OWNER;
    }
    std::string error;
    hooks::Target* target = hooks::find_or_install(name, error);
    if (!target) {
        TOME_ERROR("[%s] hook(%s): %s", mod->id.c_str(), name, error.c_str());
        return TOME_ERR_UNAVAILABLE;
    }
    hooks::Handler handler;
    handler.mod = mod;
    handler.phase = phase;
    handler.priority = priority;
    handler.fn = fn;
    handler.user = user;
    handler.gate = std::make_shared<hooks::Gate>();
    // The mod's DLL is freed only once no handler of it runs (ModManager::unload).
    // Under the mod's lock: hook() may run on any thread, alongside disable.
    ExclusiveLock hold(mod->lock);
    std::erase_if(mod->hook_gates, [](const auto& g) { return g->revoked && g->busy == 0; });
    mod->hook_gates.push_back(handler.gate);
    hooks::add(target, handler);
    // Removes exactly this registration (the same fn and user may be hooked twice).
    const hooks::Gate* gate = handler.gate.get();
    mod->revokers.push_back({[target, gate] { hooks::remove_gate(target, gate); }, gate});
    return TOME_OK;
}

tome_result TOME_CALL api_unhook(tome_mod* mod, const char* name, int32_t phase, tome_hook_fn fn, void* user) {
    if (!mod || !name || !fn || !valid_phase(phase)) return TOME_ERR_INVALID_ARGUMENT;
    if (hooks::is_reserved(name)) return TOME_ERR_NOT_OWNER;  // a built-in patch owns it; nothing of the mod's to remove
    hooks::Target* target = hooks::find(name);
    std::vector<const void*> removed;
    if (!target || hooks::remove(target, mod, phase, fn, user, &removed) == 0) return TOME_ERR_INVALID_ARGUMENT;
    mod->drop_revokers(removed);
    return TOME_OK;
}

void TOME_CALL api_hook_call_next(tome_hook_call* call) {
    hooks::call_next(call);
}

tome_symbol_kind to_api_kind(mappings::Kind kind) {
    switch (kind) {
    case mappings::Kind::Function: return TOME_SYMBOL_FUNCTION;
    case mappings::Kind::Data:     return TOME_SYMBOL_DATA;
    case mappings::Kind::Call:     return TOME_SYMBOL_CALL;
    case mappings::Kind::Offset:   return TOME_SYMBOL_OFFSET;
    case mappings::Kind::Constant: return TOME_SYMBOL_CONSTANT;
    case mappings::Kind::Import:   return TOME_SYMBOL_IMPORT;
    }
    return TOME_SYMBOL_CONSTANT;
}

} // namespace

void init_api(const GameBuild& build) {
    g_build_name = build.known ? build.known->data->name : "unknown";
    g_file_version = build.file_version;

    g_game_info = {};
    g_game_info.size = sizeof(g_game_info);
    g_game_info.build_name = g_build_name.c_str();
    g_game_info.file_version = g_file_version.c_str();
    g_game_info.game_base = build.base;

    // The strings point into the database, which lives as long as the process.
    g_symbols.clear();
    if (build.known) {
        for (const mappings::Symbol& sym : build.known->data->symbols) {
            tome_symbol out = {};
            out.size = sizeof(out);
            out.name = sym.name.c_str();
            out.kind = to_api_kind(sym.kind);
            out.rva = mappings::has_rva(sym.kind) ? sym.value : 0;
            out.value = mappings::has_rva(sym.kind) ? build.base + sym.value : sym.value;
            out.convention = mappings::to_string(sym.convention);
            out.signature = sym.signature.c_str();
            g_symbols.push_back(out);
        }
    }

    g_api = {};
    g_api.size = sizeof(g_api);
    g_api.api_version = TOME_API_VERSION;
    g_api.tome_version = TOME_VERSION;
    g_api.log = api_log;
    g_api.game_info = api_game_info;
    g_api.mod_id = api_mod_id;
    g_api.mod_version = api_mod_version;
    g_api.mod_dir = api_mod_dir;
    g_api.config_dir = api_config_dir;
    g_api.register_native = api_register_native;
    g_api.resolve = api_resolve;
    g_api.read_game_file = api_read_game_file;
    frames::fill_api(g_api);
    hooks::init(build.base, build.known ? build.known->data : nullptr);
    g_api.hook = api_hook;
    g_api.unhook = api_unhook;
    g_api.hook_call_next = api_hook_call_next;
    events::fill_api(g_api);
    natives::fill_api(g_api);
    settings_api::fill_api(g_api);
    keybinds::fill_api(g_api);
    units::fill_api(g_api);
    popups::fill_api(g_api);
    intermod_api::fill_api(g_api);
}

const tome_api* api_table() {
    return &g_api;
}

} // namespace tome
