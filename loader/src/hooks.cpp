// SPDX-License-Identifier: GPL-3.0-or-later
#include "hooks.h"

#include "core/x86.h"
#include "guard.h"
#include "log.h"
#include "mod_mappings.h"
#include "patch.h"
#include "shade.h"
#include "unwind.h"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>

namespace tome::hooks {

struct Target {
    std::string name;  // the first name it was hooked by
    uintptr_t address = 0;
    mappings::Convention convention = mappings::Convention::None;
    std::string signature;
    Layout layout;
    bool import = false;            // an import address table slot, not code
    size_t moved = 0;               // code: the bytes moved to the trampoline, from `address` on
    uint8_t original[5] = {};       // code: the bytes the jmp replaced
    uint8_t* entry = nullptr;       // push <this>; jmp tome_hook_entry
    uint8_t* trampoline = nullptr;  // the moved instructions; jmp back (an import: the imported function)
    SRWLOCK lock = SRWLOCK_INIT;
    std::shared_ptr<const std::vector<Handler>> handlers = std::make_shared<std::vector<Handler>>();
};

} // namespace tome::hooks

// st(0) results: what tome_hook_entry loads and tome_hook_invoke stores.
enum : uint32_t { kFpNone = 0, kFpFloat = 1, kFpDouble = 2 };

union TomeHookFp {
    float f;
    double d;
};

// What tome_hook_entry leaves on the stack: room for the dispatcher's answer,
// the registers it saved, the target its stub pushed, then the game's return
// address and stack arguments.
struct TomeHookFrame {
    // Set by the dispatcher.
    uint32_t eax;
    uint32_t edx_out;
    uint32_t pop_bytes;  // ret N
    uint32_t fp_kind;    // kFp*: what to load into st(0)
    TomeHookFp fp;
    // Saved by tome_hook_entry.
    uint32_t ecx;
    uint32_t edx;
    uint32_t eax_in;  // the caller's eax
    tome::hooks::Target* target;
    uint32_t return_address;
    uint32_t stack[1];
};
static_assert(offsetof(TomeHookFrame, fp) == 16 && offsetof(TomeHookFrame, ecx) == 24 &&
              offsetof(TomeHookFrame, eax_in) == 32 && offsetof(TomeHookFrame, stack) == 44,
              "tome_hook_entry depends on this layout");

struct TomeHookResult {
    uint32_t eax;
    uint32_t edx;
    TomeHookFp fp;
};
static_assert(offsetof(TomeHookResult, fp) == 8, "tome_hook_invoke depends on this layout");

extern "C" {
void tome_hook_dispatch(TomeHookFrame* frame);
void tome_hook_entry();
void tome_hook_invoke(const void* fn, const uint32_t* stack, uint32_t count, uint32_t ecx, uint32_t edx,
                      TomeHookResult* out, uint32_t fp_kind, uint32_t eax);
}

