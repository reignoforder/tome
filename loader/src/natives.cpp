// SPDX-License-Identifier: GPL-3.0-or-later
#include "natives.h"

#include "core/jass.h"
#include "frames.h"
#include "guard.h"
#include "hooks.h"
#include "log.h"
#include "mods_ready.h"
#include "patch.h"
#include "unwind.h"

#include <tome/utf8.h>
#include <windows.h>

#include <cstring>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>

// GCC warns that __thiscall is meant for class methods but still applies it
// to function pointer types (see mod_menu.cpp).
#pragma GCC diagnostic ignored "-Wattributes"

namespace tome::natives {
namespace {

using BindNativeFn = void(__fastcall*)(void* fn, const char* name, const char* signature);
using InitNativesFn = uint32_t (*)();
using StringTextFn = const char*(__thiscall*)(const void* jass_string);
using CreateStringFn = uint32_t(__fastcall*)(const char* text);
// The game's own natives, called like the VM calls them (cdecl, handles as ids).
using NativeFn0 = uint32_t (*)();
using NativeFn1 = uint32_t (*)(uint32_t);
using NativeFn2 = uint32_t (*)(uint32_t, uint32_t);

constexpr size_t kThunkSize = 64;
constexpr size_t kThunkPage = 4096;

struct Slot {
    std::string name;
    std::string signature;
    std::string owner;
    void* volatile target = nullptr;  // read by the thunk on every call
    Handler* volatile handler = nullptr;  // instead of a function (attach_handler); target is then the handler
    std::string handler_signature;        // what the handler converts arguments and its result by
    std::string bound;                    // `signature` as the engine has it (the last InitNatives); "" before
    bool stale_logged = false;            // main thread
    volatile uint32_t code_mask = 0;  // bit i: parameter i is `code`; read by the thunk
    uint32_t arg_count = 0;           // parameters; each one is a 32-bit word
    tome_mod* mod = nullptr;          // the attached mod, for the guard (main thread)
    uint8_t* thunk = nullptr;
    unsigned bind_count = 0;
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<std::unique_ptr<Slot>> g_slots;
std::map<std::string, Slot*> g_by_name;
uint8_t* g_thunk_page = nullptr;
size_t g_thunk_used = kThunkPage;

BindNativeFn g_bind_native = nullptr;
InitNativesFn g_init_natives = nullptr;
unsigned g_map_loads = 0;

// Strings and code (API version 6).
const void* const* g_game_instance = nullptr;  // CGameWar3.instance
StringTextFn g_string_text = nullptr;
CreateStringFn g_create_string = nullptr;
NativeFn0 g_create_trigger = nullptr;
NativeFn1 g_destroy_trigger = nullptr;
NativeFn1 g_condition = nullptr;
NativeFn2 g_trigger_add_condition = nullptr;
NativeFn1 g_trigger_evaluate = nullptr;
NativeFn2 g_trigger_add_action = nullptr;
NativeFn1 g_trigger_execute = nullptr;

// Main thread. Both are per map script: a code value is an index into the
// script's code table, which starts over with every map, so an old value
// would name a different function. Cleared at every InitNatives.
std::unordered_set<uint32_t> g_codes_seen;                // passed to a Tome native
std::unordered_map<uint32_t, uint32_t> g_action_triggers;  // code -> trigger with it as its action

// Called by a thunk whose native takes `code`, before the mod's function.
void note_codes(const Slot* slot, const uint32_t* args) {
    for (uint32_t mask = slot->code_mask, i = 0; mask; mask >>= 1, ++i) {
        if ((mask & 1) && args[i]) g_codes_seen.insert(args[i]);
    }
}

// A mod's native, called by its thunk: runs it under a guard with a copy
// of the arguments. A crashed native returns 0, like a disabled one.
uint32_t call_guarded(Slot* slot, const uint32_t* args) {
    // Both at once (attach, attach_handler and detach write them under the
    // lock): read one by one, a change in between could pair a handler's
    // target with no handler, and call the Handler object as code. Not held
    // during the call, which may attach or detach natives itself.
    void* fn;
    Handler* handler;
    bool stale = false;
    std::string bound, registered;  // only when stale, for the log
    {
        SharedLock hold(g_lock);
        fn = slot->target;
        handler = slot->handler;
        // A handler trusts the signature it was attached with: the engine
        // may have another (tome.mod.json changed it, and a map loaded since,
        // or the mod registered it again before one did), and then a result
        // checked as one type would reach the engine as another (an integer
        // as a string). 0 until they agree again.
        stale = handler && slot->handler_signature != slot->bound;
        if (stale && !slot->stale_logged) {
            bound = slot->bound;
            registered = slot->handler_signature;
        }
    }
    if (!fn) return 0;
    uint32_t result = 0;
    if (stale) {
        if (!slot->stale_logged) {
            slot->stale_logged = true;
            TOME_WARN("Native %s: the map has it as %s, but its mod registered it as %s; it returns 0 until they "
                      "match (the next map load, or enabling the mod again)",
                      slot->name.c_str(), bound.c_str(), registered.c_str());
        }
        return 0;
    }
    if (handler) {
        guard::run(slot->mod, "native", slot->name.c_str(), [&] { result = handler->call(args); });
        return result;
    }
    guard::run(slot->mod, "native", slot->name.c_str(),
               [&] { result = hooks::call_with_stack(fn, args, slot->arg_count); });
    return result;
}

// If the native takes `code`: note_codes(slot, the arguments). Then
// mov eax, [slot->target] ; test eax, eax ; jz default ; <call> ; default: xor eax, eax ; ret
// Tome's own natives jump to the target: natives are __cdecl, so jumping
// (not calling) hands it the engine's exact stack frame, and the caller
// cleans up either way. A mod's native goes through call_guarded(slot,
// the arguments) instead. eax, ecx and edx are free to use at a cdecl
// function's entry.
uint8_t* make_thunk(Slot* slot, bool guarded) {
    if (g_thunk_used + kThunkSize > kThunkPage) {
        g_thunk_page = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, kThunkPage, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!g_thunk_page) return nullptr;
        g_thunk_used = 0;
    }
    uint8_t* code = g_thunk_page + g_thunk_used;
    g_thunk_used += kThunkSize;

