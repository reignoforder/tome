// SPDX-License-Identifier: GPL-3.0-or-later
#include "wasm_mods.h"

#include "api.h"
#include "core/intermod.h"
#include "core/path_guard.h"
#include "core/text.h"
#include "core/wasm_abi.h"
#include "core/wasm_alloc.h"
#include "core/wasm_imports.h"
#include "core/wasm_layout.h"
#include "log.h"
#include "natives.h"
#include "objects.h"
#include "paths.h"
#include "storm.h"
#include "guard.h"
#include "timers.h"

#include "wasm_export.h"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <unordered_map>
#include <vector>

// WAMR's log lines (BH_VPRINTF, loader/CMakeLists.txt), into tome.log.
extern "C" int tome_wamr_vprintf(const char* format, va_list ap) {
    char line[512] = "";
    int n = vsnprintf(line, sizeof(line), format, ap);
    if (n < 0) line[0] = '\0';  // the buffer's contents are unspecified then
    size_t len = std::strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
    if (len) TOME_WARN("WAMR: %s", line);
    return n;
}

namespace tome::wasm_mods {
namespace {

// Longest string an import takes (a command name, a setting key or value,
// a log line, a displayed text, a published message).
constexpr size_t kMaxString = 4096;
constexpr size_t kMaxMessage = 64 * 1024;
// Longest string a native may return.
constexpr size_t kMaxNativeString = 64 * 1024;
// Handles for units, items and destructables seen by one module before
// they're dropped (they're only for telling objects apart).
constexpr size_t kMaxObjectHandles = 100000;
// Largest game file read_game_file reads: no more than a module's memory
// can hold, and never a huge allocation in the game's 32-bit address space.
constexpr size_t kMaxGameFile = size_t(kMaxMemoryPages) * 65536;

// What a failed call leaves for the guard. Called only from frames that own
// nothing: the guard's unwind skips destructors.
[[noreturn]] void fail_call() {
    RaiseException(TOME_EXCEPTION_MOD_FAILED, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    __builtin_unreachable();
}

const tome_api* api() {
    return api_table();
}

} // namespace

struct Instance::Impl {
    tome_mod* mod = nullptr;
    std::vector<uint8_t> bytes;  // WAMR keeps pointers into the module's bytes
    wasm_module_t module = nullptr;
    wasm_module_inst_t inst = nullptr;
    wasm_exec_env_t env = nullptr;
    DWORD env_thread = 0;
    int depth = 0;                 // calls into the module on the stack right now
    ULONGLONG call_started = 0;    // GetTickCount64 when the outermost one began (kCallTimeLimitMs)
    bool takes_callbacks = false;  // exports tome_buffer and tome_callback
    bool takes_natives = false;    // exports tome_native (and tome_buffer)
    // The exports Tome calls back through, looked up once (nullptr if absent).
    wasm_function_inst_t fn_buffer = nullptr;
    wasm_function_inst_t fn_callback = nullptr;
    wasm_function_inst_t fn_native = nullptr;
    wasm_function_inst_t fn_revoked = nullptr;  // tome_callbacks_revoked
    // Disabled since the callbacks and natives were last forgotten: every
    // registration was revoked, so the next enable frees them (forget_revoked).
    bool revoked = false;

    struct Callback {
        Impl* impl;
        uint32_t id;
        uint32_t frame = 0;  // frame_on_click: the button's handle
    };
    // The `user` of each callback the module registered; they live as long
    // as the instance (revoked on disable, before it's destroyed).
    std::deque<Callback> callbacks;
    // Timers share one `user` (a module may start a timer every frame):
    // each running one's callback id is kept by timer id, until it fires
    // for the last time, is stopped or its game ends.
    Callback timer_cb{this, 0};
    std::map<uint32_t, uint32_t> timer_slots;

    // What a frame handle stands for: looked up again on every use, so a
    // frame that's gone is never touched (frames::frame_alive's rule).
    struct FrameRef {
        std::string name;
        int32_t context = 0;
        int32_t root_of = 0;  // a screen's root (ui_root) instead of a named frame
    };
    std::deque<FrameRef> frames;
    std::map<std::pair<std::string, int32_t>, FrameRef*> frames_by_name;
    std::map<int32_t, FrameRef*> roots;
    wasm::HandleTable handles;

    struct Native;
    std::deque<std::unique_ptr<Native>> natives;

    ~Impl();

    Callback* callback(uint32_t id) {
        callbacks.push_back({this, id});
        return &callbacks.back();
    }
    // A registration's result: the callback of one Tome refused is dropped
    // again (nothing holds it), so refusals don't pile up until re-enable.
    int32_t registered(const Callback* cb, int32_t result) {
        if (result != TOME_OK && !callbacks.empty() && &callbacks.back() == cb) callbacks.pop_back();
        return result;
    }

    // Before registering a callback: one the module can't receive is refused.
    bool can_take_callbacks(const char* import) const {
        if (takes_callbacks) return true;
        TOME_ERROR("[%s] %s: the module doesn't export tome_buffer and tome_callback, which callbacks need",
                   mod->id.c_str(), import);
        return false;
    }

    // Taken again for every access: memory.grow may move it.
    wasm::Memory memory() const {
        wasm::Memory m;
        wasm_memory_inst_t mem = wasm_runtime_get_default_memory(inst);
        if (!mem) return m;
        m.base = static_cast<uint8_t*>(wasm_memory_get_base_address(mem));
        m.size = uint64_t(wasm_memory_get_cur_page_count(mem)) * wasm_memory_get_bytes_per_page(mem);
        return m;
    }

    // --- handles ---

    uint32_t frame_handle(const std::string& name, int32_t context) {
        auto key = std::make_pair(name, context);
        auto it = frames_by_name.find(key);
        FrameRef* ref = nullptr;
        if (it != frames_by_name.end()) {
            ref = it->second;
        } else {
            frames.push_back({name, context, 0});
            ref = frames_by_name[key] = &frames.back();
        }
        return handles.add(wasm::HandleKind::Frame, ref);
    }
    uint32_t root_handle(int32_t event) {
        FrameRef*& ref = roots[event];
        if (!ref) {
            frames.push_back({"", 0, event});
            ref = &frames.back();
        }
        return handles.add(wasm::HandleKind::Frame, ref);
    }
    // The frame now, or nullptr if it's gone (or the handle isn't one).
    tome_frame* frame(uint32_t handle) const {
        auto* ref = static_cast<FrameRef*>(handles.get(handle, wasm::HandleKind::Frame));
        if (!ref) return nullptr;
        if (ref->root_of) return api()->ui_root(ref->root_of);
        return api()->frame_get(ref->name.c_str(), ref->context);
    }
    unsigned map_loads = 0;  // natives::map_load_count() when the object handles were last used
    wasm::Handles object_handles() {
        // A new map script (a map's load, or leaving a game) means new
        // objects, maybe at the same addresses: drop the old handles, whether
        // or not the module gets the map's events.
        if (const unsigned loads = natives::map_load_count(); loads != map_loads) {
            drop_object_handles();
            map_loads = loads;
        }
        return [this](wasm::HandleKind kind, const void* object) -> uint32_t {
            if (!object) return 0;
            if (handles.live() > kMaxObjectHandles) drop_object_handles();
            const uint32_t handle = handles.add(kind, const_cast<void*>(object));
            // The object is live now (an event's, the selection's): its agent
            // id says later whether it still is, without reading the object.
            // A new object at a freed one's address gets the same handle, so
            // the id is taken again every time.
            if (handle && kind != wasm::HandleKind::Player) agent_ids[handle] = objects::agent_id(object);
            return handle;
        };
    }
    // The agent id of each unit, item and destructable handle (object_handles).
    std::unordered_map<uint32_t, objects::AgentId> agent_ids;
    void drop_object_handles() {
        agent_ids.clear();
        handles.remove_all(wasm::HandleKind::Unit);
        handles.remove_all(wasm::HandleKind::Item);
        handles.remove_all(wasm::HandleKind::Destructable);
        handles.remove_all(wasm::HandleKind::Player);
    }

    // --- calls into the module ---

    // An exec env made on this thread: one made on another (a mod's first
    // load runs on the bootstrap thread) has that thread's stack bounds.
    bool ensure_env() {
        const DWORD thread = GetCurrentThreadId();
        if (env && env_thread == thread) return true;
        if (depth > 0) return false;  // a call into the module is running on another thread
        static thread_local bool thread_ready = false;
        if (!thread_ready) thread_ready = wasm_runtime_init_thread_env();
        if (env) wasm_runtime_destroy_exec_env(env);
        env = wasm_runtime_create_exec_env(inst, kStackSize);
        env_thread = env ? thread : 0;
        return env != nullptr;
    }

    // Past kCallTimeLimitMs in the outermost call: makes the module trap
    // as soon as the import or callback running now returns to it. True if
    // it's trapping (for this or an earlier reason).
    bool out_of_time() {
        if (wasm_runtime_get_exception(inst)) return true;
        if (depth == 0 || GetTickCount64() - call_started <= kCallTimeLimitMs) return false;
        wasm_runtime_set_exception(inst, "time limit exceeded (a call's imports and callbacks took too long)");
        return true;
    }

