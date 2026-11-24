// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/hooks.h"
#include "test.h"

using namespace tome;
using hooks::Where;

namespace {

hooks::Layout layout(mappings::Convention conv, const char* sig) {
    hooks::Layout out;
    std::string error;
    if (!hooks::layout_for(conv, sig, out, error)) test::fail(__FILE__, __LINE__, std::string(sig) + ": " + error);
    return out;
}

bool refused(mappings::Convention conv, const char* sig, const char* reason) {
    hooks::Layout out;
    std::string error;
    return !hooks::layout_for(conv, sig, out, error) && error.find(reason) != std::string::npos;
}

// The trace handlers append to, and a fake original.
std::string g_trace;

void TOME_CALL trace_handler(tome_mod*, tome_hook_call* call, void* user) {
    g_trace += static_cast<const char*>(user);
    g_trace += ' ';
    (void)call;
}

void TOME_CALL doubling_before(tome_mod*, tome_hook_call* call, void*) {
    g_trace += "double ";
    call->args[0] *= 2;
}

void TOME_CALL skipping_before(tome_mod*, tome_hook_call* call, void*) {
    g_trace += "skip ";
    call->skip_original = 1;
    call->result = 77;
}

void TOME_CALL replace_calls_next(tome_mod*, tome_hook_call* call, void* user) {
    g_trace += static_cast<const char*>(user);
    g_trace += "( ";
    hooks::call_next(call);
    g_trace += ") ";
    call->result += 1000;
}

void TOME_CALL replace_alone(tome_mod*, tome_hook_call* call, void*) {
    g_trace += "alone ";
    call->result = 5;
}

void TOME_CALL after_adds(tome_mod*, tome_hook_call* call, void*) {
    g_trace += "after ";
    call->result += 1;
}

void original(void*, tome_hook_call& call) {
    g_trace += "original ";
    call.result = call.args[0] + 1;
}

hooks::Handler handler(int32_t phase, int32_t priority, tome_hook_fn fn, const char* user, uint64_t seq,
                       tome_mod* mod = nullptr) {
    hooks::Handler h;
    h.mod = mod;
    h.phase = phase;
    h.priority = priority;
    h.fn = fn;
    h.user = const_cast<char*>(user);
    h.seq = seq;
    return h;
}

uint32_t run(const std::vector<hooks::Handler>& chain, uint32_t arg) {
    g_trace.clear();
    uint32_t args[1] = {arg};
    hooks::Dispatch d;
    d.call.args = args;
    d.call.arg_count = 1;
    d.handlers = chain.data();
    d.count = chain.size();
    d.original = original;
    hooks::run(d);
    return d.call.result;
}

} // namespace

