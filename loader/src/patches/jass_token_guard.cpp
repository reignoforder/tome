// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * JASS token guard (docs/known-bugs.md, "Long string literal overruns the
 * JASS compiler's buffer"; docs/reverse-engineering.md, JASS compiler).
 *
 * The JASS scanner (JassScanner::nextToken, a flex-style DFA) keeps the
 * token it reads in two heap buffers of JassScanner.capacity + 1 bytes (the
 * text and each character's state; Jass::compile makes it with 2100). Its
 * character loop checks the length against the capacity, but the exit it
 * takes then (return -2: the parser stops the compile) also needs a flag the
 * function clears when it starts, so the check never fires: a string literal,
 * comment or name longer than 2100 characters in war3map.j writes past both
 * buffers while the map loads. ~4000 characters crashed the game; shorter
 * overruns corrupt the heap silently.
 *
 * Fix: the check's conditional jump (JassScanner::nextToken.overflow_check,
 * `cmp [reg+0x1C], 0; jne exit`) becomes a plain jump to the same exit, the
 * scanner's own error path: a token that fills the buffers ends the compile
 * as a syntax error does, before anything is written past them. The jump's
 * target is read from the instruction and checked to be that exit (return
 * -2) before anything is written. An AFTER handler on nextToken logs it.
 *
 * That error path was never safe: the parser's constructor leaves
 * JassParser.root unset, only a finished parse sets it, and after a failed
 * one the caller destroys whatever it holds -- stack garbage (a crash, seen
 * in game). So JassParser::parse (BEFORE) clears it first: a stopped parse
 * then destroys nothing, and the map's script just doesn't compile.
 *
 * Built in as a patch, not a mod, so mods can't turn it off (patches.h).
 */
#include "patches.h"

#include "hooks.h"
#include "log.h"
#include "patch.h"
#include "shade.h"
#include "tome/tome.h"

#include <cstdint>
#include <cstring>
#include <string>

#pragma GCC diagnostic ignored "-Wattributes"