    // Calls export `name` with argv (its results in argv afterwards). False,
    // with the reason logged, if it trapped or couldn't be called.
    bool invoke(const char* name, uint32_t argc, uint32_t* argv) {
        return invoke(name, wasm_runtime_lookup_function(inst, name), argc, argv);
    }
    // The same with the export already looked up (`fn`, nullptr if absent).
    bool invoke(const char* name, wasm_function_inst_t fn, uint32_t argc, uint32_t* argv) {
        if (!fn) {
            TOME_ERROR("[%s] the module doesn't export %s", mod->id.c_str(), name);
            return false;
        }
        if (!ensure_env()) {
            TOME_ERROR("[%s] couldn't set up a call into the module", mod->id.c_str());
            return false;
        }
        if (depth == 0) {
            wasm_runtime_clear_exception(inst);
            wasm_runtime_set_instruction_count_limit(env, kInstructionBudget);
            call_started = GetTickCount64();
        } else if (out_of_time()) {
            // Nested in a call that's trapping, or ran out of time: the
            // outer call traps when this returns.
            return false;
        }
        ++depth;
        const bool ok = wasm_runtime_call_wasm(env, fn, argc, argv);
        --depth;
        if (!ok) {
            const char* why = wasm_runtime_get_exception(inst);
            TOME_ERROR("[%s] the module trapped in %s: %s", mod->id.c_str(), name, why ? why : "?");
        }
        return ok;
    }

    // Copies a block into a buffer the module gives (tome_buffer), encoded
    // for its address: `make(base)`. Its address, or 0 (logged) if that
    // failed.
    bool to_buffer(const std::function<std::string(uint32_t)>& make, uint32_t& ptr, uint32_t& len) {
        len = static_cast<uint32_t>(make(0).size());
        uint32_t argv[1] = {len};
        if (!invoke("tome_buffer", fn_buffer, 1, argv)) return false;
        ptr = argv[0];
        // 0 is how the SDKs' tome_buffer says it couldn't allocate: writing
        // there would overwrite the module's own data.
        if (ptr == 0 && len != 0) {
            TOME_ERROR("[%s] tome_buffer(%u) returned no buffer (0)", mod->id.c_str(), len);
            return false;
        }
        const std::string data = make(ptr);
        if (data.size() != len || wasm::write_out(memory(), ptr, len, data) != len) {
            TOME_ERROR("[%s] tome_buffer(%u) returned a buffer outside the module's memory", mod->id.c_str(), len);
            return false;
        }
        return true;
    }

    // After a disable revoked every registration, Tome can't call the old
    // callbacks and natives any more: frees them, and tells the module
    // (tome_callbacks_revoked) so it can free its side. Only with no call
    // into the module on the stack, where a trampoline may still hold one.
    // False if the module trapped.
    bool forget_revoked() {
        if (!revoked || depth > 0) return true;
        revoked = false;
        callbacks.clear();
        timer_slots.clear();
        natives.clear();
        uint32_t argv[1] = {0};
        return !fn_revoked || invoke("tome_callbacks_revoked", fn_revoked, 0, argv);
    }

    // A callback into the module: tome_callback(id, ptr, len) with the block.
    bool send(uint32_t id, const std::function<std::string(uint32_t)>& make) {
        uint32_t ptr = 0, len = 0;
        if (!to_buffer(make, ptr, len)) return false;
        uint32_t argv[3] = {id, ptr, len};
        return invoke("tome_callback", fn_callback, 3, argv);
    }
    // A callback that answers in its block: `size` zero bytes go in, and
    // what the module left there comes back in `out` (support/tome_wasm.c
    // writes a checksum's 8 bytes there).
    bool send_for_result(uint32_t id, uint32_t size, std::vector<uint8_t>& out) {
        uint32_t ptr = 0, len = 0;
        if (!to_buffer([&](uint32_t) { return std::string(size, '\0'); }, ptr, len)) return false;
        uint32_t argv[3] = {id, ptr, len};
        if (!invoke("tome_callback", fn_callback, 3, argv)) return false;
        if (!wasm::read_bytes(memory(), ptr, size, size, out) || out.size() != size) {
            TOME_ERROR("[%s] the module's answer to a callback is outside its memory", mod->id.c_str());
            return false;
        }
        return true;
    }
    bool send_text(uint32_t id, std::string_view text) {
        return send(id, [&](uint32_t) { return std::string(text); });
    }
    bool send_words(uint32_t id, std::initializer_list<uint32_t> words) {
        std::string block;
        for (uint32_t w : words) {
            for (int i = 0; i < 4; ++i) block += static_cast<char>(w >> (8 * i));
        }
        return send(id, [&](uint32_t) { return block; });
    }
};

// A Wasm mod's JASS native (natives::Handler): the arguments converted
// (reals read from their pointers, strings copied), tome_native(id, args,
// count) called, its result checked and converted back.
struct Instance::Impl::Native : natives::Handler {
    Impl* impl;
    uint32_t id;
    std::string name;
    wasm::JassSignature sig;

    Native(Impl* i, uint32_t n, std::string native, wasm::JassSignature s)
        : impl(i), id(n), name(std::move(native)), sig(std::move(s)) {}

    bool run(const uint32_t* args, uint32_t& result) {
        std::vector<wasm::NativeArg> values(sig.params.size());
        for (size_t i = 0; i < sig.params.size(); ++i) {
            switch (sig.params[i].kind) {
            case 'R': {
                const auto* real = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(args[i]));
                values[i].word = real ? *real : 0;
                break;
            }
            case 'S': {
                const char* text = natives::string_text(reinterpret_cast<const void*>(static_cast<uintptr_t>(args[i])));
                values[i].null = !text;
                if (text) values[i].text = text;
                break;
            }
            default: values[i].word = args[i];
            }
        }
        uint32_t ptr = 0, len = 0;
        if (!impl->to_buffer([&](uint32_t base) { return wasm::encode_native_args(sig, values, base); }, ptr, len))
            return false;
        uint32_t argv[3] = {id, ptr, static_cast<uint32_t>(sig.params.size())};
        if (!impl->invoke("tome_native", impl->fn_native, 3, argv)) return false;
        const uint32_t value = argv[0];
        switch (sig.result.kind) {
        case 'V': result = 0; break;
        case 'B': result = value != 0; break;  // JASS compares booleans as words: true is 1
        case 'S': {
            if (!value) {
                result = 0;
                break;
            }
            // A NUL-terminated string in the module's memory.
            const wasm::Memory m = impl->memory();
            uint32_t n = 0;
            while (wasm::in_bounds(m, value + n, 1) && n < kMaxNativeString && m.base[value + n]) ++n;
            std::string text;
            if (!wasm::in_bounds(m, value + n, 1) || n >= kMaxNativeString ||
                wasm::read_string(m, value, n, kMaxNativeString, text) != wasm::StringError::None) {
                TOME_ERROR("[%s] native %s: the string it returned isn't a NUL-terminated UTF-8 string in its memory",
                           impl->mod->id.c_str(), name.c_str());
                result = 0;
                break;
            }
            result = api()->jass_string_create(text.c_str());
            break;
        }
        default:
            if (!wasm::valid_native_result(sig, values, value)) {
                TOME_ERROR("[%s] native %s returned handle %u, which isn't one of its own %s arguments: returned null",
                           impl->mod->id.c_str(), name.c_str(), value, sig.result.handle.c_str());
                result = 0;
            } else {
                result = value;
            }
        }
        return true;
    }