// tome_hook_entry: every stub jumps here with its Target* pushed. Saves
// eax and ecx/edx (fastcall/thiscall arguments) next to it, makes room for the
// answer and calls the dispatcher with a pointer to the whole frame. Then
// loads st(0) for floating-point results, sets eax/edx, puts the caller's
// ecx back (as the dispatcher does with edx) and returns to the game, popping
// the arguments for callee-cleaned conventions: for those it moves the return
// address up over them and the saved ecx just below it, then pops that.
//
// tome_hook_invoke: calls `fn` with `count` stack arguments and eax/ecx/edx set,
// whatever its convention: esp is restored from ebp afterwards, so it doesn't
// matter whether the callee popped them. Stores eax, edx and, if `fp_kind`
// says so, pops st(0).
asm(R"(
    .intel_syntax noprefix
    .text
    .globl _tome_hook_entry
_tome_hook_entry:
    push eax
    push edx
    push ecx
    sub esp, 24
    push esp
    call _tome_hook_dispatch
    add esp, 4
    mov ecx, [esp + 12]
    cmp ecx, 1
    jb .Ltome_hook_regs
    ja .Ltome_hook_double
    fld dword ptr [esp + 16]
    jmp .Ltome_hook_regs
.Ltome_hook_double:
    fld qword ptr [esp + 16]
.Ltome_hook_regs:
    mov eax, [esp]
    mov edx, [esp + 4]
    mov ecx, [esp + 8]
    test ecx, ecx
    jnz .Ltome_hook_pop
    mov ecx, [esp + 24]
    add esp, 40
    ret
.Ltome_hook_pop:
    push eax
    mov eax, [esp + 44]
    mov [esp + ecx + 44], eax
    mov eax, [esp + 28]
    mov [esp + ecx + 40], eax
    pop eax
    lea esp, [esp + ecx + 36]
    pop ecx
    ret

    .globl _tome_hook_invoke
_tome_hook_invoke:
    push ebp
    mov ebp, esp
    push esi
    mov esi, [ebp + 12]
    mov ecx, [ebp + 16]
.Ltome_invoke_push:
    test ecx, ecx
    jz .Ltome_invoke_call
    dec ecx
    push dword ptr [esi + ecx * 4]
    jmp .Ltome_invoke_push
.Ltome_invoke_call:
    mov ecx, [ebp + 20]
    mov edx, [ebp + 24]
    mov eax, [ebp + 36]
    call dword ptr [ebp + 8]
    mov esi, [ebp + 28]
    mov [esi], eax
    mov [esi + 4], edx
    mov ecx, [ebp + 32]
    cmp ecx, 1
    jb .Ltome_invoke_done
    ja .Ltome_invoke_double
    fstp dword ptr [esi + 8]
    jmp .Ltome_invoke_done
.Ltome_invoke_double:
    fstp qword ptr [esi + 8]
.Ltome_invoke_done:
    lea esp, [ebp - 4]
    pop esi
    pop ebp
    ret
    .att_syntax prefix
)");