    auto mask_addr = reinterpret_cast<uintptr_t>(&slot->code_mask);
    auto slot_addr = reinterpret_cast<uintptr_t>(slot);
    auto target_addr = reinterpret_cast<uintptr_t>(&slot->target);
    uint8_t bytes[kThunkSize] = {
        0x83, 0x3D, 0, 0, 0, 0, 0x00,  // cmp dword [code_mask], 0
        0x74, 0x12,                    // je call_target
        0x8D, 0x44, 0x24, 0x04,        // lea eax, [esp+4]: the arguments
        0x50,                          // push eax
        0x68, 0, 0, 0, 0,              // push slot
        0xE8, 0, 0, 0, 0,              // call note_codes
        0x83, 0xC4, 0x08,              // add esp, 8
        0xA1, 0, 0, 0, 0,              // call_target: mov eax, [target]
        0x85, 0xC0,                    // test eax, eax
        0x74, 0x02,                    // jz default
        0xFF, 0xE0,                    // jmp eax
        0x31, 0xC0,                    // default: xor eax, eax
        0xC3,                          // ret
    };
    std::memset(bytes + 41, 0xCC, kThunkSize - 41);
    // Guarded: the `jmp eax` becomes a call of call_guarded.
    const uint8_t guarded_call[] = {
        0x74, 0x13,                    // jz default
        0x8D, 0x4C, 0x24, 0x04,        // lea ecx, [esp+4]: the arguments
        0x51,                          // push ecx
        0x68, 0, 0, 0, 0,              // push slot
        0xE8, 0, 0, 0, 0,              // call call_guarded
        0x83, 0xC4, 0x08,              // add esp, 8
        0xC3,                          // ret
        0x31, 0xC0,                    // default: xor eax, eax
        0xC3,                          // ret
    };
    static_assert(34 + sizeof(guarded_call) <= kThunkSize);
    auto rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&note_codes) -
                                    (reinterpret_cast<uintptr_t>(code) + 24));
    std::memcpy(bytes + 2, &mask_addr, 4);
    std::memcpy(bytes + 15, &slot_addr, 4);
    std::memcpy(bytes + 20, &rel, 4);
    std::memcpy(bytes + 28, &target_addr, 4);
    if (guarded) {
        std::memcpy(bytes + 34, guarded_call, sizeof(guarded_call));
        auto guarded_rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&call_guarded) -
                                                (reinterpret_cast<uintptr_t>(code) + 51));
        std::memcpy(bytes + 42, &slot_addr, 4);
        std::memcpy(bytes + 47, &guarded_rel, 4);
    }
    std::memcpy(code, bytes, kThunkSize);
    FlushInstructionCache(GetCurrentProcess(), code, kThunkSize);
    return code;
}

