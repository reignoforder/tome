#pragma once

// The portable half of Tome's hook manager (loader/src/hooks.cpp patches
// the game): where a mapped function's arguments live, and the order in
// which a chain of handlers runs around it. See tome.h, "Hooks".

#include "mappings.h"

#include <tome/tome.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tome::hooks {

constexpr size_t kMaxArgWords = 32;
// Variadic functions: how many words of the caller's stack after the fixed
// arguments are handed to the handlers and passed on to the original.
constexpr size_t kVariadicWords = 32;

enum class Where : uint8_t { Ecx, Edx, Stack };
enum class Result : uint8_t { None, Int, Float, Double };  // Int: eax, or edx:eax

struct Param {
    Where where = Where::Stack;
    uint8_t slot = 0;   // Stack: index of the first 32-bit stack slot (0 = just above the return address)
    uint8_t index = 0;  // index of the first word in tome_hook_call.args
    uint8_t words = 1;  // 2 for double and 64-bit integers (low half first)
    bool is_float = false;
};

struct Layout {
    std::vector<Param> params;  // in declaration order
    uint32_t arg_words = 0;     // words in tome_hook_call.args (fixed arguments)
    uint32_t stack_slots = 0;   // 32-bit stack slots of the fixed arguments
    uint32_t pop_bytes = 0;     // what the function pops on return (0 for cdecl and variadic)
    Result result = Result::None;
    bool variadic = false;      // "..." last: always caller-cleaned, everything on the stack
};

// Works out the layout from a mapping's convention and signature, e.g.
// fastcall "CFrame*(const char* name, int context)": name in ecx, context in
// edx. Pointers and unknown type names count as 32-bit; double, int64_t,
// uint64_t, __int64 and long long as 64-bit. Variadic functions follow
// MSVC: whatever the declared convention, the caller pushes everything and
// cleans up.
bool layout_for(mappings::Convention convention, const std::string& signature, Layout& out, std::string& error);

// One registration's state, shared by every snapshot of the chain that
// holds it, so a mod can be unloaded only once none of its handlers runs.
struct Gate {
    std::atomic<int> busy{0};          // calls of the handler running right now, any thread
    std::atomic<bool> revoked{false};  // set before the handler leaves the chain; no new calls start
};

// How many calls of `gate`'s handler are on this thread's stack.
int running_on_this_thread(const Gate& gate);

struct Handler {
    tome_mod* mod = nullptr;
    int32_t phase = TOME_HOOK_BEFORE;
    int32_t priority = 0;
    // Tome's own innermost handlers (not settable through the mod API):
    // after every handler of its phase without it, whatever their priority,
    // so a mod's handler at INT32_MIN still runs before it.
    bool last = false;
    tome_hook_fn fn = nullptr;
    void* user = nullptr;
    uint64_t seq = 0;  // registration order, for equal priorities
    std::shared_ptr<Gate> gate;  // optional
};

// Inserts `h` in run order: by phase, then `last` ones after the rest, then
// higher priority first, then earlier registration first.
void insert(std::vector<Handler>& chain, const Handler& h);

// Removes the handlers matching all given fields; returns how many.
size_t remove(std::vector<Handler>& chain, tome_mod* mod, int32_t phase, tome_hook_fn fn, void* user);
size_t remove_all(std::vector<Handler>& chain, tome_mod* mod);

// One call of a hooked function going through its chain. `call` comes first
// so the tome_hook_call* handed to mods leads back here.
struct Dispatch {
    tome_hook_call call = {};
    const Handler* handlers = nullptr;  // sorted with insert()
    size_t count = 0;
    size_t next = 0;  // the next handler call_next considers
    bool in_replace = false;
    // Runs the game's function with call.args, setting call.result(_high)
    // or call.result_float.
    void (*original)(void* context, tome_hook_call& call) = nullptr;
    void* context = nullptr;
    // Calls one handler; false if it didn't finish (it crashed, or its mod
    // had crashed before). Null: handlers are called directly.
    bool (*invoke)(const Handler& h, tome_hook_call& call) = nullptr;
    size_t handed_on = 0;  // counts call_next_in runs, so a crashed REPLACE handler's next isn't run twice
};

// Runs the whole chain: BEFORE, REPLACE (or the original), AFTER. Handlers
// whose gate is revoked are skipped. A REPLACE handler that doesn't finish
// before handing on counts as skipped: the next one (or the original) runs.
void run(Dispatch& d);

// This thread's place in nested hook calls. A guard that abandons a crashed
// call takes it before the call and restores it afterwards, dropping the
// handlers and chains the crash skipped (their gates stop counting as busy).
struct ThreadState {
    size_t running = 0;
    size_t dispatches = 0;
};
ThreadState thread_state();
void restore_thread_state(const ThreadState& state);

// hook_call_next: the next REPLACE handler, or the original after the last.
// Does nothing unless `call` is the call of a Dispatch running on this
// thread and inside its REPLACE phase; any other pointer is only compared,
// never read.
void call_next(tome_hook_call* call);

} // namespace tome::hooks