namespace tome::hooks {
namespace {

constexpr size_t kJmpSize = 5;
// entry (10, padded to 16) + moved instructions (at most 4 + 15 source bytes,
// up to 9 bytes each once short branches are widened) + jmp back (5)
constexpr size_t kCodeBlock = 128;
constexpr size_t kCodePage = 4096;

SRWLOCK g_lock = SRWLOCK_INIT;  // the maps and the code allocator
std::vector<std::unique_ptr<Target>> g_owned;  // never freed: the patches stay
std::map<std::string, Target*> g_targets;       // by every name it was hooked by
std::map<uintptr_t, Target*> g_by_address;
std::set<std::string> g_reserved;  // functions a built-in patch owns; mods can't hook/unhook them
// Where those are in Tome's mapping: another name for one, or for a place
// inside a function (its "size"), is reserved too.
struct ReservedRange {
    mappings::Kind kind;
    uint32_t start, end;  // RVAs, end exclusive
};
std::vector<ReservedRange> g_reserved_at;
// Handler::name for handlers hooked by another name than their target's own:
// never freed, since handlers keep pointers into it.
std::set<std::string> g_alias_names;
// What each name hooked before it moved (a mod's file now maps it
// elsewhere, find_or_install): its handlers there stay, and unhook by
// that name still reaches them.
std::map<std::string, std::vector<Target*>> g_former;
uint8_t* g_code_page = nullptr;
size_t g_code_used = kCodePage;
std::atomic<uint64_t> g_seq{0};

uintptr_t g_game_base = 0;
const mappings::Build* g_build = nullptr;

// Tome's mapping, then the mods' own files ("<mod id>/<name>").
const mappings::Symbol* lookup(const std::string& name) {
    if (const mappings::Symbol* sym = g_build ? g_build->find(name) : nullptr) return sym;
    return mod_mappings::find(name);
}

std::string hex_address(uintptr_t address) {
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%08lx", static_cast<unsigned long>(address));
    return buf;
}

// Another name for a function or import slot that's hooked already (Tome's
// name and a mod's own, or two mods'): it shares the patch if it passes its
// arguments the same way and, for code, its bytes match what was there
// before the patch. Under g_lock.
Target* add_alias(const std::string& name, Target& t, mappings::Convention convention, const std::string& signature,
                  const std::vector<int16_t>* bytes, std::string& error) {
    Layout layout;
    if (!layout_for(convention, signature, layout, error)) return nullptr;
    if (!same_layout(layout, t.layout)) {
        error = "the same function as \"" + t.name + "\" (" + mappings::to_string(t.convention) + " \"" +
                t.signature + "\"), which this mapping says takes its arguments another way (" +
                mappings::to_string(convention) + " \"" + signature + "\")";
        return nullptr;
    }
    if (bytes) {
        if (bytes->size() < kJmpSize || !readable(t.address, bytes->size())) {
            error = hex_address(t.address) + " isn't readable code";
            return nullptr;
        }
        // The first bytes are the patch's jmp now; the rest are as they were.
        auto code = reinterpret_cast<const uint8_t*>(t.address);
        for (size_t i = 0; i < bytes->size(); ++i) {
            const uint8_t actual = i < kJmpSize ? t.original[i] : code[i];
            if ((*bytes)[i] >= 0 && actual != (*bytes)[i]) {
                char buf[96];
                snprintf(buf, sizeof(buf), "byte %zu is 0x%02x, expected 0x%02x", i, actual, (*bytes)[i]);
                error = buf;
                return nullptr;
            }
        }
    }
    g_targets[name] = &t;
    TOME_INFO("Hooked %s: the same function as %s", name.c_str(), t.name.c_str());
    return &t;
}

// The code hook whose moved bytes overlap [address, address + size), if any.
const Target* overlapping(uintptr_t address, size_t size) {
    for (const auto& [at, t] : g_by_address) {
        if (t->import || at == address) continue;
        if (at < address + size && address < at + t->moved) return t;
    }
    return nullptr;
}

uint8_t* alloc_code() {
    if (g_code_used + kCodeBlock > kCodePage) {
        g_code_page = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, kCodePage, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!g_code_page) return nullptr;
        g_code_used = 0;
    }
    uint8_t* block = g_code_page + g_code_used;
    g_code_used += kCodeBlock;
    std::memset(block, 0xCC, kCodeBlock);
    return block;
}

void write_jmp(uint8_t* at, const void* to) {
    at[0] = 0xE9;
    uint32_t rel = reinterpret_cast<uintptr_t>(to) - (reinterpret_cast<uintptr_t>(at) + kJmpSize);
    std::memcpy(at + 1, &rel, 4);
}

// Replaces the first 5 bytes with `jmp entry`. When they sit inside one
// aligned 8-byte word (usual for function starts), that word is swapped in
// a single write, so another thread never runs half a jump.
bool write_patch(uintptr_t address, const uint8_t* entry, std::string& error) {
    uint8_t jmp[kJmpSize] = {0xE9};
    uint32_t rel = reinterpret_cast<uintptr_t>(entry) - (address + kJmpSize);
    std::memcpy(jmp + 1, &rel, 4);

    auto code = reinterpret_cast<uint8_t*>(address);
    DWORD old_protect;
    if (!VirtualProtect(code, kJmpSize, PAGE_EXECUTE_READWRITE, &old_protect)) {
        error = "VirtualProtect failed";
        return false;
    }
    note_original(address, kJmpSize);
    const uintptr_t word = address & ~uintptr_t(7);
    if (address - word + kJmpSize <= 8) {
        auto target = reinterpret_cast<volatile LONG64*>(word);
        LONG64 value = *target;
        std::memcpy(reinterpret_cast<uint8_t*>(&value) + (address - word), jmp, kJmpSize);
        InterlockedExchange64(target, value);
    } else {
        std::memcpy(code, jmp, kJmpSize);
    }
    VirtualProtect(code, kJmpSize, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), code, kJmpSize);
    return true;
}

uint32_t fp_kind(const Layout& layout) {
    return layout.result == Result::Float ? kFpFloat : layout.result == Result::Double ? kFpDouble : kFpNone;
}

// How many words past the fixed arguments a variadic function's handlers
// see: kVariadicWords, or fewer near the top of the thread's stack.
uint32_t extra_words(const Layout& layout, const TomeHookFrame& frame) {
    if (!layout.variadic) return 0;
    auto tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const uintptr_t first = reinterpret_cast<uintptr_t>(&frame.stack[layout.stack_slots]);
    const uintptr_t top = reinterpret_cast<uintptr_t>(tib->StackBase);
    const uintptr_t available = top > first ? (top - first) / 4 : 0;
    return static_cast<uint32_t>(available < kVariadicWords ? available : kVariadicWords);
}

struct Original {
    const Target* target;
    const TomeHookFrame* frame;
    const uint32_t* args;  // the dispatcher's array, even if a mod repoints call.args
    uint32_t extra;
    bool called = false;
};

