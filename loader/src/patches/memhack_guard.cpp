/*
 * Memhack guard (docs/known-bugs.md, docs/reverse-engineering.md).
 *
 * The 1.27b JASS compiler interns each name to one shared symbol whose type
 * descriptor lives at symbol+0x18. A local variable declaration overwrites
 * that pointer with the local's descriptor (JassLocalVarDesc::ctor, swap at
 * 0x91DD5E) and never restores it at endfunction. A local that shadows a
 * GLOBAL of a different type therefore repoints the global's type for the rest
 * of the compile -- the handle<->integer typecast primitive (the memhack/ACE
 * gateway).
 *
 * Fix (transparent scope restore): when a local is declared over a global's
 * symbol, remember the global's descriptor; when the scanner yields the
 * "endfunction" keyword (JassScanner::nextToken returns 0x115), write the
 * global's descriptor back into symbol+0x18. The local still works as its own
 * type inside the function; the global's type is correct everywhere after it,
 * closing the typecast with no change to legitimate code. Parameters are not a
 * vector (their ctor doesn't touch symbol+0x18). Runs during compilation on
 * the game's main thread, so the single static save list needs no lock.
 *
 * The save list is also dropped when a compile starts and ends (Jass::compile,
 * BEFORE and AFTER): a script that stops inside a function (a syntax error,
 * EOF) never reaches its endfunction, and the next compile's first
 * endfunction would otherwise write into the old compile's freed symbols.
 *
 * Built in as a patch, not a mod, so it's always in force (patches.h). The
 * downloaded-map scanner that *reports* the same pattern in the log lives
 * separately, at map load (events.cpp, core/jass.cpp).
 */
#include "patches.h"

#include "hooks.h"
#include "log.h"
#include "tome/tome.h"

#include <cstdint>
#include <vector>

// GCC warns that __thiscall (TOME_CALL) is meant for class methods, but it does
// apply it to these free functions (as in events.cpp and mod_menu.cpp).
#pragma GCC diagnostic ignored "-Wattributes"

namespace tome::patches {
namespace {

constexpr uint32_t kKindLocalVar = 0x29;    // JassVarDesc+0x4 for a local variable
constexpr uint32_t kTokEndFunction = 0x115; // JassScanner::nextToken code for `endfunction`

struct Saved {
    uint32_t sym = 0;
    uint32_t desc = 0;
};
// Unbounded: a fixed cap would let a function declare that many harmless
// shadows first and then a typecasting one past the cap, unrestored.
std::vector<Saved> g_saved;
unsigned long g_restored_total = 0;

// Guarded game-memory access: the compiler's own pointers are in this range.
bool ok(uint32_t p) { return p >= 0x10000u && p < 0xF0000000u; }
uint32_t rd(uint32_t p) { return ok(p) ? *reinterpret_cast<volatile uint32_t*>(static_cast<uintptr_t>(p)) : 0u; }
void wr(uint32_t p, uint32_t v) {
    if (ok(p)) *reinterpret_cast<volatile uint32_t*>(static_cast<uintptr_t>(p)) = v;
}

// BEFORE JassLocalVarDesc::ctor(this, a0, typeNode, sym, a3): sym+0x18 still
// holds the symbol's current descriptor. Unless it's another local's (or
// there is none), remember it so the next endfunction can restore it: global
// variables (kind 0x2a) and any other non-local kind, so a global kind other
// than 0x2a (e.g. 0x2b, which the compiler's variable test also accepts)
// can't slip past the restore.
void TOME_CALL on_local_before(tome_mod*, tome_hook_call* call, void*) {
    if (call->arg_count < 4) return;
    uint32_t sym = call->args[3];
    uint32_t cur = rd(sym + 0x18);
    if (!ok(cur) || rd(cur + 0x4) == kKindLocalVar) return;  // nothing shadowed, or a leftover local
    g_saved.push_back({sym, cur});
}

// AFTER JassScanner::nextToken -> token code in result. On the endfunction
// keyword, restore every shadowed global's descriptor and clear the list.
void TOME_CALL on_next_token(tome_mod*, tome_hook_call* call, void*) {
    if (call->result != kTokEndFunction || g_saved.empty()) return;
    // Newest first, so a name saved twice ends with its oldest descriptor.
    for (auto it = g_saved.rbegin(); it != g_saved.rend(); ++it) {
        wr(it->sym + 0x18, it->desc);
        ++g_restored_total;
    }
    TOME_INFO("memhack-guard: endfunction restored %zu shadowed global(s) (total %lu)", g_saved.size(),
              g_restored_total);
    g_saved.clear();
}

// BEFORE and AFTER Jass::compile: a new compile's symbols aren't the ones
// saved, and a finished one's are about to be (or were) freed.
void TOME_CALL on_compile(tome_mod*, tome_hook_call*, void*) {
    if (g_saved.empty()) return;
    TOME_WARN("memhack-guard: a JASS compile stopped inside a function; dropped %zu saved descriptor(s)",
              g_saved.size());
    g_saved.clear();
}

bool install_memhack_guard(uintptr_t, const BuildMappings&, std::string& report) {
    std::string error;
    hooks::Target* compile = hooks::find_or_install("Jass::compile", error);
    if (!compile) {
        report = "Jass::compile: " + error;
        return false;
    }
    hooks::Target* ctor = hooks::find_or_install("JassLocalVarDesc::ctor", error);
    if (!ctor) {
        report = "JassLocalVarDesc::ctor: " + error;
        return false;
    }
    hooks::Target* token = hooks::find_or_install("JassScanner::nextToken", error);
    if (!token) {
        report = "JassScanner::nextToken: " + error;
        return false;
    }

    hooks::Handler before;
    before.phase = TOME_HOOK_BEFORE;
    before.priority = INT32_MAX;  // before any mod's handler
    before.fn = on_local_before;
    hooks::add(ctor, before);

    hooks::Handler after;
    after.phase = TOME_HOOK_AFTER;
    after.priority = INT32_MIN;  // after any mod's handler
    after.fn = on_next_token;
    hooks::add(token, after);

    hooks::Handler start;
    start.phase = TOME_HOOK_BEFORE;
    start.priority = INT32_MAX;
    start.fn = on_compile;
    hooks::add(compile, start);
    hooks::Handler end = start;
    end.phase = TOME_HOOK_AFTER;
    end.priority = INT32_MIN;
    hooks::add(compile, end);

    g_saved.clear();
    report = "transparent scope restore on the JASS compiler";
    return true;
}

const char* const kSymbols[] = {"Jass::compile", "JassLocalVarDesc::ctor", "JassScanner::nextToken", nullptr};

} // namespace

// extern: const at namespace scope is internal linkage by default; patches.cpp
// references this across translation units.
extern const Patch kMemhackGuard = {
    "memhack-guard",
    "closes the JASS local-shadows-global typecast (memhack/ACE)",
    nullptr,   // any build that maps the symbols below
    kSymbols,  // symbols it needs mapped
    kSymbols,  // and reserves: mods can't hook either compiler function
    nullptr,    // needed everywhere
    install_memhack_guard,
};

} // namespace tome::patches