TEST(hooks_layout_per_convention) {
    using C = mappings::Convention;
    hooks::Layout l = layout(C::Fastcall, "CFrame*(const char* name, int context)");
    CHECK_EQ(l.params.size(), size_t(2));
    CHECK(l.params[0].where == Where::Ecx);
    CHECK(l.params[1].where == Where::Edx);
    CHECK_EQ(l.stack_slots, 0u);
    CHECK_EQ(l.pop_bytes, 0u);
    CHECK(l.result == hooks::Result::Int);

    l = layout(C::Fastcall, "void(CModelInstance* instance, const int* tags, int count, int flags)");
    CHECK(l.params[2].where == Where::Stack && l.params[2].slot == 0);
    CHECK(l.params[3].where == Where::Stack && l.params[3].slot == 1);
    CHECK_EQ(l.pop_bytes, 8u);
    CHECK(l.result == hooks::Result::None);

    // Floats never go in fastcall registers.
    l = layout(C::Fastcall, "int(float a, int b, float c, int d, int e)");
    CHECK(l.params[0].where == Where::Stack && l.params[0].is_float);
    CHECK(l.params[1].where == Where::Ecx);
    CHECK(l.params[2].where == Where::Stack && l.params[2].slot == 1);
    CHECK(l.params[3].where == Where::Edx);
    CHECK(l.params[4].where == Where::Stack && l.params[4].slot == 2);

    l = layout(C::Thiscall, "void(CLayoutFrame* this, int point, CLayoutFrame* relative, int relative_point, "
                            "float x, float y, int update)");
    CHECK_EQ(l.params.size(), size_t(7));
    CHECK(l.params[0].where == Where::Ecx);
    CHECK(l.params[1].where == Where::Stack && l.params[1].slot == 0);
    CHECK(l.params[4].is_float && l.params[4].slot == 3);
    CHECK_EQ(l.pop_bytes, 24u);

    l = layout(C::Cdecl, "void(void)");
    CHECK(l.params.empty());
    l = layout(C::Cdecl, "int(int a, int b)");
    CHECK_EQ(l.stack_slots, 2u);
    CHECK_EQ(l.pop_bytes, 0u);
    l = layout(C::Stdcall, "int(int a, int b)");
    CHECK_EQ(l.pop_bytes, 8u);
    l = layout(C::Cdecl, "uint64_t(int a)");  // edx:eax
    CHECK(l.result == hooks::Result::Int);
    CHECK(l.wide_result);
    CHECK(layout(C::Cdecl, "ULONGLONG(void)").wide_result);  // Windows' names too
    CHECK(!layout(C::Cdecl, "int(int a)").wide_result);      // edx goes back to the caller as it came
    CHECK(layout(C::Cdecl, "float(int)").result == hooks::Result::Float);
    CHECK(layout(C::Cdecl, "double(void)").result == hooks::Result::Double);
    CHECK(layout(C::Cdecl, "float*(void)").result == hooks::Result::Int);
    l = layout(C::Cdecl, "void(void (*callback)(int, int), int x)");
    CHECK_EQ(l.params.size(), size_t(2));
}

TEST(hooks_layout_wide_arguments) {
    using C = mappings::Convention;
    // 64-bit arguments take two words and two stack slots, low half first.
    hooks::Layout l = layout(C::Cdecl, "void(int a, double b, int c)");
    CHECK_EQ(l.arg_words, 4u);
    CHECK_EQ(l.stack_slots, 4u);
    CHECK(l.params[1].index == 1 && l.params[1].words == 2 && l.params[1].slot == 1);
    CHECK(l.params[2].index == 3 && l.params[2].slot == 3);

    // fastcall: a 64-bit argument goes on the stack and leaves the registers
    // to the next 32-bit ones.
    l = layout(C::Fastcall, "int(long long a, int b, unsigned __int64 c, int d, int e)");
    CHECK(l.params[0].where == Where::Stack && l.params[0].slot == 0 && l.params[0].words == 2);
    CHECK(l.params[1].where == Where::Ecx && l.params[1].index == 2);
    CHECK(l.params[2].where == Where::Stack && l.params[2].slot == 2);
    CHECK(l.params[3].where == Where::Edx);
    CHECK(l.params[4].where == Where::Stack && l.params[4].slot == 4);
    CHECK_EQ(l.pop_bytes, 20u);

    l = layout(C::Stdcall, "void(uint64_t a)");
    CHECK_EQ(l.pop_bytes, 8u);
}

TEST(hooks_layout_variadic) {
    using C = mappings::Convention;
    hooks::Layout l = layout(C::Cdecl, "int(const char* fmt, ...)");
    CHECK(l.variadic);
    CHECK_EQ(l.arg_words, 1u);
    CHECK_EQ(l.pop_bytes, 0u);
    // Whatever the declared convention, a variadic function is caller-cleaned
    // with everything on the stack.
    l = layout(C::Thiscall, "void(CObject* this, const char* fmt, ...)");
    CHECK(l.variadic);
    CHECK(l.params[0].where == Where::Stack && l.params[0].slot == 0);
    CHECK_EQ(l.pop_bytes, 0u);
    l = layout(C::Stdcall, "void(int n, ...)");
    CHECK_EQ(l.pop_bytes, 0u);
}