// Called by the engine (main thread) in place of InitNatives.
uint32_t hooked_init_natives() {
    uint32_t result = g_init_natives();
    ++g_map_loads;
    g_codes_seen.clear();
    g_action_triggers.clear();  // the game destroys the old script's handles itself

    mods_ready::wait("binding natives");

    size_t count;
    {
        ExclusiveLock hold(g_lock);  // writes `bound`, which call_guarded reads under the lock
        for (const auto& slot : g_slots) {
            g_bind_native(slot->thunk, slot->name.c_str(), slot->signature.c_str());
            slot->bound = slot->signature;
            slot->stale_logged = false;
            ++slot->bind_count;
        }
        count = g_slots.size();
    }

    TOME_INFO("Engine initialized natives; bound %zu Tome native%s", count, count == 1 ? "" : "s");
    return result;
}

// Built-in natives every Tome install provides.

// native TomeIsLoaded takes nothing returns boolean
int32_t native_tome_is_loaded() {
    return 1;
}

// native TomeLogInteger takes integer value returns nothing
int32_t native_tome_log_integer(int32_t value) {
    TOME_INFO("[map] TomeLogInteger(%ld)", static_cast<long>(value));
    return 0;
}

// native TomeLogString takes string value returns nothing
int32_t native_tome_log_string(const void* value) {
    const char* text = string_text(value);
    TOME_INFO("[map] TomeLogString(%s%s%s)", text ? "\"" : "", text ? text : "null", text ? "\"" : "");
    return 0;
}

// --- strings and code (API version 6) ---

// The map script's VM lives in the CGameWar3; natives only run while there is one.
bool have_game() {
    return g_game_instance && *g_game_instance;
}

const char* TOME_CALL api_jass_string(tome_jass_string value) {
    return string_text(value);
}

tome_jass_string_ret TOME_CALL api_jass_string_create(const char* text) {
    if (!text || !g_create_string) return 0;
    if (!frames::on_main_thread()) {
        TOME_WARN("jass_string_create called off the main thread; returning null");
        return 0;
    }
    if (!have_game()) {
        TOME_WARN("jass_string_create called without a map script; returning null");
        return 0;
    }
    if (tome_utf8_valid(text)) return g_create_string(text);
    std::string clean(tome_utf8_sanitize(nullptr, 0, text), '\0');
    tome_utf8_sanitize(clean.data(), clean.size() + 1, text);
    return g_create_string(clean.c_str());
}

// Runs `code` the way vJass's .evaluate() does: as the condition of a
// trigger of its own, so it runs to completion before this returns and its
// boolean result comes back. The game keeps one conditionfunc per code
// value, so only the trigger (and with it the triggercondition) is destroyed.
// Checks shared by jass_code_call and jass_code_execute.
tome_result check_code(tome_jass_code code, const char* what) {
    if (!code) return TOME_ERR_INVALID_ARGUMENT;
    if (!g_create_trigger) return TOME_ERR_UNAVAILABLE;
    if (!frames::on_main_thread()) return TOME_ERR_WRONG_THREAD;
    if (!have_game()) return TOME_ERR_UNAVAILABLE;
    if (!g_codes_seen.count(code)) {
        TOME_WARN("%s: code value %lu wasn't passed to a native by the current map script", what,
                  static_cast<unsigned long>(code));
        return TOME_ERR_INVALID_ARGUMENT;
    }
    return TOME_OK;
}

