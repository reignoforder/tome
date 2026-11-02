// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Path sanitizer for JASS natives that take a file path (docs/known-bugs.md,
 * "PreloadGenEnd file writes"; docs/reverse-engineering.md).
 *
 * A downloaded map can create a file from a map-controlled name through
 * PreloadGenEnd (writes the accumulated Preload buffer), InitGameCache
 * (opens/creates a cache file) and the save natives SaveGame, CopySaveGame and
 * Rename/RemoveSaveDirectory (which also move and delete). On 1.27b the write
 * lands inside the game folder (PreloadGenEnd: CustomMapData\, saves:
 * save\<profile>\; Blizzard moved it to the user-data folder in 1.28.1), so a
 * traversing or absolute name -- "..\\..\\Start
 * Menu\\...\\x.bat", "C:\\...", a UNC share -- lets the map plant a file
 * anywhere the process can write: a persistence / code-execution primitive,
 * not just a cheat. The natives that read a file by name are covered too:
 * Preloader runs a file as script, LoadGame and ChangeLevel load a save or a
 * map, StartMeleeAI/StartCampaignAI load an AI script, and the model, texture,
 * movie and sound natives load assets, so none of them can reach outside the
 * data folder either (to run a planted file, probe for one, or open a device).
 * The fix refuses such a name, and one longer than path_guard::kMaxPathLen,
 * which the game would truncate (for PreloadGenEnd, one too long to fit after
 * the game's folder, which can crash the game); a plain relative name (the
 * natives' intended use, subdirectories included) is untouched. Preload's
 * argument may be any length: maps use it as a data channel
 * (path_guard::is_unsafe_any_length).
 *
 * Mechanism (build-independent, no native addresses hardcoded): hook the
 * mapped BindNative(fn, name, signature), which the engine's InitNatives
 * calls for every built-in native on every map load. When it binds one of our
 * targets, remember the engine's real fn and substitute a wrapper of ours as
 * the bound `fn`. The VM then calls the wrapper exactly as it would the native
 * (same __cdecl ABI, string arg as an RCString*); the wrapper reads the paths,
 * and either calls the real native or skips the call and returns 0. Because
 * we key off the native's NAME at bind time, this needs only the BindNative
 * mapping and automatically covers any build that has it.
 *
 * Built in as a patch, not a mod, so it's always in force (patches.h). The
 * BindNative hook is deliberately NOT reserved: the threat model is malicious
 * maps, not mods, and a mod is native code that can bypass any API boundary
 * anyway (see hooks::reserve), so reserving BindNative would only cost
 * legitimate mods a useful hook for no real security gain.
 */
#include "patches.h"

#include "core/path_guard.h"
#include "hooks.h"
#include "log.h"
#include "tome/tome.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

// GCC warns that __thiscall (TOME_CALL) is meant for class methods, but it
// does apply it to these free functions (as in events.cpp and mod_menu.cpp).
#pragma GCC diagnostic ignored "-Wattributes"

namespace tome::patches {
namespace {

using StringTextFn = const char*(__thiscall*)(const void* string);

// Every native in 1.27b's common.j that takes a file path, with its argument
// count and which arguments are paths (bit i = argument i). All are __cdecl,
// one 32-bit word per parameter (a string is an RCString*, see
// docs/reverse-engineering.md "Native ABI"), and all return nothing, a
// boolean, an integer or a handle, so a refused call returns 0: nothing
// happened / false / 0 / null.
//
// Writes (create, overwrite, move or delete a file) also refuse an empty
// name, which has no target. Reads allow one: maps pass "" on purpose, e.g. a
// special effect with no model. Data (only Preload) is a read whose argument
// may also be any length: Preload keeps its string as is, and maps use it to
// carry data into the file PreloadGenEnd writes (codeless save/load).
enum class Access { Write, Read, Data };
// in_custom_map_data: the game puts the name after "<game folder>\\CustomMapData\\"
// in a MAX_PATH buffer (PreloadGenEnd), so its limit is path_guard::
// max_name_after that prefix, not kMaxPathLen.
struct Spec {
    const char* name;
    int args;
    unsigned paths;
    Access access;
    bool in_custom_map_data = false;
};
constexpr Spec kSpecs[] = {
    // Writes. 1.27b has no CreateSaveDirectory.
    {"PreloadGenEnd", 1, 0b1, Access::Write, true},   // (filename): writes the Preload buffer
    {"InitGameCache", 1, 0b1, Access::Write},         // (campaignFile): opens/creates a cache file
    {"SaveGame", 1, 0b1, Access::Write},              // (saveFileName)
    {"CopySaveGame", 2, 0b11, Access::Write},         // (sourceSaveName, destSaveName)
    {"RenameSaveDirectory", 2, 0b11, Access::Write},  // (sourceDirName, destDirName)
    {"RemoveSaveDirectory", 1, 0b1, Access::Write},   // (sourceDirName)
    // Files read as script or game state.
    {"Preloader", 1, 0b1, Access::Read},         // (filename): runs the file as script
    {"LoadGame", 2, 0b1, Access::Read},          // (saveFileName, doScoreScreen)
    {"SaveGameExists", 1, 0b1, Access::Read},    // (saveName)
    {"ChangeLevel", 2, 0b1, Access::Read},       // (newLevel, doScoreScreen): loads a map
    {"StartMeleeAI", 2, 0b10, Access::Read},     // (player, script)
    {"StartCampaignAI", 2, 0b10, Access::Read},  // (player, aiscript)
    // Models, textures, movies and sounds.
    {"Preload", 1, 0b1, Access::Data},                         // (filename): also a data channel
    {"SetDayNightModels", 2, 0b11, Access::Read},              // (terrainDNCFile, unitDNCFile)
    {"SetSkyModel", 1, 0b1, Access::Read},                     // (skyModelFile)
    {"SetIntroShotModel", 1, 0b1, Access::Read},               // (introModelPath)
    {"PlayModelCinematic", 1, 0b1, Access::Read},              // (modelName)
    {"PlayCinematic", 1, 0b1, Access::Read},                   // (movieName)
    {"SetAltMinimapIcon", 1, 0b1, Access::Read},               // (iconPath)
    {"CreateTrackable", 4, 0b1, Access::Read},                 // (trackableModelPath, x, y, facing)
    {"QuestSetIconPath", 2, 0b10, Access::Read},               // (quest, iconPath)
    {"MultiboardSetItemsIcon", 2, 0b10, Access::Read},         // (lb, iconPath)
    {"MultiboardSetItemIcon", 2, 0b10, Access::Read},          // (mbi, iconFileName)
    {"SetCinematicCamera", 1, 0b1, Access::Read},              // (cameraModelFile)
    {"SetCineFilterTexture", 1, 0b1, Access::Read},            // (filename)
    {"CreateSound", 7, 0b1, Access::Read},                     // (fileName, looping, is3D, ...)
    {"CreateSoundFilenameWithLabel", 7, 0b1, Access::Read},    // (fileName, looping, is3D, ...)
    {"SetMapMusic", 3, 0b1, Access::Read},                     // (musicName, random, index)
    {"PlayMusic", 1, 0b1, Access::Read},                       // (musicName)
    {"PlayMusicEx", 3, 0b1, Access::Read},                     // (musicName, frommsecs, fadeinmsecs)
    {"PlayThematicMusic", 1, 0b1, Access::Read},               // (musicFileName)
    {"PlayThematicMusicEx", 2, 0b1, Access::Read},             // (musicFileName, frommsecs)
    {"GetSoundFileDuration", 1, 0b1, Access::Read},            // (musicFileName)
    {"AddSpecialEffect", 3, 0b1, Access::Read},                // (modelName, x, y)
    {"AddSpecialEffectLoc", 2, 0b1, Access::Read},             // (modelName, where)
    {"AddSpecialEffectTarget", 3, 0b1, Access::Read},          // (modelName, targetWidget, attachPointName)
    {"AddSpellEffectTarget", 4, 0b1, Access::Read},            // (modelName, t, targetWidget, attachPoint)
    {"CreateImage", 11, 0b1, Access::Read},                    // (file, sizeX, ..., imageType)
};
constexpr size_t kCount = sizeof(kSpecs) / sizeof(kSpecs[0]);

// Bound once at install from the build's mappings (not shared with
// natives.cpp, so the patch doesn't depend on install order).
StringTextFn g_string_text = nullptr;

// The engine's real natives, captured at bind time, indexed like kSpecs.
// Written on the main thread during InitNatives (map load), read on the main
// thread while a map script runs -- binding always precedes the first call,
// so `volatile` (as in natives.cpp) is enough and no lock is needed.
void* volatile g_orig[kCount] = {};
unsigned long g_refused_total = 0;

// The longest name PreloadGenEnd may take here (path_guard::max_name_after the
// game's own "<game folder>\\CustomMapData\\"), set at install.
size_t g_custom_map_data_max = path_guard::kMaxPathLen;

// A map can call these natives in a loop with an arbitrary (long, multi-line)
// name: log the first refusals in full, then only every kLogEvery-th, and
// print at most kLogChars of the name with control characters replaced, so a
// map can neither flood tome.log nor forge lines in it. The first refusal of
// each native for each reason is logged too, whatever came before it (at most
// 2 * kCount lines), so one is never lost behind other refusals.
constexpr unsigned long kLogFirst = 20;
constexpr unsigned long kLogEvery = 1000;
constexpr size_t kLogChars = 160;
enum Reason { kTooLong, kEscapes, kReasons };
bool g_logged[kCount][kReasons] = {};

// True if a path argument (an RCString*) of kSpecs[i] escapes the data
// folder or is too long, and the call must be skipped. A null string passes
// through: the engine handles it, and there's nothing to sanitize.
bool escapes(size_t i, uint32_t jass_string) {
    const Spec& spec = kSpecs[i];
    const char* path = jass_string ? g_string_text(reinterpret_cast<const void*>(jass_string)) : nullptr;
    if (!path || (spec.access != Access::Write && !*path)) return false;
    const std::string_view text(path);
    const size_t max = spec.access == Access::Data  ? SIZE_MAX
                       : spec.in_custom_map_data ? g_custom_map_data_max
                                                 : path_guard::kMaxPathLen;
    const bool too_long = text.size() > max;
    if (!too_long && (spec.access == Access::Data ? !path_guard::is_unsafe_any_length(text)
                                                  : !path_guard::is_unsafe(text))) {
        return false;
    }
    ++g_refused_total;
    const Reason reason = too_long ? kTooLong : kEscapes;
    const bool first_of_kind = !g_logged[i][reason];
    g_logged[i][reason] = true;
    if (first_of_kind || g_refused_total <= kLogFirst || g_refused_total % kLogEvery == 0) {
        char shown[kLogChars + 1];
        size_t n = 0;
        for (; n < kLogChars && path[n]; ++n) {
            auto c = static_cast<unsigned char>(path[n]);
            shown[n] = (c < 0x20 || c == 0x7f) ? '?' : path[n];
        }
        shown[n] = '\0';
        TOME_WARN("path-sanitizer: refused %s(\"%s%s\") -- %s (total %lu)", spec.name, shown, path[n] ? "..." : "",
                  reason == kTooLong ? "too long" : "escapes the data folder", g_refused_total);
    }
    return true;
}

// True if any path argument of kSpecs[i] must be refused. Checks (and logs)
// every path, so a refused CopySaveGame names both.
bool refuse(size_t i, const uint32_t* args) {
    const Spec& spec = kSpecs[i];
    bool refused = false;
    for (int a = 0; a < spec.args; ++a) {
        if (spec.paths & (1u << a)) refused = escapes(i, args[a]) || refused;
    }
    return refused;
}

// The bound wrapper for kSpecs[I]: the native's own ABI (Args 32-bit words),
// so it forwards exactly the arguments it got, or returns 0 when refused.
template <size_t I, class Seq>
struct Wrapper;
template <size_t I, size_t... A>
struct Wrapper<I, std::index_sequence<A...>> {
    using Fn = uint32_t(__cdecl*)(decltype(A, uint32_t{})...);
    static uint32_t __cdecl call(decltype(A, uint32_t{})... args) {
        auto orig = reinterpret_cast<Fn>(g_orig[I]);
        const uint32_t words[] = {args...};
        if (!orig || refuse(I, words)) return 0;
        return orig(args...);
    }
};

template <size_t I>
constexpr void* wrapper() {
    static_assert(kSpecs[I].args >= 1 && kSpecs[I].args <= 16 && kSpecs[I].paths != 0 &&
                  kSpecs[I].paths < (1u << kSpecs[I].args));
    return reinterpret_cast<void*>(&Wrapper<I, std::make_index_sequence<kSpecs[I].args>>::call);
}

template <size_t... I>
std::array<void*, kCount> make_wrappers(std::index_sequence<I...>) {
    return {wrapper<I>()...};
}
const std::array<void*, kCount> kWrappers = make_wrappers(std::make_index_sequence<kCount>{});

// BEFORE BindNative(void* fn, const char* name, const char* signature)
// (fastcall: fn=args[0], name=args[1], signature=args[2]). For a target
// native, remember the engine's real fn and bind our wrapper instead. Mods
// hook later, so a mod's BEFORE handler at INT32_MIN (equal priorities run in
// registration order) or a REPLACE handler still runs after this one and could
// rebind the native: that's accepted, since mods aren't the threat (above). The
// engine re-binds with the real fn on every map load, so this re-captures the
// same pointer each time; guarding against our own wrapper keeps `orig`
// correct even if that ever changes.
void TOME_CALL on_bind_native(tome_mod*, tome_hook_call* call, void*) {
    if (call->arg_count < 3) return;
    const char* name = reinterpret_cast<const char*>(call->args[1]);
    if (!name) return;
    for (size_t i = 0; i < kCount; ++i) {
        if (std::strcmp(name, kSpecs[i].name) != 0) continue;
        void* fn = reinterpret_cast<void*>(call->args[0]);
        if (fn != kWrappers[i]) g_orig[i] = fn;
        call->args[0] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(kWrappers[i]));
        return;
    }
}

// The length of the folder PreloadGenEnd writes in, worked out as the game
// does it (PreloadGen::End, docs/reverse-engineering.md): war3.exe's path cut
// after its last separator, through GetFullPathNameA, then "CustomMapData\\".
// 0 if it can't be known.
size_t custom_map_data_prefix_len() {
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 0;
    char* sep = nullptr;
    for (char* c = exe; *c; ++c) {
        if (*c == '\\' || *c == '/') sep = c;
    }
    if (!sep) return 0;
    sep[1] = '\0';
    char full[MAX_PATH];
    n = GetFullPathNameA(exe, MAX_PATH, full, nullptr);
    if (n == 0 || n >= MAX_PATH) return 0;
    return n + std::strlen("CustomMapData\\");
}

bool install_path_sanitizer(uintptr_t game_base, const BuildMappings& build, std::string& report) {
    g_string_text = reinterpret_cast<StringTextFn>(game_base + build.jass.string_get_text);
    if (size_t prefix = custom_map_data_prefix_len()) {
        g_custom_map_data_max = path_guard::max_name_after(prefix);
    } else {
        TOME_WARN("path-sanitizer: can't tell the length of the game folder; PreloadGenEnd names over %zu refused",
                  path_guard::max_name_after(MAX_PATH / 2));
        g_custom_map_data_max = path_guard::max_name_after(MAX_PATH / 2);
    }
    std::string error;
    hooks::Target* bind = hooks::find_or_install("BindNative", error);
    if (!bind) {
        report = "BindNative: " + error;
        return false;
    }
    hooks::Handler before;
    before.phase = TOME_HOOK_BEFORE;
    before.priority = INT32_MIN;  // after mods' BEFORE handlers of higher priority (see on_bind_native)
    before.fn = on_bind_native;
    hooks::add(bind, before);
    report = "sanitizes paths of " + std::to_string(kCount) + " file natives (PreloadGenEnd names up to " +
             std::to_string(g_custom_map_data_max) + " characters)";
    return true;
}

// RCString::GetText reads the path argument; without it the patch couldn't
// check anything, so require it too.
const char* const kSymbols[] = {"BindNative", "RCString::GetText", nullptr};

} // namespace

// extern: const at namespace scope is internal linkage by default; patches.cpp
// references this across translation units.
extern const Patch kPathSanitizer = {
    "path-sanitizer",
    "refuses path traversal in every JASS native that takes a file path",
    nullptr,    // any build that maps the symbols below
    kSymbols,   // symbols it needs mapped
    nullptr,    // reserves nothing: mods may still hook BindNative (see file header)
    nullptr,    // needed everywhere
    install_path_sanitizer,
};

} // namespace tome::patches