TEST(hooks_layout_refusals) {
    using C = mappings::Convention;
    CHECK(refused(C::Cdecl, "void(int, ..., int)", "last parameter"));
    CHECK(refused(C::Thiscall, "void(void)", "`this`"));
    CHECK(refused(C::Cdecl, "nonsense", "result(params)"));
    CHECK(refused(C::Cdecl, "(int)", "no result type"));
    CHECK(refused(C::None, "void(void)", "convention"));
    CHECK(refused(C::Cdecl,
                  "void(double, double, double, double, double, double, double, double, "
                  "double, double, double, double, double, double, double, double, int)",
                  "words of arguments"));
}

// Two mappings of one function share its patch only if they call it the same way.
TEST(hooks_same_layout) {
    using C = mappings::Convention;
    CHECK(hooks::same_layout(layout(C::Cdecl, "int(int a, void* b)"), layout(C::Cdecl, "int(int x, Foo* y)")));
    // A float argument is passed like an int; a float result isn't.
    CHECK(hooks::same_layout(layout(C::Stdcall, "void(float a)"), layout(C::Stdcall, "void(int a)")));
    CHECK(!hooks::same_layout(layout(C::Cdecl, "float(int a)"), layout(C::Cdecl, "int(int a)")));
    CHECK(!hooks::same_layout(layout(C::Cdecl, "int(int a)"), layout(C::Stdcall, "int(int a)")));     // cleanup
    CHECK(!hooks::same_layout(layout(C::Fastcall, "int(int a)"), layout(C::Stdcall, "int(int a)")));  // ecx
    CHECK(!hooks::same_layout(layout(C::Cdecl, "int(int a)"), layout(C::Cdecl, "int(int a, int b)")));
    // The same words, but one argument against two: refused, to be safe.
    CHECK(!hooks::same_layout(layout(C::Cdecl, "int(double a)"), layout(C::Cdecl, "int(int a, int b)")));
    CHECK(!hooks::same_layout(layout(C::Cdecl, "int64_t(int a)"), layout(C::Cdecl, "int(int a)")));   // edx:eax
    CHECK(!hooks::same_layout(layout(C::Cdecl, "int(int n, ...)"), layout(C::Cdecl, "int(int n)")));
}

TEST(hooks_chain_order) {
    std::vector<hooks::Handler> chain;
    hooks::insert(chain, handler(TOME_HOOK_AFTER, 0, trace_handler, "a0", 1));
    hooks::insert(chain, handler(TOME_HOOK_BEFORE, 0, trace_handler, "b0", 2));
    hooks::insert(chain, handler(TOME_HOOK_BEFORE, 10, trace_handler, "b10", 3));
    hooks::insert(chain, handler(TOME_HOOK_BEFORE, 0, trace_handler, "b0late", 4));
    hooks::insert(chain, handler(TOME_HOOK_AFTER, 5, trace_handler, "a5", 5));
    CHECK_EQ(run(chain, 1), 2u);
    CHECK_EQ(g_trace, std::string("b10 b0 b0late original a5 a0 "));

    // A `last` handler (Tome's innermost) runs after its phase's others,
    // even one at INT32_MIN registered after it.
    hooks::Handler last = handler(TOME_HOOK_BEFORE, INT32_MAX, trace_handler, "last", 6);
    last.last = true;
    hooks::insert(chain, last);
    hooks::insert(chain, handler(TOME_HOOK_BEFORE, INT32_MIN, trace_handler, "bmin", 7));
    CHECK_EQ(run(chain, 1), 2u);
    CHECK_EQ(g_trace, std::string("b10 b0 b0late bmin last original a5 a0 "));
}

