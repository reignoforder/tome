// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Global string guard (docs/known-bugs.md, "JASS natives that crash or
 * misbehave").
 *
 * A global whose initial value calls a native that returns a string
 *   globals
 *       string s = GetLocalizedString("COLON_GOLD")   // or I2S(...), SubString(...)
 * crashes the game while the map loads, constant or not (checked in game on
 * 1.27b, Nov 2026; natives returning integers are fine there). Every such
 * native returns its string through CreateJassString, which adds it to the
 * script VM's string table: JassVM::Get(CGameWar3.vm_index) -> JassVM.strings.
 * While the globals block runs, that VM isn't registered yet, so
 * JassVM::Get returns null and the game reads [null + JassVM.strings]
 * (access violation reading 0x2858 at game.dll+0x91AF8B).
 *
 * Fix: CreateJassString's call to CGameWar3::CreateString goes to a check
 * instead (patch_call: no hook, so the many string natives a map calls pay
 * one lookup each). When there's no game, no VM or no string table it
 * returns 0 -- the null string, which every string native may return -- and
 * the global is null instead of the game crashing; otherwise it calls
 * CreateString as before. Mods' hooks on CreateJassString still work (the
 * call is "??" in its mapped bytes) and run before the check.
 */
#include "patches.h"

#include "log.h"
#include "patch.h"

#include <cstdint>

#pragma GCC diagnostic ignored "-Wattributes"

namespace tome::patches {
namespace {

using VmGetFn = void*(__fastcall*)(int index);
using CreateStringFn = uint32_t(__thiscall*)(void* game, const char* text);

VmGetFn g_vm_get = nullptr;
CreateStringFn g_create_string = nullptr;
uint32_t g_vm_index_offset = 0;
uint32_t g_vm_strings_offset = 0;
unsigned long g_skipped = 0;

// Whether CreateString would find the string table it writes to.
bool string_table_ready(const char* game) {
    if (!game) return false;
    const int index = *reinterpret_cast<const int*>(game + g_vm_index_offset);
    const char* vm = static_cast<const char*>(g_vm_get(index));
    return vm && *reinterpret_cast<void* const*>(vm + g_vm_strings_offset);
}

// In place of CGameWar3::CreateString(game, text), called by CreateJassString
// (text isn't null: it returned 0 for that already).
uint32_t __thiscall create_string(void* game, const char* text) {
    if (string_table_ready(static_cast<const char*>(game))) return g_create_string(game, text);
    ++g_skipped;
    if (g_skipped <= 20 || g_skipped % 1000 == 0) {
        TOME_WARN("global-string-guard: a native returned a string before the map script's string table exists "
                  "(a string native in a global's initial value); it returns null instead of crashing the game "
                  "(total %lu)", g_skipped);
    }
    return 0;
}

bool install(uintptr_t game_base, const BuildMappings& build, const Variant&, std::string& report) {
    if (!build.strings.create_string || !build.strings.create_string_call) {
        report = "CGameWar3::CreateString or its call in CreateJassString isn't mapped";
        return false;
    }
    g_vm_get = reinterpret_cast<VmGetFn>(game_base + build.objects.jass_vm_get);
    g_vm_index_offset = build.objects.vm_index_offset;
    g_vm_strings_offset = build.objects.vm_strings_offset;
    g_create_string = reinterpret_cast<CreateStringFn>(game_base + build.strings.create_string);
    std::string error;
    if (!patch_call(game_base + build.strings.create_string_call, game_base + build.strings.create_string,
                    reinterpret_cast<const void*>(&create_string), error)) {
        report = "CreateJassString.call.create_string: " + error;
        return false;
    }
    report = "string natives in global initial values return null instead of crashing";
    return true;
}

const char* const kSymbols[] = {"CreateJassString.call.create_string", "CGameWar3::CreateString", "JassVM::Get",
                                "CGameWar3.vm_index", "JassVM.strings", nullptr};

} // namespace

extern const Patch kGlobalStringGuard = {
    "global-string-guard",
    "keeps a string native in a global's initial value from crashing the map load",
    {},
    kSymbols,
    nullptr,  // reserves nothing: mods' own handlers on CreateJassString run before the check
    nullptr,
    install,
};

} // namespace tome::patches