// Calls the original with `args` (fixed arguments, then `extra` variadic words).
TomeHookResult invoke(const Target& t, const TomeHookFrame& frame, const uint32_t* args, uint32_t extra) {
    const Layout& l = t.layout;
    uint32_t stack[kMaxArgWords + kVariadicWords];
    // Registers no parameter uses pass through as the caller set them (eax
    // too: a void function the compiler knows leaves it alone, or a
    // constructor returning `this` in it, gives the caller what it expects).
    uint32_t ecx = frame.ecx, edx = frame.edx;
    for (const Param& p : l.params) {
        switch (p.where) {
        case Where::Ecx: ecx = args[p.index]; break;
        case Where::Edx: edx = args[p.index]; break;
        case Where::Stack:
            for (uint8_t w = 0; w < p.words; ++w) stack[p.slot + w] = args[p.index + w];
            break;
        }
    }
    std::memcpy(stack + l.stack_slots, args + l.arg_words, extra * 4);
    TomeHookResult r = {};
    tome_hook_invoke(t.trampoline, stack, l.stack_slots + extra, ecx, edx, &r, fp_kind(l), frame.eax_in);
    return r;
}

void call_original(void* context, tome_hook_call& call) {
    auto& o = *static_cast<Original*>(context);
    o.called = true;
    TomeHookResult r = invoke(*o.target, *o.frame, o.args, o.extra);
    call.result = r.eax;
    call.result_high = r.edx;
    const Result kind = o.target->layout.result;
    if (kind == Result::Float) call.result_float = r.fp.f;
    if (kind == Result::Double) call.result_float = r.fp.d;
}

// For a mod's REPLACE handler handing on: the game's time isn't the mod's.
void call_original_uncharged(void* context, tome_hook_call& call) {
    struct Args {
        void* context;
        tome_hook_call* call;
    } args{context, &call};
    guard::uncharged([](void* a) {
        auto& x = *static_cast<Args*>(a);
        call_original(x.context, *x.call);
    }, &args);
}

// Every mod handler runs under a guard; a crashed one is abandoned and the
// chain goes on without it.
bool invoke_guarded(const Handler& h, tome_hook_call& call) {
    const char* what = h.phase == TOME_HOOK_BEFORE    ? "before hook"
                       : h.phase == TOME_HOOK_REPLACE ? "replace hook"
                                                      : "after hook";
    // Each handler sees the name it hooked by, also inside another one's
    // call_next (which may have hooked by another name).
    const char* outer_name = call.name;
    if (h.name) call.name = h.name;
    bool ok = true;
    if (!h.mod) {  // Tome's own
        h.fn(h.mod, &call, h.user);
    } else {
        ok = guard::run(h.mod, what, call.name, [&] { h.fn(h.mod, &call, h.user); });
    }
    call.name = outer_name;
    return ok;
}

// Which target `name` hooks: none (unbound), or `target` again.
void bind_name(const std::string& name, Target* target) {
    ExclusiveLock hold(g_lock);
    if (target) {
        g_targets.try_emplace(name, target);
    } else {
        g_targets.erase(name);
    }
}

} // namespace

uint32_t call_with_stack(const void* fn, const uint32_t* args, uint32_t count) {
    TomeHookResult r = {};
    tome_hook_invoke(fn, args, count, 0, 0, &r, kFpNone, 0);
    return r.eax;
}

void init(uintptr_t game_base, const mappings::Build* build) {
    g_game_base = game_base;
    g_build = build;
}