TEST(hooks_replace_nests_and_args_flow) {
    std::vector<hooks::Handler> chain;
    hooks::insert(chain, handler(TOME_HOOK_BEFORE, 0, doubling_before, "", 1));
    hooks::insert(chain, handler(TOME_HOOK_REPLACE, 0, replace_calls_next, "inner", 2));
    hooks::insert(chain, handler(TOME_HOOK_REPLACE, 9, replace_calls_next, "outer", 3));
    hooks::insert(chain, handler(TOME_HOOK_AFTER, 0, after_adds, "", 4));
    // 10 doubled → original 21 → +1000 (inner) → +1000 (outer) → +1 (after)
    CHECK_EQ(run(chain, 10), 2022u);
    CHECK_EQ(g_trace, std::string("double outer( inner( original ) ) after "));

    // A REPLACE handler that doesn't call next stops the chain there.
    hooks::insert(chain, handler(TOME_HOOK_REPLACE, 5, replace_alone, "", 5));
    CHECK_EQ(run(chain, 10), 1006u);
    CHECK_EQ(g_trace, std::string("double outer( alone ) after "));
}

TEST(hooks_skip_original) {
    std::vector<hooks::Handler> chain;
    hooks::insert(chain, handler(TOME_HOOK_BEFORE, 5, skipping_before, "", 1));
    hooks::insert(chain, handler(TOME_HOOK_BEFORE, 0, trace_handler, "later", 2));
    hooks::insert(chain, handler(TOME_HOOK_REPLACE, 0, replace_calls_next, "r", 3));
    hooks::insert(chain, handler(TOME_HOOK_AFTER, 0, after_adds, "", 4));
    CHECK_EQ(run(chain, 1), 78u);
    CHECK_EQ(g_trace, std::string("skip later after "));
}

TEST(hooks_remove) {
    tome_mod* a = reinterpret_cast<tome_mod*>(0x10);
    tome_mod* b = reinterpret_cast<tome_mod*>(0x20);
    std::vector<hooks::Handler> chain;
    hooks::insert(chain, handler(TOME_HOOK_BEFORE, 0, trace_handler, "a", 1, a));
    hooks::insert(chain, handler(TOME_HOOK_BEFORE, 0, trace_handler, "b", 2, b));
    hooks::insert(chain, handler(TOME_HOOK_AFTER, 0, trace_handler, "a", 3, a));
    CHECK_EQ(hooks::remove(chain, a, TOME_HOOK_AFTER, trace_handler, const_cast<char*>("x")), size_t(0));
    CHECK_EQ(run(chain, 1), 2u);
    CHECK_EQ(g_trace, std::string("a b original a "));
    CHECK_EQ(hooks::remove(chain, b, TOME_HOOK_BEFORE, trace_handler, chain[1].user), size_t(1));
    CHECK_EQ(hooks::remove_all(chain, a), size_t(2));
    CHECK(chain.empty());
    CHECK_EQ(run(chain, 1), 2u);
}

TEST(hooks_revoked_gates_are_skipped) {
    auto gate = std::make_shared<hooks::Gate>();
    std::vector<hooks::Handler> chain;
    hooks::Handler before = handler(TOME_HOOK_BEFORE, 0, trace_handler, "b", 1);
    before.gate = gate;
    hooks::insert(chain, before);
    hooks::Handler replace = handler(TOME_HOOK_REPLACE, 0, replace_calls_next, "r", 2);
    replace.gate = gate;
    hooks::insert(chain, replace);
    CHECK_EQ(run(chain, 1), 1002u);
    CHECK_EQ(g_trace, std::string("b r( original ) "));
    CHECK_EQ(gate->busy.load(), 0);
    // A call still holding this chain after the handlers were revoked skips
    // them, and a skipped REPLACE handler passes on to the next one.
    gate->revoked = true;
    CHECK_EQ(run(chain, 1), 2u);
    CHECK_EQ(g_trace, std::string("original "));
    CHECK_EQ(gate->busy.load(), 0);
}