namespace tome::patches {
namespace {

constexpr uint32_t kTokenTooLong = 0xFFFFFFFEu;  // nextToken's -2

// The check and its exit as each build compiled them.
struct Check {
    uint8_t cmp[4];        // cmp dword [reg+0x1C], 0
    const uint8_t* exit;   // the first bytes at the jne's target
    size_t exit_size;
};
// 1.27b: cmp [edi+0x1C], 0; the exit pops edi and esi, then mov eax, -2.
const uint8_t kExit127b[] = {0x5f, 0x5e, 0xb8, 0xfe, 0xff, 0xff, 0xff};
const Check kCheck127b = {{0x83, 0x7f, 0x1c, 0x00}, kExit127b, sizeof kExit127b};
// 1.26a: cmp [esi+0x1C], 0; the exit pops edi, esi and ebp, then mov eax, -2.
const uint8_t kExit126a[] = {0x5f, 0x5e, 0x5d, 0xb8, 0xfe, 0xff, 0xff, 0xff};
const Check kCheck126a = {{0x83, 0x7e, 0x1c, 0x00}, kExit126a, sizeof kExit126a};

uint32_t g_capacity_offset = 0;
uint32_t g_root_offset = 0;
unsigned long g_refused = 0;  // the compiler runs on the main thread

// BEFORE JassParser::parse(this, context): no root until the parse sets one.
void TOME_CALL on_parse(tome_mod*, tome_hook_call* call, void*) {
    if (call->arg_count < 1 || !call->args[0]) return;
    const uint32_t none = 0;
    std::memcpy(reinterpret_cast<uint8_t*>(call->args[0]) + g_root_offset, &none, sizeof none);
}

// AFTER JassScanner::nextToken(this) -> the token's code.
void TOME_CALL on_next_token(tome_mod*, tome_hook_call* call, void*) {
    if (call->result != kTokenTooLong || call->arg_count < 1 || !call->args[0]) return;
    int32_t capacity = 0;
    std::memcpy(&capacity, reinterpret_cast<const uint8_t*>(call->args[0]) + g_capacity_offset, sizeof capacity);
    ++g_refused;
    TOME_WARN("jass-token-guard: a script has a token (a string, comment or name) longer than the JASS scanner's "
              "%d characters; the game stops compiling it rather than write past its buffer (total %lu)",
              capacity, g_refused);
}

bool install_jass_token_guard(uintptr_t base, const BuildMappings& build, const Variant& variant, std::string& report) {
    const Check& check = *static_cast<const Check*>(variant.params);
    g_capacity_offset = build.scanner.capacity_offset;
    g_root_offset = build.scanner.root_offset;
    // cmp [reg+0x1C], 0 (4 bytes), jne rel32 (0f 85, 6 bytes).
    const uintptr_t site = base + build.scanner.overflow_check;
    constexpr size_t kSize = 10;
    if (!readable(site, kSize)) {
        report = "the length check isn't readable";
        return false;
    }
    uint8_t expected[kSize];
    std::memcpy(expected, reinterpret_cast<const void*>(site), kSize);
    if (std::memcmp(expected, check.cmp, sizeof check.cmp) != 0 || expected[4] != 0x0f || expected[5] != 0x85) {
        report = "the length check isn't as expected";
        return false;
    }
    int32_t rel = 0;
    std::memcpy(&rel, expected + 6, sizeof rel);
    const uintptr_t exit = site + kSize + static_cast<intptr_t>(rel);
    if (!readable(exit, check.exit_size) || std::memcmp(reinterpret_cast<const void*>(exit), check.exit, check.exit_size) != 0) {
        report = "the length check's exit isn't nextToken's return -2";
        return false;
    }
    // Under Shade the jump can't be written (the code is recompiled): the
    // hooks would only slow every compile down, so none are installed.
    if (shade::active()) {
        report = "the length check: code patches don't apply under Shade (the game's code is recompiled)";
        return false;
    }
    std::string error;
    hooks::Target* token = hooks::find_or_install("JassScanner::nextToken", error);
    if (!token) {
        report = "JassScanner::nextToken: " + error;
        return false;
    }
    hooks::Target* parse = hooks::find_or_install("JassParser::parse", error);
    if (!parse) {
        report = "JassParser::parse: " + error;
        return false;
    }
    // The root first: the length check below may stop a parse from now on.
    hooks::Handler before;
    before.phase = TOME_HOOK_BEFORE;
    before.priority = INT32_MAX;
    before.fn = on_parse;
    hooks::add(parse, before);
    hooks::Handler after;
    after.phase = TOME_HOOK_AFTER;
    after.priority = INT32_MIN;  // after any mod's handler
    after.fn = on_next_token;
    hooks::add(token, after);
    // jmp rel32 to the same exit, then NOPs over the rest of the check.
    uint8_t replacement[kSize];
    std::memset(replacement, 0x90, kSize);
    replacement[0] = 0xe9;
    const int32_t jmp_rel = static_cast<int32_t>(exit - (site + 5));
    std::memcpy(replacement + 1, &jmp_rel, sizeof jmp_rel);
    if (!patch_bytes(site, expected, replacement, kSize, error)) {
        // Not in force (the write itself failed): the handlers come off
        // again rather than stay behind a failed patch.
        hooks::remove(parse, nullptr, TOME_HOOK_BEFORE, on_parse, nullptr);
        hooks::remove(token, nullptr, TOME_HOOK_AFTER, on_next_token, nullptr);
        report = "the length check: " + error;
        return false;
    }
    report = "the JASS scanner stops at a token too long for its buffers";
    return true;
}

const char* const kSymbols[] = {"JassScanner::nextToken", "JassScanner::nextToken.overflow_check",
                                "JassScanner.capacity", "JassParser::parse", "JassParser.root", nullptr};
const char* const kReserve[] = {"JassScanner::nextToken", "JassParser::parse", nullptr};
const char* const k126a[] = {"1.26a", nullptr};
const Variant kVariants[] = {
    {k126a, nullptr, nullptr, &kCheck126a},
    {nullptr, nullptr, nullptr, &kCheck127b},
};

} // namespace

extern const Patch kJassTokenGuard = {
    "jass-token-guard",
    "stops the JASS compiler at a token too long for its buffers instead of overrunning them",
    kVariants,  // the check's register and its exit
    kSymbols,
    kReserve,   // (memhack-guard reserves it too)
    nullptr,    // needed everywhere
    install_jass_token_guard,
    true,  // sync: changes what map scripts do
};

} // namespace tome::patches