Target* install(const std::string& name, uintptr_t address, mappings::Convention convention,
                const std::string& signature, const std::vector<int16_t>& bytes, std::string& error) {
    ExclusiveLock hold(g_lock);
    Target* result = nullptr;
    char buf[160];
    do {
        if (auto it = g_targets.find(name); it != g_targets.end()) {
            result = it->second;
            break;
        }
        if (auto it = g_by_address.find(address); it != g_by_address.end()) {
            if (it->second->import) {
                error = "an import slot hooked as \"" + it->second->name + "\", not a function";
                break;
            }
            result = add_alias(name, *it->second, convention, signature, &bytes, error);
            break;
        }
        if (const Target* other = overlapping(address, 1)) {
            error = "inside the bytes the hook on \"" + other->name + "\" moved (at " + hex_address(other->address) + ")";
            break;
        }
        auto t = std::make_unique<Target>();
        t->name = name;
        t->address = address;
        t->convention = convention;
        t->signature = signature;
        if (!layout_for(convention, signature, t->layout, error)) break;
        if (bytes.size() < kJmpSize || !readable(address, bytes.size())) {
            error = hex_address(address) + " isn't readable code";
            break;
        }
        // The bytes were checked at startup; check again right before
        // patching, in case something else changed them since.
        auto code = reinterpret_cast<const uint8_t*>(address);
        size_t mismatch = bytes.size();
        for (size_t i = 0; i < bytes.size() && mismatch == bytes.size(); ++i) {
            if (bytes[i] >= 0 && code[i] != bytes[i]) mismatch = i;
        }
        if (mismatch < bytes.size()) {
            snprintf(buf, sizeof(buf), "byte %zu is 0x%02x, expected 0x%02x%s", mismatch, code[mismatch],
                     bytes[mismatch], code[0] == 0xE9 ? " (patched by another tool?)" : "");
            error = buf;
            break;
        }

        if (shade::active()) {
            // Recompiled code: Shade runs the hook and the original. The
            // jmp is still written, so memory reads as it would on Windows.
            Target* raw = t.get();
            const auto entry = reinterpret_cast<uintptr_t>(&tome_hook_entry);
            const auto context = reinterpret_cast<uintptr_t>(raw);
            uintptr_t original = shade::hook_function(address, entry, context);
            uintptr_t stub = original ? shade::make_stub(entry, context) : 0;
            if (!stub) {
                error = hex_address(address) + " isn't the start of a function Shade recompiled";
                break;
            }
            t->moved = kJmpSize;
            std::memcpy(t->original, code, kJmpSize);
            t->trampoline = reinterpret_cast<uint8_t*>(original);
            t->entry = reinterpret_cast<uint8_t*>(stub);
            if (!write_patch(address, t->entry, error)) break;
            TOME_INFO("Hooked %s at 0x%08lx (Shade, %zu arguments)", name.c_str(), static_cast<unsigned long>(address),
                      raw->layout.params.size());
            g_by_address[address] = raw;
            result = raw;
            g_targets[name] = raw;
            g_owned.push_back(std::move(t));
            break;
        }

        uint8_t* block = alloc_code();
        if (!block) {
            error = "out of memory for hook code";
            break;
        }
        // Moved instructions only come from the checked bytes.
        t->entry = block;
        t->trampoline = block + 16;
        std::vector<uint8_t> moved;
        size_t consumed = 0;
        if (!x86::relocate(code, bytes.size(), address, reinterpret_cast<uintptr_t>(t->trampoline), kJmpSize,
                           moved, consumed, error)) {
            g_code_used -= kCodeBlock;  // the last block handed out; give it back
            break;
        }
        if (16 + moved.size() + kJmpSize > kCodeBlock) {
            error = "the moved instructions don't fit the hook's code block";
            g_code_used -= kCodeBlock;
            break;
        }
        if (const Target* other = overlapping(address, consumed)) {
            error = "its first " + std::to_string(consumed) + " bytes would move the start of \"" + other->name +
                    "\" (hooked at " + hex_address(other->address) + ")";
            g_code_used -= kCodeBlock;
            break;
        }
        t->moved = consumed;
        std::memcpy(t->original, code, kJmpSize);
        std::memcpy(t->trampoline, moved.data(), moved.size());
        write_jmp(t->trampoline + moved.size(), code + consumed);
        t->entry[0] = 0x68;  // push imm32
        Target* raw = t.get();
        std::memcpy(t->entry + 1, &raw, 4);
        write_jmp(t->entry + 5, reinterpret_cast<const void*>(&tome_hook_entry));
        FlushInstructionCache(GetCurrentProcess(), block, kCodeBlock);

        if (!write_patch(address, t->entry, error)) {
            g_code_used -= kCodeBlock;
            break;
        }
        TOME_INFO("Hooked %s at 0x%08lx (%zu bytes moved, %zu arguments)", name.c_str(),
                  static_cast<unsigned long>(address), consumed, raw->layout.params.size());
        g_by_address[address] = raw;
        result = raw;
        g_targets[name] = raw;
        g_owned.push_back(std::move(t));
    } while (false);
    return result;
}