    uint32_t call(const uint32_t* args) override {
        uint32_t result = 0;
        if (!run(args, result) && impl->depth == 0) fail_call();
        return result;
    }
};

Instance::Impl::~Impl() {
    if (env) wasm_runtime_destroy_exec_env(env);
    if (inst) wasm_runtime_deinstantiate(inst);
    if (module) wasm_runtime_unload(module);
}

namespace {

using Impl = Instance::Impl;
using Callback = Impl::Callback;

// Every import starts here: past the call's time limit, the module traps
// once this import returns (Impl::out_of_time).
Impl* impl_of(wasm_exec_env_t env) {
    Impl* impl = static_cast<Impl*>(wasm_runtime_get_custom_data(wasm_runtime_get_module_inst(env)));
    impl->out_of_time();
    return impl;
}

// A string argument; false (and a log line naming the import) if it isn't
// one. The import then does nothing.
bool arg_string(Impl* impl, const char* import, uint32_t ptr, uint32_t len, std::string& out,
                size_t max = kMaxString) {
    const wasm::StringError e = wasm::read_string(impl->memory(), ptr, len, max, out);
    if (e == wasm::StringError::None) return true;
    TOME_ERROR("[%s] %s: a string argument is %s", impl->mod->id.c_str(), import, wasm::describe(e));
    return false;
}

// A frame argument; nullptr (logged) if the handle isn't a frame or it's gone.
tome_frame* arg_frame(Impl* impl, const char* import, uint32_t handle) {
    tome_frame* f = impl->frame(handle);
    if (!f) TOME_ERROR("[%s] %s: frame handle %u isn't a frame that exists now", impl->mod->id.c_str(), import, handle);
    return f;
}

// An i32 out-parameter (may be 0: not wanted); false if it's out of bounds.
bool store(Impl* impl, uint32_t ptr, uint32_t value) {
    return ptr == 0 || wasm::store_u32(impl->memory(), ptr, value);
}

// --- trampolines: Tome's callbacks into the module ------------------------
// Each one owns nothing when it hands a trap to the guard (fail_call).

void TOME_CALL on_text(tome_mod*, const char* text, void* user) {
    auto* cb = static_cast<Callback*>(user);
    if (!cb->impl->send_text(cb->id, text ? text : "") && cb->impl->depth == 0) fail_call();
}

bool deliver_event(Callback* cb, const tome_event_info* info) {
    Impl* impl = cb->impl;
    if (info->event == TOME_EVENT_MAP_LOAD) impl->drop_object_handles();
    const wasm::Handles handles = impl->object_handles();
    const bool ok = impl->send(cb->id, [&](uint32_t base) { return wasm::encode_event(*info, handles, base); });
    if (info->event == TOME_EVENT_GAME_END) impl->drop_object_handles();
    return ok;
}

void TOME_CALL on_event(tome_mod*, const tome_event_info* info, void* user) {
    auto* cb = static_cast<Callback*>(user);
    if (!deliver_event(cb, info) && cb->impl->depth == 0) fail_call();
}

void TOME_CALL on_ui(tome_mod*, int32_t event, void* user) {
    auto* cb = static_cast<Callback*>(user);
    if (!cb->impl->send_words(cb->id, {static_cast<uint32_t>(event)}) && cb->impl->depth == 0) fail_call();
}

void TOME_CALL on_click(tome_mod*, tome_frame*, void* user) {
    auto* cb = static_cast<Callback*>(user);
    if (!cb->impl->send_words(cb->id, {cb->frame}) && cb->impl->depth == 0) fail_call();
}

void TOME_CALL on_popup(tome_mod*, uint32_t popup, int32_t choice, void* user) {
    auto* cb = static_cast<Callback*>(user);
    if (!cb->impl->send_words(cb->id, {popup, static_cast<uint32_t>(choice)}) && cb->impl->depth == 0) fail_call();
}

// A message: [topic length u32][topic][data].
bool deliver_message(Callback* cb, const char* topic, const void* data, size_t size) {
    const size_t topic_len = topic ? std::strlen(topic) : 0;
    std::string block(4, '\0');
    for (int i = 0; i < 4; ++i) block[i] = static_cast<char>(topic_len >> (8 * i));
    block.append(topic ? topic : "", topic_len);
    if (data && size) block.append(static_cast<const char*>(data), size);
    return cb->impl->send(cb->id, [&](uint32_t) { return block; });
}

void TOME_CALL on_message(tome_mod*, const char* topic, const void* data, size_t size, void* user) {
    auto* cb = static_cast<Callback*>(user);
    if (!deliver_message(cb, topic, data, size) && cb->impl->depth == 0) fail_call();
}

// A message between players: [player i32][replay i32][data]. Its own frame,
// so the block is freed before fail_call (whose unwind skips destructors).
bool deliver_net_message(Callback* cb, const tome_net_message* m) {
    std::string block(8, '\0');
    for (int i = 0; i < 4; ++i) {
        block[i] = static_cast<char>(static_cast<uint32_t>(m->player) >> (8 * i));
        block[4 + i] = static_cast<char>(static_cast<uint32_t>(m->replay) >> (8 * i));
    }
    if (m->data && m->data_size) block.append(static_cast<const char*>(m->data), m->data_size);
    return cb->impl->send(cb->id, [&](uint32_t) { return block; });
}

void TOME_CALL on_net_message_cb(tome_mod*, const tome_net_message* m, void* user) {
    auto* cb = static_cast<Callback*>(user);
    if (!deliver_net_message(cb, m) && cb->impl->depth == 0) fail_call();
}

// A timer: [timer id][state], state 0 if it fires again, 1 for its last
// time, 2 if it ended without firing (its game ended; the module only
// frees its record of it).
void TOME_CALL on_timer(tome_mod*, uint32_t timer, void* user) {
    Impl* impl = static_cast<Callback*>(user)->impl;
    auto it = impl->timer_slots.find(timer);
    if (it == impl->timer_slots.end()) return;
    const uint32_t slot = it->second;
    const bool last = !timers_api::running(timer);
    if (last) impl->timer_slots.erase(it);
    if (!impl->send_words(slot, {timer, last ? 1u : 0u}) && impl->depth == 0) fail_call();
}

void on_timer_ended(tome_mod* mod, uint32_t timer, tome_timer_fn fn, void* user) {
    if (fn != on_timer) return;
    Impl* impl = static_cast<Callback*>(user)->impl;
    auto it = impl->timer_slots.find(timer);
    if (it == impl->timer_slots.end()) return;
    const uint32_t slot = it->second;
    impl->timer_slots.erase(it);
    guard::run(mod, "timer callback", "(ended)", [&] {
        if (!impl->send_words(slot, {timer, 2u}) && impl->depth == 0) fail_call();
    });
}

// A checksum: 8 bytes the module overwrites with its hash. Its own frame,
// so the answer is freed before fail_call.
bool read_checksum(Callback* cb, uint64_t& hash) {
    std::vector<uint8_t> answer;
    if (!cb->impl->send_for_result(cb->id, 8, answer)) return false;
    hash = 0;
    for (int i = 0; i < 8; ++i) hash |= static_cast<uint64_t>(answer[i]) << (8 * i);
    return true;
}

uint64_t TOME_CALL on_checksum(tome_mod*, void* user) {
    auto* cb = static_cast<Callback*>(user);
    uint64_t hash = 0;
    if (!read_checksum(cb, hash) && cb->impl->depth == 0) fail_call();
    return hash;
}

// --- imports (module "tome") ------------------------------------------------
// Results are tome_result values unless they say otherwise. Out-parameters
// (ptr, capacity) return the full length (UINT32_MAX for a buffer outside
// the module's memory), like the C API's.

void w_log(wasm_exec_env_t env, int32_t level, uint32_t ptr, uint32_t len) {
    Impl* impl = impl_of(env);
    std::string text;
    if (!arg_string(impl, "log", ptr, len, text)) return;
    const tome_log_level l = level <= TOME_LOG_DEBUG   ? TOME_LOG_DEBUG
                             : level >= TOME_LOG_ERROR ? TOME_LOG_ERROR
                                                       : static_cast<tome_log_level>(level);
    api()->log(impl->mod, l, text.c_str());
}

uint32_t w_mod_id(wasm_exec_env_t env, uint32_t ptr, uint32_t capacity) {
    Impl* impl = impl_of(env);
    return wasm::write_out(impl->memory(), ptr, capacity, impl->mod->id);
}

uint32_t w_mod_version(wasm_exec_env_t env, uint32_t ptr, uint32_t capacity) {
    Impl* impl = impl_of(env);
    return wasm::write_out(impl->memory(), ptr, capacity, impl->mod->version);
}

// tome_wasm_game_info, encoded for `ptr`.
uint32_t w_game_info(wasm_exec_env_t env, uint32_t ptr, uint32_t capacity) {
    Impl* impl = impl_of(env);
    const tome_game_info* info = api()->game_info();
    return wasm::write_out(impl->memory(), ptr, capacity,
                           wasm::encode_game_info(info->build_name, info->file_version, info->edition, ptr));
}

int32_t w_register_command(wasm_exec_env_t env, uint32_t ptr, uint32_t len, uint32_t callback) {
    Impl* impl = impl_of(env);
    std::string name;
    if (!impl->can_take_callbacks("register_command") || !arg_string(impl, "register_command", ptr, len, name))
        return TOME_ERR_INVALID_ARGUMENT;
    Callback* cb = impl->callback(callback);
    return impl->registered(cb, api()->register_command(impl->mod, name.c_str(), on_text, cb));
}

int32_t w_display_text(wasm_exec_env_t env, uint32_t ptr, uint32_t len, float seconds) {
    Impl* impl = impl_of(env);
    std::string text;
    if (!arg_string(impl, "display_text", ptr, len, text)) return TOME_ERR_INVALID_ARGUMENT;
    return api()->display_text(text.c_str(), seconds);
}

int32_t w_setting_bool(wasm_exec_env_t env, uint32_t ptr, uint32_t len, int32_t fallback) {
    Impl* impl = impl_of(env);
    std::string key;
    if (!arg_string(impl, "setting_bool", ptr, len, key)) return fallback;
    return api()->setting_bool(impl->mod, key.c_str(), fallback);
}

int32_t w_setting_int(wasm_exec_env_t env, uint32_t ptr, uint32_t len, int32_t fallback) {
    Impl* impl = impl_of(env);
    std::string key;
    if (!arg_string(impl, "setting_int", ptr, len, key)) return fallback;
    return api()->setting_int(impl->mod, key.c_str(), fallback);
}

double w_setting_number(wasm_exec_env_t env, uint32_t ptr, uint32_t len, double fallback) {
    Impl* impl = impl_of(env);
    std::string key;
    if (!arg_string(impl, "setting_number", ptr, len, key)) return fallback;
    return api()->setting_number(impl->mod, key.c_str(), fallback);
}

uint32_t w_setting_string(wasm_exec_env_t env, uint32_t ptr, uint32_t len, uint32_t out, uint32_t capacity) {
    Impl* impl = impl_of(env);
    std::string key;
    if (!arg_string(impl, "setting_string", ptr, len, key)) return UINT32_MAX;
    const size_t size = api()->setting_string(impl->mod, key.c_str(), nullptr, 0);
    std::string value(size + 1, '\0');
    api()->setting_string(impl->mod, key.c_str(), value.data(), value.size());
    value.resize(size);
    return wasm::write_out(impl->memory(), out, capacity, value);
}

int32_t w_set_setting_bool(wasm_exec_env_t env, uint32_t ptr, uint32_t len, int32_t value) {
    Impl* impl = impl_of(env);
    std::string key;
    if (!arg_string(impl, "set_setting_bool", ptr, len, key)) return TOME_ERR_INVALID_ARGUMENT;
    return api()->set_setting_bool(impl->mod, key.c_str(), value);
}

int32_t w_set_setting_int(wasm_exec_env_t env, uint32_t ptr, uint32_t len, int32_t value) {
    Impl* impl = impl_of(env);
    std::string key;
    if (!arg_string(impl, "set_setting_int", ptr, len, key)) return TOME_ERR_INVALID_ARGUMENT;
    return api()->set_setting_int(impl->mod, key.c_str(), value);
}

int32_t w_set_setting_number(wasm_exec_env_t env, uint32_t ptr, uint32_t len, double value) {
    Impl* impl = impl_of(env);
    std::string key;
    if (!arg_string(impl, "set_setting_number", ptr, len, key)) return TOME_ERR_INVALID_ARGUMENT;
    return api()->set_setting_number(impl->mod, key.c_str(), value);
}

int32_t w_set_setting_string(wasm_exec_env_t env, uint32_t ptr, uint32_t len, uint32_t vptr, uint32_t vlen) {
    Impl* impl = impl_of(env);
    std::string key, value;
    if (!arg_string(impl, "set_setting_string", ptr, len, key) ||
        !arg_string(impl, "set_setting_string", vptr, vlen, value)) {
        return TOME_ERR_INVALID_ARGUMENT;
    }
    return api()->set_setting_string(impl->mod, key.c_str(), value.c_str());
}

int32_t w_on_setting_changed(wasm_exec_env_t env, uint32_t callback) {
    Impl* impl = impl_of(env);
    if (!impl->can_take_callbacks("on_setting_changed")) return TOME_ERR_INVALID_ARGUMENT;
    Callback* cb = impl->callback(callback);
    return impl->registered(cb, api()->on_setting_changed(impl->mod, on_text, cb));
}

// Events and screens.

int32_t w_on_event(wasm_exec_env_t env, int32_t event, uint32_t callback) {
    Impl* impl = impl_of(env);
    if (!impl->can_take_callbacks("on_event")) return TOME_ERR_INVALID_ARGUMENT;
    Callback* cb = impl->callback(callback);
    return impl->registered(cb, api()->on_event(impl->mod, event, on_event, cb));
}

int32_t w_on_ui(wasm_exec_env_t env, int32_t event, uint32_t callback) {
    Impl* impl = impl_of(env);
    if (!impl->can_take_callbacks("on_ui")) return TOME_ERR_INVALID_ARGUMENT;
    Callback* cb = impl->callback(callback);
    return impl->registered(cb, api()->on_ui(impl->mod, event, on_ui, cb));
}

// Frames: handles (Impl::FrameRef), 0 for none.

uint32_t w_ui_root(wasm_exec_env_t env, int32_t event) {
    Impl* impl = impl_of(env);
    return api()->ui_root(event) ? impl->root_handle(event) : 0;
}

uint32_t w_frame_get(wasm_exec_env_t env, uint32_t ptr, uint32_t len, int32_t context) {
    Impl* impl = impl_of(env);
    std::string name;
    if (!arg_string(impl, "frame_get", ptr, len, name)) return 0;
    return api()->frame_get(name.c_str(), context) ? impl->frame_handle(name, context) : 0;
}

uint32_t w_frame_create(wasm_exec_env_t env, uint32_t ptr, uint32_t len, uint32_t parent, int32_t priority,
                        int32_t context) {
    Impl* impl = impl_of(env);
    std::string name;
    if (!arg_string(impl, "frame_create", ptr, len, name)) return 0;
    tome_frame* p = arg_frame(impl, "frame_create", parent);
    if (!p) return 0;
    return api()->frame_create(impl->mod, name.c_str(), p, priority, context) ? impl->frame_handle(name, context) : 0;
}

int32_t w_frame_set_point(wasm_exec_env_t env, uint32_t frame, int32_t point, uint32_t relative,
                          int32_t relative_point, float x, float y) {
    Impl* impl = impl_of(env);
    tome_frame* f = arg_frame(impl, "frame_set_point", frame);
    tome_frame* r = f ? arg_frame(impl, "frame_set_point", relative) : nullptr;
    if (!f || !r) return TOME_ERR_INVALID_ARGUMENT;
    return api()->frame_set_point(f, point, r, relative_point, x, y);
}

int32_t w_frame_set_text(wasm_exec_env_t env, uint32_t frame, uint32_t ptr, uint32_t len) {
    Impl* impl = impl_of(env);
    std::string text;
    tome_frame* f = arg_frame(impl, "frame_set_text", frame);
    if (!f || !arg_string(impl, "frame_set_text", ptr, len, text)) return TOME_ERR_INVALID_ARGUMENT;
    return api()->frame_set_text(f, text.c_str());
}

int32_t w_frame_set_visible(wasm_exec_env_t env, uint32_t frame, int32_t visible) {
    Impl* impl = impl_of(env);
    tome_frame* f = arg_frame(impl, "frame_set_visible", frame);
    if (!f) return TOME_ERR_INVALID_ARGUMENT;
    return api()->frame_set_visible(f, visible);
}

int32_t w_frame_set_animation(wasm_exec_env_t env, uint32_t frame, uint32_t tags, int32_t count) {
    Impl* impl = impl_of(env);
    tome_frame* f = arg_frame(impl, "frame_set_animation", frame);
    std::vector<uint8_t> bytes;
    if (!f || count < 0 || count > 16 ||
        !wasm::read_bytes(impl->memory(), tags, static_cast<uint32_t>(count) * 4, 64, bytes)) {
        return TOME_ERR_INVALID_ARGUMENT;
    }
    int32_t values[16];
    std::memcpy(values, bytes.data(), bytes.size());
    return api()->frame_set_animation(f, values, count);
}

int32_t w_frame_on_click(wasm_exec_env_t env, uint32_t frame, uint32_t callback) {
    Impl* impl = impl_of(env);
    tome_frame* f = arg_frame(impl, "frame_on_click", frame);
    if (!f || !impl->can_take_callbacks("frame_on_click")) return TOME_ERR_INVALID_ARGUMENT;
    Callback* cb = impl->callback(callback);
    cb->frame = frame;
    return impl->registered(cb, api()->frame_on_click(impl->mod, f, on_click, cb));
}

// A path inside the mod's folder (the API checks it too).
int32_t w_load_fdf(wasm_exec_env_t env, uint32_t ptr, uint32_t len) {
    Impl* impl = impl_of(env);
    std::string path;
    if (!arg_string(impl, "load_fdf", ptr, len, path)) return TOME_ERR_INVALID_ARGUMENT;
    return api()->load_fdf(impl->mod, path.c_str());
}

// A game file's name: with the game's "Allow Local Files" on, Storm reads
// loose files from the disk first, so a name must stay under the game folder
// (the path sanitizer's rule: no "..", drive, root or device).
bool game_file_name(Impl* impl, const char* import, uint32_t ptr, uint32_t len, std::string& name) {
    if (!arg_string(impl, import, ptr, len, name)) return false;
    if (!path_guard::is_unsafe(name)) return true;
    TOME_ERROR("[%s] %s(%s): refused, the name leaves the game's files", impl->mod->id.c_str(), import, name.c_str());
    return false;
}

int32_t w_load_toc(wasm_exec_env_t env, uint32_t ptr, uint32_t len) {
    Impl* impl = impl_of(env);
    std::string path;
    if (!game_file_name(impl, "load_toc", ptr, len, path)) return TOME_ERR_INVALID_ARGUMENT;
    return api()->load_toc(path.c_str());
}

// Keybinds, popups, restarting.

int32_t w_on_keybind(wasm_exec_env_t env, uint32_t ptr, uint32_t len, uint32_t callback) {
    Impl* impl = impl_of(env);
    std::string id;
    if (!impl->can_take_callbacks("on_keybind") || !arg_string(impl, "on_keybind", ptr, len, id))
        return TOME_ERR_INVALID_ARGUMENT;
    Callback* cb = impl->callback(callback);
    return impl->registered(cb, api()->on_keybind(impl->mod, id.c_str(), on_text, cb));
}

uint32_t w_keybind_name(wasm_exec_env_t env, uint32_t ptr, uint32_t len, uint32_t out, uint32_t capacity) {
    Impl* impl = impl_of(env);
    std::string id;
    if (!arg_string(impl, "keybind_name", ptr, len, id)) return UINT32_MAX;
    const size_t size = api()->keybind_name(impl->mod, id.c_str(), nullptr, 0);
    std::string value(size + 1, '\0');
    api()->keybind_name(impl->mod, id.c_str(), value.data(), value.size());
    value.resize(size);
    return wasm::write_out(impl->memory(), out, capacity, value);
}

// tome_wasm_popup: title (ptr, len), text (ptr, len), options (an array of
// (ptr, len)), option_count, default_option, cancel_option: 9 i32.
int32_t w_popup(wasm_exec_env_t env, uint32_t spec, uint32_t callback, uint32_t id_out) {
    Impl* impl = impl_of(env);
    const wasm::Memory m = impl->memory();
    uint32_t f[9];
    for (uint32_t i = 0; i < 9; ++i) {
        if (!wasm::load_u32(m, spec + 4 * i, f[i])) return TOME_ERR_INVALID_ARGUMENT;
    }
    std::string title, text;
    if (!arg_string(impl, "popup", f[0], f[1], title) || !arg_string(impl, "popup", f[2], f[3], text))
        return TOME_ERR_INVALID_ARGUMENT;
    const uint32_t count = f[5];
    if (count == 0 || count > 32) return TOME_ERR_INVALID_ARGUMENT;
    std::vector<std::string> options(count);
    std::vector<const char*> labels(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t p = 0, l = 0;
        if (!wasm::load_u32(m, f[4] + 8 * i, p) || !wasm::load_u32(m, f[4] + 8 * i + 4, l) ||
            !arg_string(impl, "popup", p, l, options[i])) {
            return TOME_ERR_INVALID_ARGUMENT;
        }
        labels[i] = options[i].c_str();
    }
    if (callback && !impl->can_take_callbacks("popup")) return TOME_ERR_INVALID_ARGUMENT;
    // Checked before the popup shows: once it has, its callback is
    // registered, and a failure the module sees would free that id for reuse.
    if (!store(impl, id_out, 0)) return TOME_ERR_INVALID_ARGUMENT;
    tome_popup popup{};
    popup.size = sizeof(popup);
    popup.title = title.c_str();
    popup.text = text.c_str();
    popup.options = labels.data();
    popup.option_count = static_cast<int32_t>(count);
    popup.default_option = static_cast<int32_t>(f[6]);
    popup.cancel_option = static_cast<int32_t>(f[7]);
    uint32_t id = 0;
    Callback* cb = callback ? impl->callback(callback) : nullptr;
    const tome_result r = api()->popup(impl->mod, &popup, callback ? on_popup : nullptr, cb, &id);
    if (cb) impl->registered(cb, r);
    if (r == TOME_OK) store(impl, id_out, id);  // checked above (memory only grows)
    return r;
}

int32_t w_popup_close(wasm_exec_env_t env, uint32_t id) {
    return api()->popup_close(impl_of(env)->mod, id);
}

int32_t w_restart_game(wasm_exec_env_t env) {
    return api()->restart_game(impl_of(env)->mod);
}

// Units: an encoded array of tome_wasm_unit (names after it), if it fits;
// the size it needs. *count gets how many are selected, *leader the active one.
uint32_t w_selection(wasm_exec_env_t env, uint32_t out, uint32_t capacity, uint32_t count_out, uint32_t leader_out) {
    Impl* impl = impl_of(env);
    tome_unit units[12];
    int32_t leader = -1;
    for (tome_unit& u : units) u.size = sizeof(u);
    const int32_t n = api()->selection(units, sizeof(tome_unit), 12, &leader);
    const uint32_t count = static_cast<uint32_t>(std::clamp(n, 0, 12));
    if (!store(impl, count_out, count) || !store(impl, leader_out, static_cast<uint32_t>(leader))) return UINT32_MAX;
    return wasm::write_out(impl->memory(), out, capacity,
                           wasm::encode_units(units, count, impl->object_handles(), out));
}

// Messages between mods (bytes; services, which are function tables, aren't
// available to Wasm mods).

// A topic: the services' name rules, said when they're broken (the API is
// silent about it).
bool topic_name(Impl* impl, const char* import, uint32_t ptr, uint32_t len, std::string& topic) {
    if (!arg_string(impl, import, ptr, len, topic)) return false;
    if (intermod::valid_name(topic)) return true;
    TOME_ERROR("[%s] %s(%s): a topic is 1-64 of a-z 0-9 . - _", impl->mod->id.c_str(), import, topic.c_str());
    return false;
}

int32_t w_subscribe(wasm_exec_env_t env, uint32_t ptr, uint32_t len, uint32_t callback) {
    Impl* impl = impl_of(env);
    std::string topic;
    if (!impl->can_take_callbacks("subscribe") || !topic_name(impl, "subscribe", ptr, len, topic))
        return TOME_ERR_INVALID_ARGUMENT;
    Callback* cb = impl->callback(callback);
    return impl->registered(cb, api()->subscribe(impl->mod, topic.c_str(), on_message, cb));
}

int32_t w_publish(wasm_exec_env_t env, uint32_t ptr, uint32_t len, uint32_t data, uint32_t size) {
    Impl* impl = impl_of(env);
    std::string topic;
    std::vector<uint8_t> bytes;
    if (!topic_name(impl, "publish", ptr, len, topic)) return -1;
    if (!wasm::read_bytes(impl->memory(), data, size, kMaxMessage, bytes)) {
        TOME_ERROR("[%s] publish: the message is outside the module's memory or larger than %zu bytes",
                   impl->mod->id.c_str(), kMaxMessage);
        return -1;
    }
    return api()->publish(impl->mod, topic.c_str(), bytes.data(), bytes.size());
}

// The map script and the game's files.

uint32_t w_map_script_shadows(wasm_exec_env_t env, uint32_t out, uint32_t capacity, uint32_t count_out) {
    Impl* impl = impl_of(env);
    const int32_t total = api()->map_script_shadows(nullptr, sizeof(tome_script_shadow), 0);
    std::vector<tome_script_shadow> shadows(static_cast<size_t>(std::clamp(total, 0, 4096)));
    for (tome_script_shadow& s : shadows) s.size = sizeof(s);
    const int32_t n = shadows.empty() ? 0
                                      : api()->map_script_shadows(shadows.data(), sizeof(tome_script_shadow),
                                                                  static_cast<int32_t>(shadows.size()));
    const uint32_t count = static_cast<uint32_t>(std::clamp<int32_t>(n, 0, static_cast<int32_t>(shadows.size())));
    if (!store(impl, count_out, count)) return UINT32_MAX;
    return wasm::write_out(impl->memory(), out, capacity, wasm::encode_shadows(shadows.data(), count, out));
}

// Whether `name` is in Tome's own folder: tome/ is in the game folder unless
// TOME_HOME moves it, so with "Allow Local Files" Storm would read
// state.json, the log and every mod's config files there (config_read is a
// mod's only way into its own). Win32 drops a component's trailing dots and
// spaces ("tome." is "tome"), and "." components go nowhere.
bool in_tome_folder(std::string_view name) {
    while (!name.empty()) {
        const size_t end = name.find_first_of("\\/");
        std::string_view part = name.substr(0, end);
        while (!part.empty() && (part.back() == '.' || part.back() == ' ')) part.remove_suffix(1);
        if (!part.empty()) return text::iequals(part, "tome");
        if (end == std::string_view::npos) break;
        name.remove_prefix(end + 1);
    }
    return false;
}

uint32_t w_read_game_file(wasm_exec_env_t env, uint32_t ptr, uint32_t len, uint32_t out, uint32_t capacity) {
    Impl* impl = impl_of(env);
    std::string name;
    if (!game_file_name(impl, "read_game_file", ptr, len, name)) return UINT32_MAX;
    if (in_tome_folder(name)) {
        TOME_ERROR("[%s] read_game_file(%s): refused, that's Tome's own folder", impl->mod->id.c_str(), name.c_str());
        return UINT32_MAX;
    }
    if (!wasm::in_bounds(impl->memory(), out, capacity)) return UINT32_MAX;
    // Read once (read_game_file reads the whole file for its size too);
    // no such file is 0, like the API's.
    std::string data;
    bool too_large = false;
    if (!storm::init() || !storm::read_file(name.c_str(), data, kMaxGameFile, &too_large)) {
        if (too_large) {
            TOME_ERROR("[%s] read_game_file(%s): larger than %zu MiB", impl->mod->id.c_str(), name.c_str(),
                       kMaxGameFile >> 20);
            return UINT32_MAX;
        }
        data.clear();
    }
    return wasm::write_out(impl->memory(), out, capacity, data);
}

// The mod's own files, in tome/mods/config/<id>/ (instead of config_dir,
// a path): plain names only (wasm::valid_file_name), up to 1 MiB each.

bool config_path(Impl* impl, const char* import, uint32_t ptr, uint32_t len, std::wstring& path) {
    std::string name;
    if (!arg_string(impl, import, ptr, len, name, 64)) return false;
    if (!wasm::valid_file_name(name)) {
        TOME_ERROR("[%s] %s(%s): use a plain file name (letters, digits, . _ -)", impl->mod->id.c_str(), import,
                   name.c_str());
        return false;
    }
    path = impl->mod->config_dir_w + L"\\" + std::wstring(name.begin(), name.end());
    return true;
}

// The file's size (copied into `out` as far as it fits); UINT32_MAX if there's none.
uint32_t w_config_read(wasm_exec_env_t env, uint32_t ptr, uint32_t len, uint32_t out, uint32_t capacity) {
    Impl* impl = impl_of(env);
    std::wstring path;
    if (!config_path(impl, "config_read", ptr, len, path)) return UINT32_MAX;
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        return UINT32_MAX;
    }
    if (info.nFileSizeHigh || info.nFileSizeLow > wasm::kMaxFileSize) {
        TOME_ERROR("[%s] config_read: the file is larger than 1 MiB", impl->mod->id.c_str());
        return UINT32_MAX;
    }
    std::string data;
    if (!read_file(path, data)) return UINT32_MAX;
    return wasm::write_out(impl->memory(), out, capacity, data);
}

int32_t w_config_write(wasm_exec_env_t env, uint32_t ptr, uint32_t len, uint32_t data, uint32_t size) {
    Impl* impl = impl_of(env);
    std::wstring path;
    std::vector<uint8_t> bytes;
    if (!config_path(impl, "config_write", ptr, len, path)) return TOME_ERR_INVALID_ARGUMENT;
    if (!wasm::read_bytes(impl->memory(), data, size, wasm::kMaxFileSize, bytes)) {
        TOME_ERROR("[%s] config_write: the data is outside the module's memory or larger than 1 MiB",
                   impl->mod->id.c_str());
        return TOME_ERR_INVALID_ARGUMENT;
    }
    CreateDirectoryW(impl->mod->config_dir_w.c_str(), nullptr);
    return write_file_atomic(path, std::string(bytes.begin(), bytes.end())) ? TOME_OK : TOME_ERR_FAILED;
}

// JASS natives and code.

int32_t w_register_native(wasm_exec_env_t env, uint32_t ptr, uint32_t len, uint32_t id) {
    Impl* impl = impl_of(env);
    std::string name;
    if (!arg_string(impl, "register_native", ptr, len, name)) return TOME_ERR_INVALID_ARGUMENT;
    if (!impl->takes_natives) {
        TOME_ERROR("[%s] register_native(%s): the module doesn't export tome_native and tome_buffer",
                   impl->mod->id.c_str(), name.c_str());
        return TOME_ERR_INVALID_ARGUMENT;
    }
    const std::string signature = natives::signature_of(name);
    wasm::JassSignature sig;
    if (!wasm::parse_signature(signature, sig)) {
        TOME_ERROR("[%s] register_native(%s): not declared in tome.mod.json \"natives\"", impl->mod->id.c_str(),
                   name.c_str());
        return TOME_ERR_NOT_DECLARED;
    }
    if (sig.result.kind == 'C') {
        TOME_ERROR("[%s] register_native(%s): natives returning code aren't available to WebAssembly mods",
                   impl->mod->id.c_str(), name.c_str());
        return TOME_ERR_INVALID_ARGUMENT;
    }
    impl->natives.push_back(std::make_unique<Impl::Native>(impl, id, name, sig));
    Impl::Native* native = impl->natives.back().get();
    switch (natives::attach_handler(impl->mod->id, name, native, impl->mod, signature)) {
    case natives::AttachResult::Ok: break;
    case natives::AttachResult::NotDeclared:
        TOME_ERROR("[%s] register_native(%s): not declared in tome.mod.json \"natives\"", impl->mod->id.c_str(),
                   name.c_str());
        return TOME_ERR_NOT_DECLARED;
    case natives::AttachResult::NotOwner:
        TOME_ERROR("[%s] register_native(%s): declared by another mod", impl->mod->id.c_str(), name.c_str());
        return TOME_ERR_NOT_OWNER;
    }
    const std::string owner = impl->mod->id;
    impl->mod->add_revoker([owner, name] { natives::detach(owner, name); });
    return TOME_OK;
}

int32_t w_jass_code_call(wasm_exec_env_t env, uint32_t code, uint32_t result_out) {
    Impl* impl = impl_of(env);
    if (!store(impl, result_out, 0)) return TOME_ERR_INVALID_ARGUMENT;
    int32_t result = 0;
    const tome_result r = api()->jass_code_call(code, &result);
    store(impl, result_out, static_cast<uint32_t>(result));  // memory may have moved: store checks again
    return r;
}

int32_t w_jass_code_execute(wasm_exec_env_t env, uint32_t code) {
    impl_of(env);  // the call's time limit
    return api()->jass_code_execute(code);
}

// Game objects (API version 16). Units, items, destructables and players
// are the module's own handles; Tome checks the object again before every
// use (object_type: still a live agent, or one of the game's players), as
// it does for DLL mods. jass_call (below) takes JASS handle ids, which
// Tome checks against the handle table as for DLL mods (docs/wasm-mods.md).

constexpr wasm::HandleKind kObjectKinds[] = {wasm::HandleKind::Unit, wasm::HandleKind::Item,
                                             wasm::HandleKind::Destructable, wasm::HandleKind::Player};

// The object behind a handle the module has (of any of those kinds), or
// null. A unit, item or destructable only while the agent it was is still
// live (checked in the agent table, so a removed one's memory is never
// read); a player is only ever compared with the game's players.
void* object_from(Impl* impl, uint32_t handle) {
    impl->object_handles();  // first drops the handles of an older map script
    for (wasm::HandleKind kind : kObjectKinds) {
        void* object = impl->handles.get(handle, kind);
        if (!object) continue;
        // A player only while it's one of the game's players (compared, never read).
        if (kind == wasm::HandleKind::Player) return objects::is_player(object) ? object : nullptr;
        auto id = impl->agent_ids.find(handle);
        return id != impl->agent_ids.end() && objects::agent_id_alive(id->second) ? object : nullptr;
    }
    return nullptr;
}

// The module's handle for an object of a tome_object_type (0 for another type).
uint32_t object_handle(Impl* impl, void* object, int32_t type) {
    if (type < TOME_OBJECT_UNIT || type > TOME_OBJECT_PLAYER) return 0;
    return impl->object_handles()(kObjectKinds[type - TOME_OBJECT_UNIT], object);
}

int32_t w_object_type(wasm_exec_env_t env, uint32_t handle) {
    return api()->object_type(object_from(impl_of(env), handle));
}

uint32_t w_object_of(wasm_exec_env_t env, uint32_t jass_handle, uint32_t type_out) {
    Impl* impl = impl_of(env);
    int32_t type = TOME_OBJECT_NONE;
    void* object = api()->object_of(jass_handle, &type);
    const uint32_t handle = object_handle(impl, object, type);
    if (!store(impl, type_out, static_cast<uint32_t>(type))) return 0;
    return handle;
}

uint32_t w_handle_of(wasm_exec_env_t env, uint32_t handle) {
    return api()->handle_of(object_from(impl_of(env), handle));
}

uint32_t w_player_object(wasm_exec_env_t env, int32_t slot) {
    Impl* impl = impl_of(env);
    return object_handle(impl, api()->player_object(slot), TOME_OBJECT_PLAYER);
}

// A tome_wasm_unit (its name after it), if it fits; the size it needs, or
// UINT32_MAX if the handle isn't a live unit.
uint32_t w_unit_info(wasm_exec_env_t env, uint32_t handle, uint32_t out, uint32_t capacity) {
    Impl* impl = impl_of(env);
    tome_unit unit;
    if (api()->unit_info(object_from(impl, handle), &unit, sizeof(unit)) != TOME_OK) return UINT32_MAX;
    return wasm::write_out(impl->memory(), out, capacity, wasm::encode_units(&unit, 1, impl->object_handles(), out));
}

int32_t w_get_keyed_property(wasm_exec_env_t env, uint32_t handle, int32_t property, uint32_t key,
                             uint32_t value_out) {
    Impl* impl = impl_of(env);
    if (!wasm::in_bounds(impl->memory(), value_out, sizeof(double)) || !value_out) return TOME_ERR_INVALID_ARGUMENT;
    double value = 0;
    const tome_result r = api()->get_keyed_property(object_from(impl, handle), property, key, &value);
    const wasm::Memory m = impl->memory();  // taken again after the call
    if (!wasm::in_bounds(m, value_out, sizeof(double))) return TOME_ERR_INVALID_ARGUMENT;
    std::memcpy(m.base + value_out, &value, sizeof(value));
    return r;
}

int32_t w_get_property(wasm_exec_env_t env, uint32_t handle, int32_t property, uint32_t value_out) {
    return w_get_keyed_property(env, handle, property, 0, value_out);
}

int32_t w_set_keyed_property(wasm_exec_env_t env, uint32_t handle, int32_t property, uint32_t key, double value) {
    Impl* impl = impl_of(env);
    return api()->set_keyed_property(impl->mod, object_from(impl, handle), property, key, value);
}

int32_t w_set_property(wasm_exec_env_t env, uint32_t handle, int32_t property, double value) {
    return w_set_keyed_property(env, handle, property, 0, value);
}

int32_t w_host_player(wasm_exec_env_t) {
    return api()->host_player();
}

int32_t w_net_send(wasm_exec_env_t env, uint32_t data, uint32_t size) {
    Impl* impl = impl_of(env);
    std::vector<uint8_t> bytes;
    if (!wasm::read_bytes(impl->memory(), data, size, TOME_NET_MAX_MESSAGE, bytes)) {
        TOME_ERROR("[%s] net_send: the message is outside the module's memory or larger than %d bytes",
                   impl->mod->id.c_str(), TOME_NET_MAX_MESSAGE);
        return TOME_ERR_INVALID_ARGUMENT;
    }
    return api()->net_send(impl->mod, bytes.data(), static_cast<uint32_t>(bytes.size()));
}

int32_t w_on_net_message(wasm_exec_env_t env, uint32_t callback) {
    Impl* impl = impl_of(env);
    if (!impl->can_take_callbacks("on_net_message")) return TOME_ERR_INVALID_ARGUMENT;
    Callback* cb = impl->callback(callback);
    return impl->registered(cb, api()->on_net_message(impl->mod, on_net_message_cb, cb));
}

// Timers and desync checks.

int32_t w_timer_start(wasm_exec_env_t env, int32_t clock, double delay, double interval, uint32_t callback,
                      uint32_t out) {
    Impl* impl = impl_of(env);
    if (!impl->can_take_callbacks("timer_start")) return TOME_ERR_INVALID_ARGUMENT;
    if (out && !wasm::in_bounds(impl->memory(), out, 4)) {
        TOME_ERROR("[%s] timer_start: the timer id's place is outside the module's memory", impl->mod->id.c_str());
        return TOME_ERR_INVALID_ARGUMENT;
    }
    uint32_t id = 0;
    const int32_t r = api()->timer_start(impl->mod, clock, delay, interval, on_timer, &impl->timer_cb, &id);
    if (r != TOME_OK) return r;
    impl->timer_slots[id] = callback;
    store(impl, out, id);
    return r;
}

int32_t w_timer_stop(wasm_exec_env_t env, uint32_t timer) {
    Impl* impl = impl_of(env);
    const int32_t r = api()->timer_stop(impl->mod, timer);
    if (r == TOME_OK) impl->timer_slots.erase(timer);
    return r;
}

int32_t w_on_checksum(wasm_exec_env_t env, uint32_t callback) {
    Impl* impl = impl_of(env);
    if (callback == 0) return api()->on_checksum(impl->mod, nullptr, nullptr);
    if (!impl->can_take_callbacks("on_checksum")) return TOME_ERR_INVALID_ARGUMENT;
    Callback* cb = impl->callback(callback);
    return impl->registered(cb, api()->on_checksum(impl->mod, on_checksum, cb));
}

// Every object of a type, as the module's handles (u32s) in `out`, up to
// `max`; how many there are, or -1.
int32_t w_list_objects(wasm_exec_env_t env, int32_t type, uint32_t out, int32_t max) {
    Impl* impl = impl_of(env);
    if (max < 0 || (max > 0 && !wasm::in_bounds(impl->memory(), out, static_cast<uint32_t>(max) * 4u))) return -1;
    const int32_t total = api()->list_objects(type, nullptr, 0);
    if (total <= 0) return total;
    std::vector<void*> found(static_cast<size_t>(total));
    const int32_t n = std::min(api()->list_objects(type, found.data(), total), total);
    // `type` is one of kObjectKinds' (list_objects refused any other).
    const wasm::Handles handles = impl->object_handles();
    const wasm::HandleKind kind = kObjectKinds[type - TOME_OBJECT_UNIT];
    for (int32_t i = 0; i < n && i < max; ++i) {
        // Memory is taken again for each store: handing out a handle can't grow it, but store checks anyway.
        if (!store(impl, out + static_cast<uint32_t>(i) * 4u, handles(kind, found[static_cast<size_t>(i)]))) {
            return -1;
        }
    }
    return n;
}

// jass_call(name, len, args, count, result, text, capacity): the arguments
// are one word each (tome_wasm.h's tome_jass_value), strings as pointers to
// NUL-terminated UTF-8 in the module's memory (0 for null). Tome runs the
// same jass_call as for DLL mods, with its checks: handles live and of the
// parameter's type, code values the map passed to a Tome native. A string
// result is copied to `text` (NUL-terminated, cut to `capacity`) and the
// result word points at it; 0 for a null string.
constexpr uint32_t kMaxJassArgs = 32;

int32_t w_jass_call(wasm_exec_env_t env, uint32_t name_ptr, uint32_t name_len, uint32_t args_ptr, int32_t count,
                    uint32_t result_ptr, uint32_t text_ptr, uint32_t text_capacity) {
    Impl* impl = impl_of(env);
    std::string name;
    if (!arg_string(impl, "jass_call", name_ptr, name_len, name)) return TOME_ERR_INVALID_ARGUMENT;
    if (count < 0 || static_cast<uint32_t>(count) > kMaxJassArgs ||
        (count > 0 && !wasm::in_bounds(impl->memory(), args_ptr, static_cast<uint32_t>(count) * 4u)) ||
        (result_ptr && !wasm::in_bounds(impl->memory(), result_ptr, 4)) ||
        (text_capacity && !wasm::in_bounds(impl->memory(), text_ptr, text_capacity))) {
        TOME_ERROR("[%s] jass_call(%s): an argument is outside the module's memory", impl->mod->id.c_str(),
                   name.c_str());
        return TOME_ERR_INVALID_ARGUMENT;
    }
    objects::JassCall call;
    if (tome_result r = objects::prepare_jass_call(impl->mod, name.c_str(), count, call); r != TOME_OK) {
        store(impl, result_ptr, 0);
        return r;
    }
    const wasm::JassSignature& sig = call.sig;
    std::vector<tome_jass_value> args(static_cast<size_t>(count));
    std::vector<std::string> texts(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) {
        uint32_t word = 0;
        wasm::load_u32(impl->memory(), args_ptr + static_cast<uint32_t>(i) * 4u, word);
        std::memcpy(&args[static_cast<size_t>(i)], &word, sizeof(word));
        if (sig.params[static_cast<size_t>(i)].kind != 'S') continue;
        // A pointer here would be the module's: read the string out, or null.
        args[static_cast<size_t>(i)].string = nullptr;
        if (!word) continue;
        const wasm::Memory m = impl->memory();
        uint32_t end = word;
        while (end < m.size && end - word < kMaxString && m.base[end]) ++end;
        if (end >= m.size || m.base[end]) {
            TOME_ERROR("[%s] jass_call(%s): string argument %d isn't a NUL-terminated string in the module's memory "
                       "(up to %zu bytes)", impl->mod->id.c_str(), name.c_str(), i + 1, kMaxString);
            return TOME_ERR_INVALID_ARGUMENT;
        }
        texts[static_cast<size_t>(i)].assign(reinterpret_cast<const char*>(m.base + word), end - word);
        args[static_cast<size_t>(i)].string = texts[static_cast<size_t>(i)].c_str();
    }
    tome_jass_value result;
    const tome_result r = objects::run_jass_call(impl->mod, name.c_str(), call, args.data(), &result);
    uint32_t word = 0;
    if (r == TOME_OK && sig.result.kind == 'S') {
        if (result.string && text_capacity) {
            const std::string_view text(result.string);
            const size_t n = std::min<size_t>(text.size(), text_capacity - 1);
            const wasm::Memory m = impl->memory();
            if (!wasm::in_bounds(m, text_ptr, text_capacity)) return TOME_ERR_INVALID_ARGUMENT;
            std::memcpy(m.base + text_ptr, text.data(), n);
            m.base[text_ptr + n] = 0;
            word = text_ptr;
        }
    } else {
        std::memcpy(&word, &result, sizeof(word));
    }
    if (!store(impl, result_ptr, word)) return TOME_ERR_INVALID_ARGUMENT;
    return r;
}

#define IMPORT(name, sig) {#name, reinterpret_cast<void*>(w_##name), sig, nullptr},

NativeSymbol g_imports[] = {TOME_WASM_IMPORTS(IMPORT)};

#undef IMPORT

// WAMR, set up on the first Wasm mod's load (under the mod manager's lock),
// so a game without Wasm mods never starts it.
bool g_runtime_tried = false;
bool g_runtime_ok = false;

bool runtime() {
    if (g_runtime_tried) return g_runtime_ok;
    g_runtime_tried = true;
    RuntimeInitArgs init;
    std::memset(&init, 0, sizeof(init));
    // Its own allocations, with a limit (core/wasm_alloc.h).
    init.mem_alloc_type = Alloc_With_Allocator;
    init.mem_alloc_option.allocator.malloc_func = reinterpret_cast<void*>(wasm::runtime_malloc);
    init.mem_alloc_option.allocator.realloc_func = reinterpret_cast<void*>(wasm::runtime_realloc);
    init.mem_alloc_option.allocator.free_func = reinterpret_cast<void*>(wasm::runtime_free);
    if (!wasm_runtime_full_init(&init)) {
        TOME_ERROR("WAMR: couldn't start the WebAssembly runtime");
        return false;
    }
    wasm_runtime_set_log_level(WASM_LOG_LEVEL_WARNING);
    if (!wasm_runtime_register_natives("tome", g_imports, std::size(g_imports))) {
        TOME_ERROR("WAMR: couldn't register Tome's imports");
        wasm_runtime_destroy();
        return false;
    }
    g_runtime_ok = true;
    uint32_t major = 0, minor = 0, patch = 0;
    wasm_runtime_get_version(&major, &minor, &patch);
    TOME_INFO("WAMR %u.%u.%u started (fast interpreter)", major, minor, patch);
    return true;
}

// Whether export `name` exists (`present`) and, if so, has exactly these
// parameter and result types.
bool export_has_type(wasm_module_inst_t inst, const char* name, std::initializer_list<wasm_valkind_t> params,
                     std::initializer_list<wasm_valkind_t> results, bool& present) {
    wasm_function_inst_t fn = wasm_runtime_lookup_function(inst, name);
    present = fn != nullptr;
    if (!fn) return true;
    if (wasm_func_get_param_count(fn, inst) != params.size() || wasm_func_get_result_count(fn, inst) != results.size())
        return false;
    wasm_valkind_t types[8];
    if (params.size() > 8 || results.size() > 8) return false;
    wasm_func_get_param_types(fn, inst, types);
    if (!std::equal(params.begin(), params.end(), types)) return false;
    wasm_func_get_result_types(fn, inst, types);
    return std::equal(results.begin(), results.end(), types);
}

} // namespace