tome_result TOME_CALL api_jass_code_call(tome_jass_code code, int32_t* result) {
    if (result) *result = 0;
    if (tome_result status = check_code(code, "jass_code_call"); status != TOME_OK) return status;
    uint32_t trigger = g_create_trigger();
    if (!trigger) return TOME_ERR_FAILED;
    uint32_t condition = g_condition(code);
    if (!condition || !g_trigger_add_condition(trigger, condition)) {
        g_destroy_trigger(trigger);
        return TOME_ERR_FAILED;
    }
    uint32_t value = g_trigger_evaluate(trigger);
    g_destroy_trigger(trigger);
    if (result) *result = value != 0;
    return TOME_OK;
}

// Runs `code` as the action of a trigger, like TriggerExecute: on a thread
// of its own that may wait, and this returns once it finishes or first
// waits. A waiting action needs its trigger to live on, so each code value
// gets one trigger for the rest of the map script, reused for every call
// (each TriggerExecute starts a new thread, so calls can overlap).
tome_result TOME_CALL api_jass_code_execute(tome_jass_code code) {
    if (tome_result status = check_code(code, "jass_code_execute"); status != TOME_OK) return status;
    uint32_t& trigger = g_action_triggers[code];
    if (!trigger) {
        uint32_t created = g_create_trigger();
        if (!created) return TOME_ERR_FAILED;
        if (!g_trigger_add_action(created, code)) {
            g_destroy_trigger(created);
            return TOME_ERR_FAILED;
        }
        trigger = created;
    }
    g_trigger_execute(trigger);
    return TOME_OK;
}

} // namespace

const char* string_text(const void* jass_string) {
    return jass_string && g_string_text ? g_string_text(jass_string) : nullptr;
}

unsigned map_load_count() {
    return g_map_loads;
}

bool install_hooks(uintptr_t game_base, const BuildMappings& build, std::string& report) {
    mods_ready::init();
    g_bind_native = reinterpret_cast<BindNativeFn>(game_base + build.jass.bind_native);
    g_init_natives = reinterpret_cast<InitNativesFn>(game_base + build.jass.init_natives);
    const auto& jass = build.jass;
    g_game_instance = reinterpret_cast<const void* const*>(game_base + build.game.game_instance);
    g_string_text = reinterpret_cast<StringTextFn>(game_base + jass.string_get_text);
    g_create_string = reinterpret_cast<CreateStringFn>(game_base + jass.create_string);
    g_create_trigger = reinterpret_cast<NativeFn0>(game_base + jass.create_trigger);
    g_destroy_trigger = reinterpret_cast<NativeFn1>(game_base + jass.destroy_trigger);
    g_condition = reinterpret_cast<NativeFn1>(game_base + jass.condition);
    g_trigger_add_condition = reinterpret_cast<NativeFn2>(game_base + jass.trigger_add_condition);
    g_trigger_evaluate = reinterpret_cast<NativeFn1>(game_base + jass.trigger_evaluate);
    g_trigger_add_action = reinterpret_cast<NativeFn2>(game_base + jass.trigger_add_action);
    g_trigger_execute = reinterpret_cast<NativeFn1>(game_base + jass.trigger_execute);

    int patched = 0;
    report.clear();
    for (uint32_t rva : build.jass.init_natives_calls) {
        std::string error;
        if (patch_call(game_base + rva, game_base + build.jass.init_natives,
                       reinterpret_cast<const void*>(&hooked_init_natives), error)) {
            ++patched;
        } else {
            report += (report.empty() ? "" : "; ") + error;
        }
    }
    if (patched == 0) {
        return false;
    }

    std::string error;
    declare("tome", "native TomeIsLoaded takes nothing returns boolean", error);
    attach("tome", "TomeIsLoaded", reinterpret_cast<void*>(&native_tome_is_loaded));
    declare("tome", "native TomeLogInteger takes integer value returns nothing", error);
    attach("tome", "TomeLogInteger", reinterpret_cast<void*>(&native_tome_log_integer));
    declare("tome", "native TomeLogString takes string value returns nothing", error);
    attach("tome", "TomeLogString", reinterpret_cast<void*>(&native_tome_log_string));

    char buf[64];
    snprintf(buf, sizeof(buf), "redirected %d of 3 InitNatives call sites", patched);
    report = buf + (report.empty() ? std::string() : " (" + report + ")");
    return true;
}