Target* install_import(const std::string& name, uintptr_t slot, mappings::Convention convention,
                       const std::string& signature, std::string& error) {
    ExclusiveLock hold(g_lock);
    Target* result = nullptr;
    char buf[160];
    do {
        if (auto it = g_targets.find(name); it != g_targets.end()) {
            result = it->second;
            break;
        }
        if (auto it = g_by_address.find(slot); it != g_by_address.end()) {
            if (!it->second->import) {
                error = "a function hooked as \"" + it->second->name + "\", not an import slot";
                break;
            }
            result = add_alias(name, *it->second, convention, signature, nullptr, error);
            break;
        }
        if (const Target* other = overlapping(slot, 4)) {
            error = "inside the bytes the hook on \"" + other->name + "\" moved";
            break;
        }
        auto t = std::make_unique<Target>();
        t->name = name;
        t->address = slot;
        t->convention = convention;
        t->signature = signature;
        t->import = true;
        if (!layout_for(convention, signature, t->layout, error)) break;
        if (!readable(slot, 4)) {
            error = hex_address(slot) + " isn't a readable import slot";
            break;
        }
        // Calls go on to whatever the slot holds now: the imported function,
        // or another tool's replacement of it.
        auto* entry = reinterpret_cast<volatile LONG*>(slot);
        const uintptr_t original = static_cast<uintptr_t>(*entry);
        if (!readable(original, 1)) {
            snprintf(buf, sizeof(buf), "the import slot holds 0x%08lx, which isn't code",
                     static_cast<unsigned long>(original));
            error = buf;
            break;
        }
        Target* raw = t.get();
        uint8_t* block = nullptr;
        t->trampoline = reinterpret_cast<uint8_t*>(original);
        if (shade::active()) {
            // Code made at run time doesn't run under Shade: it makes the stub.
            t->entry = reinterpret_cast<uint8_t*>(
                shade::make_stub(reinterpret_cast<uintptr_t>(&tome_hook_entry), reinterpret_cast<uintptr_t>(raw)));
            if (!t->entry) {
                error = "Shade made no stub for it";
                break;
            }
        } else {
            block = alloc_code();
            if (!block) {
                error = "out of memory for hook code";
                break;
            }
            t->entry = block;
            t->entry[0] = 0x68;  // push imm32
            std::memcpy(t->entry + 1, &raw, 4);
            write_jmp(t->entry + 5, reinterpret_cast<const void*>(&tome_hook_entry));
            FlushInstructionCache(GetCurrentProcess(), block, kCodeBlock);
        }

        DWORD old_protect;
        if (!VirtualProtect(const_cast<LONG*>(entry), 4, PAGE_READWRITE, &old_protect)) {
            error = "VirtualProtect failed";
            if (block) g_code_used -= kCodeBlock;
            break;
        }
        note_original(slot, 4);
        // One aligned write: another thread calls either the old or the new target.
        InterlockedExchange(entry, static_cast<LONG>(reinterpret_cast<uintptr_t>(t->entry)));
        VirtualProtect(const_cast<LONG*>(entry), 4, old_protect, &old_protect);
        TOME_INFO("Hooked %s at its import slot 0x%08lx (%zu arguments)", name.c_str(),
                  static_cast<unsigned long>(slot), raw->layout.params.size());
        g_by_address[slot] = raw;
        result = raw;
        g_targets[name] = raw;
        g_owned.push_back(std::move(t));
    } while (false);
    return result;
}

Target* find(const std::string& name) {
    SharedLock hold(g_lock);
    auto it = g_targets.find(name);
    return it != g_targets.end() ? it->second : nullptr;
}

void reserve(const std::string& name) {
    ExclusiveLock hold(g_lock);
    g_reserved.insert(name);
    if (const mappings::Symbol* sym = g_build ? g_build->find(name) : nullptr) {
        const uint32_t size = sym->kind == mappings::Kind::Function && sym->size ? sym->size : 1;
        g_reserved_at.push_back({sym->kind, sym->value, sym->value + size});
    }
}

void unreserve(const std::string& name) {
    ExclusiveLock hold(g_lock);
    g_reserved.erase(name);
    if (const mappings::Symbol* sym = g_build ? g_build->find(name) : nullptr) {
        std::erase_if(g_reserved_at, [&](const ReservedRange& r) { return r.kind == sym->kind && r.start == sym->value; });
    }
}