Instance::Instance(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Instance::~Instance() = default;

std::unique_ptr<Instance> Instance::load(tome_mod* mod, const std::wstring& path, std::string& error) {
    timers_api::set_ended_hook(on_timer_ended);
    if (!runtime()) {
        error = "the WebAssembly runtime couldn't start";
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->mod = mod;

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "can't open the module";
        return nullptr;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart > static_cast<LONGLONG>(kMaxModuleSize)) {
        CloseHandle(file);
        error = "the module is larger than " + std::to_string(kMaxModuleSize >> 20) + " MiB";
        return nullptr;
    }
    impl->bytes.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const bool ok = impl->bytes.empty() ||
                    (ReadFile(file, impl->bytes.data(), static_cast<DWORD>(impl->bytes.size()), &read, nullptr) &&
                     read == impl->bytes.size());
    CloseHandle(file);
    if (!ok) {
        error = "can't read the module";
        return nullptr;
    }

    // Looked for before WAMR loads the bytes: it rewrites the buffer in place
    // (names moved over their length bytes), so it's the module as it was
    // only until then.
    const bool has_start = wasm::has_start_section(impl->bytes.data(), impl->bytes.size());
    char why[256] = {};
    impl->module = wasm_runtime_load(impl->bytes.data(), static_cast<uint32_t>(impl->bytes.size()), why, sizeof(why));
    if (!impl->module) {
        error = std::string("not a valid WebAssembly module: ") + why;
        return nullptr;
    }
    // Only Tome's functions: WAMR would otherwise accept an unknown import
    // and trap when it's called.
    const int32_t imports = wasm_runtime_get_import_count(impl->module);
    for (int32_t i = 0; i < imports; ++i) {
        wasm_import_t import;
        wasm_runtime_get_import_type(impl->module, i, &import);
        const std::string name = std::string(import.module_name) + "." + import.name;
        if (import.kind != WASM_IMPORT_EXPORT_KIND_FUNC) {
            error = "imports " + name + ", which isn't a function: Tome provides only functions";
            return nullptr;
        }
        if (std::strcmp(import.module_name, "tome") != 0 || !import.linked) {
            error = "imports " + name + ", which Tome doesn't provide (or not with that type)";
            return nullptr;
        }
    }
    // Nothing may run while WAMR instantiates the module: that's before
    // Tome has the instance (an import couldn't find its mod), without the
    // instruction budget and outside the crash guard. WAMR runs a start
    // function there, and exported __wasm_call_ctors and __post_instantiate.
    if (has_start) {
        error = "has a start function, which would run before Tome can watch it: do that work in tome_mod_load";
        return nullptr;
    }
    const int32_t exports = wasm_runtime_get_export_count(impl->module);
    for (int32_t i = 0; i < exports; ++i) {
        wasm_export_t exp;
        wasm_runtime_get_export_type(impl->module, i, &exp);
        if (exp.kind == WASM_IMPORT_EXPORT_KIND_FUNC &&
            (std::strcmp(exp.name, "__wasm_call_ctors") == 0 || std::strcmp(exp.name, "__post_instantiate") == 0)) {
            error = std::string("exports ") + exp.name +
                    ", which would run before Tome can watch it: call it from tome_mod_load instead";
            return nullptr;
        }
    }

    InstantiationArgs args;
    std::memset(&args, 0, sizeof(args));
    args.default_stack_size = kStackSize;
    args.max_memory_pages = kMaxMemoryPages;
    impl->inst = wasm_runtime_instantiate_ex(impl->module, &args, why, sizeof(why));
    if (!impl->inst) {
        error = std::string("couldn't instantiate the module: ") + why;
        return nullptr;
    }
    // max_memory_pages caps growth only: WAMR gives a module the initial
    // memory it declares (up to 4 GiB), in the game's 32-bit address space.
    if (impl->memory().size > uint64_t(kMaxMemoryPages) * 65536) {
        error = "starts with more than " + std::to_string(kMaxMemoryPages / 16) + " MiB of memory";
        return nullptr;
    }
    wasm_runtime_set_custom_data(impl->inst, impl.get());

    constexpr wasm_valkind_t I32 = WASM_I32;
    struct Expected {
        const char* name;
        std::initializer_list<wasm_valkind_t> params, results;
        bool required;
    };
    const Expected expected[] = {
        {"tome_mod_load", {}, {I32}, true},
        {"tome_mod_enable", {}, {}, false},
        {"tome_mod_disable", {}, {}, false},
        {"tome_mod_unload", {}, {}, false},
        {"tome_buffer", {I32}, {I32}, false},
        {"tome_callback", {I32, I32, I32}, {}, false},
        {"tome_native", {I32, I32, I32}, {I32}, false},
        {"tome_callbacks_revoked", {}, {}, false},
    };
    bool present[std::size(expected)] = {};
    for (size_t i = 0; i < std::size(expected); ++i) {
        const Expected& e = expected[i];
        if (!export_has_type(impl->inst, e.name, e.params, e.results, present[i])) {
            error = std::string("exports ") + e.name + " with the wrong type";
            return nullptr;
        }
        if (e.required && !present[i]) {
            error = std::string("doesn't export ") + e.name;
            return nullptr;
        }
    }
    if ((present[5] || present[6]) && !present[4]) {
        error = "exports tome_callback or tome_native without tome_buffer";
        return nullptr;
    }
    impl->takes_callbacks = present[4] && present[5];
    impl->takes_natives = present[4] && present[6];
    impl->fn_buffer = wasm_runtime_lookup_function(impl->inst, "tome_buffer");
    impl->fn_callback = wasm_runtime_lookup_function(impl->inst, "tome_callback");
    impl->fn_native = wasm_runtime_lookup_function(impl->inst, "tome_native");
    impl->fn_revoked = wasm_runtime_lookup_function(impl->inst, "tome_callbacks_revoked");
    return std::unique_ptr<Instance>(new Instance(std::move(impl)));
}

int32_t Instance::call_load() {
    uint32_t argv[1] = {0};
    if (!impl_->invoke("tome_mod_load", 0, argv)) fail_call();
    return static_cast<int32_t>(argv[0]);
}

bool Instance::has(const char* lifecycle_export) const {
    return wasm_runtime_lookup_function(impl_->inst, lifecycle_export) != nullptr;
}

void Instance::call(const char* lifecycle_export) {
    uint32_t argv[1] = {0};
    if (!impl_->invoke(lifecycle_export, 0, argv) && impl_->depth == 0) fail_call();
}

void Instance::enable() {
    if (!impl_->forget_revoked()) fail_call();
    if (has("tome_mod_enable")) call("tome_mod_enable");
}

void Instance::revoked() {
    impl_->revoked = true;
}

} // namespace tome::wasm_mods