bool declare(const std::string& owner, const std::string& declaration, std::string& error) {
    jass::NativeDecl decl;
    if (!jass::parse_native(declaration, decl, error)) {
        return false;
    }
    std::string signature = decl.signature();

    ExclusiveLock hold(g_lock);
    bool ok = true;
    auto it = g_by_name.find(decl.name);
    if (it != g_by_name.end()) {
        Slot* slot = it->second;
        if (slot->owner != owner) {
            error = "native " + decl.name + " is already declared by \"" + slot->owner + "\"";
            ok = false;
        } else if (slot->signature != signature) {
            // A compiled map keeps calling with the old signature until the
            // next map load re-binds it.
            TOME_WARN("Native %s changed signature %s -> %s; takes effect on the next map load",
                      decl.name.c_str(), slot->signature.c_str(), signature.c_str());
            slot->signature = signature;
            // At worst, until then, a non-code argument counts as a code value
            // seen, and a guarded call passes the new number of arguments.
            slot->code_mask = decl.code_mask();
            slot->arg_count = static_cast<uint32_t>(decl.param_types.size());
        }
    } else {
        auto slot = std::make_unique<Slot>();
        slot->name = decl.name;
        slot->signature = signature;
        slot->owner = owner;
        slot->code_mask = decl.code_mask();
        slot->arg_count = static_cast<uint32_t>(decl.param_types.size());
        slot->thunk = make_thunk(slot.get(), owner != "tome");
        if (!slot->thunk) {
            error = "out of memory for native thunks";
            ok = false;
        } else {
            g_by_name[slot->name] = slot.get();
            g_slots.push_back(std::move(slot));
        }
    }
    return ok;
}

AttachResult attach(const std::string& owner, const std::string& name, void* fn, tome_mod* mod) {
    ExclusiveLock hold(g_lock);
    AttachResult result = AttachResult::Ok;
    auto it = g_by_name.find(name);
    if (it == g_by_name.end()) {
        result = AttachResult::NotDeclared;
    } else if (it->second->owner != owner) {
        result = AttachResult::NotOwner;
    } else {
        it->second->mod = mod;
        it->second->handler = nullptr;
        it->second->target = fn;
    }
    return result;
}

AttachResult attach_handler(const std::string& owner, const std::string& name, Handler* handler, tome_mod* mod,
                            const std::string& signature) {
    ExclusiveLock hold(g_lock);
    auto it = g_by_name.find(name);
    if (it == g_by_name.end()) return AttachResult::NotDeclared;
    if (it->second->owner != owner || owner == "tome") return AttachResult::NotOwner;
    it->second->mod = mod;
    it->second->handler = handler;
    it->second->handler_signature = signature;
    it->second->stale_logged = false;
    it->second->target = handler;  // non-null: the thunk calls call_guarded
    return AttachResult::Ok;
}

std::string signature_of(const std::string& name) {
    SharedLock hold(g_lock);
    auto it = g_by_name.find(name);
    return it == g_by_name.end() ? std::string() : it->second->signature;
}

void detach(const std::string& owner, const std::string& name) {
    ExclusiveLock hold(g_lock);
    auto it = g_by_name.find(name);
    if (it != g_by_name.end() && it->second->owner == owner) {
        it->second->target = nullptr;
        it->second->handler = nullptr;
        it->second->mod = nullptr;
    }
}

void fill_api(tome_api& api) {
    api.jass_string = api_jass_string;
    api.jass_string_create = api_jass_string_create;
    api.jass_code_call = api_jass_code_call;
    api.jass_code_execute = api_jass_code_execute;
}

std::vector<NativeInfo> list() {
    std::vector<NativeInfo> out;
    SharedLock hold(g_lock);
    for (const auto& slot : g_slots) {
        out.push_back({slot->name, slot->signature, slot->owner, slot->target != nullptr, slot->bind_count});
    }
    return out;
}

} // namespace tome::natives