bool is_reserved(const std::string& name) {
    SharedLock hold(g_lock);
    if (g_reserved.count(name)) return true;
    // Another name for a reserved function (a mod's own mapping of it) is reserved too.
    const mappings::Symbol* sym = lookup(name);
    if (!sym || (sym->kind != mappings::Kind::Function && sym->kind != mappings::Kind::Import)) return false;
    return std::any_of(g_reserved_at.begin(), g_reserved_at.end(), [&](const ReservedRange& r) {
        return r.kind == sym->kind && sym->value >= r.start && sym->value < r.end;
    });
}

std::vector<Target*> targets_of(const std::string& name) {
    SharedLock hold(g_lock);
    std::vector<Target*> out;
    if (auto it = g_targets.find(name); it != g_targets.end()) out.push_back(it->second);
    if (auto it = g_former.find(name); it != g_former.end()) {
        for (Target* t : it->second) {
            if (std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
        }
    }
    return out;
}

const char* handler_name(const Target* target, const std::string& name) {
    ExclusiveLock hold(g_lock);
    auto it = g_targets.find(name);
    bool hooks_it = it != g_targets.end() && it->second == target;
    if (auto former = g_former.find(name); !hooks_it && former != g_former.end()) {
        hooks_it = std::find(former->second.begin(), former->second.end(), target) != former->second.end();
    }
    if (!hooks_it) return nullptr;
    if (target->name == name) return target->name.c_str();  // kept as long as the target
    return g_alias_names.insert(name).first->c_str();
}

namespace {

// Hooks `name`, mapped as `sym` (null: not mapped).
Target* install_symbol(const std::string& name, const mappings::Symbol* sym, std::string& error) {
    if (!sym) {
        if (name.find('/') == std::string::npos) {
            error = "not mapped for this build";
        } else if (std::string why = mod_mappings::missing_reason(name); !why.empty()) {
            error = "not mapped now: " + why;
        } else {
            error = "not in that mod's mapping file for this build";
        }
        return nullptr;
    }
    // Tome's mapping was checked at startup, mods' files when they were read
    // (mod_mappings::verify).
    if (sym->kind == mappings::Kind::Import) {
        return install_import(name, g_game_base + sym->value, sym->convention, sym->signature, error);
    }
    if (sym->kind != mappings::Kind::Function) {
        error = std::string("a ") + mappings::to_string(sym->kind) + ", not a function";
        return nullptr;
    }
    return install(name, g_game_base + sym->value, sym->convention, sym->signature, sym->bytes, error);
}

} // namespace

Target* find_or_install(const std::string& name, std::string& error) {
    const mappings::Symbol* sym = lookup(name);
    Target* bound = find(name);
    // Tome's mapping stays as it is; a mod's file can change while the game
    // runs (mod_mappings publishes it anew, or withdraws it), and the name
    // then hooks what the file maps now, or nothing. What was hooked at the
    // old place stays, and stays unhookable by that name.
    if (bound && (sym ? bound->address == g_game_base + sym->value : name.find('/') == std::string::npos)) {
        return bound;
    }
    if (bound && sym) bind_name(name, nullptr);
    Target* t = install_symbol(name, sym, error);
    if (!t && bound && sym) bind_name(name, bound);
    if (t && bound && t != bound) {
        ExclusiveLock hold(g_lock);
        std::vector<Target*>& former = g_former[name];
        if (std::find(former.begin(), former.end(), bound) == former.end()) former.push_back(bound);
    }
    return t;
}

void add(Target* target, Handler handler) {
    handler.seq = ++g_seq;
    if (!handler.name) handler.name = target->name.c_str();
    ExclusiveLock hold(target->lock);
    auto chain = std::make_shared<std::vector<Handler>>(*target->handlers);
    insert(*chain, handler);
    target->handlers = std::move(chain);
}

size_t remove(Target* target, tome_mod* mod, int32_t phase, tome_hook_fn fn, void* user,
              std::vector<const void*>* removed_gates, const std::string* by_name) {
    const char* name = by_name ? handler_name(target, *by_name) : nullptr;
    if (by_name && !name) return 0;  // not a name of this target: nothing was hooked by it here
    auto matches = [&](const Handler& h) {
        return hooks::matches(h, mod, phase, fn, user) && (!by_name || h.name == name);
    };
    ExclusiveLock hold(target->lock);
    auto chain = std::make_shared<std::vector<Handler>>(*target->handlers);
    // Calls that already hold the old chain skip these from now on.
    for (const Handler& h : *chain) {
        if (matches(h) && h.gate) {
            h.gate->revoked = true;
            if (removed_gates) removed_gates->push_back(h.gate.get());
        }
    }
    const size_t removed = std::erase_if(*chain, matches);
    if (removed) target->handlers = std::move(chain);
    return removed;
}

bool remove_gate(Target* target, const Gate* gate) {
    ExclusiveLock hold(target->lock);
    auto chain = std::make_shared<std::vector<Handler>>(*target->handlers);
    const size_t removed = std::erase_if(*chain, [&](const Handler& h) {
        if (h.gate.get() != gate) return false;
        h.gate->revoked = true;
        return true;
    });
    if (removed) target->handlers = std::move(chain);
    return removed > 0;
}

const char* name(const Target* target) {
    return target->name.c_str();
}

bool idle(const std::vector<std::shared_ptr<Gate>>& gates, unsigned wait_ms) {
    const ULONGLONG deadline = GetTickCount64() + wait_ms;
    for (;;) {
        bool others = false, own = false;
        for (const auto& gate : gates) {
            const int mine = running_on_this_thread(*gate);
            own = own || mine > 0;
            others = others || gate->busy.load() > mine;
        }
        if (!others) return !own;
        if (GetTickCount64() >= deadline) return false;
        Sleep(1);
    }
}

} // namespace tome::hooks