namespace {
std::shared_ptr<hooks::Gate> g_inner_gate;
int g_seen_inside = -1;
void TOME_CALL count_running(tome_mod*, tome_hook_call*, void*) {
    g_seen_inside = hooks::running_on_this_thread(*g_inner_gate) * 10 + g_inner_gate->busy.load();
}
} // namespace

TEST(hooks_gate_counts_running_calls) {
    g_inner_gate = std::make_shared<hooks::Gate>();
    std::vector<hooks::Handler> chain;
    hooks::Handler h = handler(TOME_HOOK_BEFORE, 0, count_running, "", 1);
    h.gate = g_inner_gate;
    hooks::insert(chain, h);
    run(chain, 1);
    CHECK_EQ(g_seen_inside, 11);  // one call, on this thread
    CHECK_EQ(hooks::running_on_this_thread(*g_inner_gate), 0);
    g_inner_gate.reset();
}

namespace {
void TOME_CALL replace_with_a_copy(tome_mod*, tome_hook_call* call, void*) {
    g_trace += "copy ";
    tome_hook_call copy = *call;  // not the call Tome passed in
    hooks::call_next(&copy);
    call->result = 9;
}
} // namespace

TEST(hooks_call_next_only_accepts_the_running_call) {
    std::vector<hooks::Handler> chain;
    hooks::insert(chain, handler(TOME_HOOK_REPLACE, 0, replace_with_a_copy, "", 1));
    CHECK_EQ(run(chain, 1), 9u);
    CHECK_EQ(g_trace, std::string("copy "));  // the original never ran
}

TEST(hooks_call_next_outside_replace_does_nothing) {
    g_trace.clear();
    hooks::Dispatch d;  // never run
    d.original = original;
    hooks::call_next(&d.call);
    hooks::call_next(nullptr);
    CHECK(g_trace.empty());
}

namespace {
void original_handed_on(void* context, tome_hook_call& call) {
    g_trace += "uncharged-";
    original(context, call);
}

uint32_t run_with_handed_on(const std::vector<hooks::Handler>& chain, uint32_t arg) {
    g_trace.clear();
    uint32_t args[1] = {arg};
    hooks::Dispatch d;
    d.call.args = args;
    d.call.arg_count = 1;
    d.handlers = chain.data();
    d.count = chain.size();
    d.original = original;
    d.original_handed_on = original_handed_on;
    hooks::run(d);
    return d.call.result;
}
} // namespace

// The original runs uncharged (guard::uncharged in the loader) only when a
// mod's REPLACE handler handed on towards it (through Tome's own handlers
// too): not without one, nor for Tome's handlers alone (no mod, no
// profile; the time stays with whoever called the function).
TEST(hooks_original_handed_on_by_a_mod) {
    tome_mod* mod = reinterpret_cast<tome_mod*>(0x1000);
    run_with_handed_on({}, 1);
    CHECK_EQ(g_trace, std::string("original "));
    run_with_handed_on({handler(TOME_HOOK_REPLACE, 0, replace_calls_next, "a", 1, mod)}, 1);
    CHECK_EQ(g_trace, std::string("a( uncharged-original ) "));
    run_with_handed_on({handler(TOME_HOOK_REPLACE, 0, replace_calls_next, "tome", 1)}, 1);
    CHECK_EQ(g_trace, std::string("tome( original ) "));
    // Tome's handler first, then a mod's: the mod's is the one handing on.
    run_with_handed_on({handler(TOME_HOOK_REPLACE, 1, replace_calls_next, "tome", 1),
                        handler(TOME_HOOK_REPLACE, 0, replace_calls_next, "a", 2, mod)},
                       1);
    CHECK_EQ(g_trace, std::string("tome( a( uncharged-original ) ) "));
    run_with_handed_on({handler(TOME_HOOK_REPLACE, 1, replace_calls_next, "a", 1, mod),
                        handler(TOME_HOOK_REPLACE, 0, replace_calls_next, "tome", 2)},
                       1);
    CHECK_EQ(g_trace, std::string("a( tome( uncharged-original ) ) "));
}