using namespace tome::hooks;

extern "C" void tome_hook_dispatch(TomeHookFrame* frame) {
    Target& t = *frame->target;
    const Layout& l = t.layout;
    std::shared_ptr<const std::vector<Handler>> chain;
    {
        tome::SharedLock hold(t.lock);
        chain = t.handlers;
    }
    // A crash guard further out may abandon this call (a mod's handler
    // called the game, which crashed in the original): drop the snapshot.
    tome::OnUnwind release_chain([&] { chain.reset(); });

    uint32_t args[kMaxArgWords + kVariadicWords];
    for (const Param& p : l.params) {
        switch (p.where) {
        case Where::Ecx: args[p.index] = frame->ecx; break;
        case Where::Edx: args[p.index] = frame->edx; break;
        case Where::Stack:
            for (uint8_t w = 0; w < p.words; ++w) args[p.index + w] = frame->stack[p.slot + w];
            break;
        }
    }
    const uint32_t extra = extra_words(l, *frame);
    std::memcpy(args + l.arg_words, frame->stack + l.stack_slots, extra * 4);

    uint32_t eax, edx;
    double fp = 0;
    if (chain->empty()) {
        TomeHookResult r = invoke(t, *frame, args, extra);
        eax = r.eax;
        edx = r.edx;
        fp = l.result == Result::Float ? r.fp.f : r.fp.d;
    } else {
        Original original{&t, frame, args, extra};
        Dispatch d;
        d.call.size = sizeof(d.call);
        d.call.name = t.name.c_str();
        d.call.args = args;
        d.call.arg_count = static_cast<int32_t>(l.arg_words);
        d.call.extra_count = static_cast<int32_t>(extra);
        d.call.return_address = frame->return_address;
        d.handlers = chain->data();
        d.count = chain->size();
        d.original = call_original;
        d.original_handed_on = call_original_uncharged;
        d.context = &original;
        d.invoke = invoke_guarded;
        run(d);
        // A void function the handlers skipped leaves eax as the caller had
        // it, as the original would have (if it leaves eax alone).
        eax = l.result == Result::None && !original.called ? frame->eax_in : d.call.result;
        edx = d.call.result_high;
        fp = d.call.result_float;
    }
    frame->eax = eax;
    // edx is the caller's again unless the result is 64-bit: 1.26a's compiler
    // relies on a leaf function it calls leaving a register alone (callers of
    // RCString::GetText keep `this` in ecx across the call), so a hook must
    // too. tome_hook_entry gives back ecx itself; eax goes into the original.
    frame->edx_out = l.wide_result ? edx : frame->edx;
    frame->pop_bytes = l.pop_bytes;
    frame->fp_kind = fp_kind(l);
    if (l.result == Result::Float) frame->fp.f = static_cast<float>(fp);
    if (l.result == Result::Double) frame->fp.d = fp;
}
