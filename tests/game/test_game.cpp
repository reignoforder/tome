// SPDX-License-Identifier: GPL-3.0-or-later
// The loader against the real game.dll, loaded into this process but never
// run: every check here is deterministic (no window, no clock, no rendering).
// Needs a local install of a build Tome maps (input/<version>: game_dir.h);
// each test says "skipped" without it. game.dll is loaded from the install
// as it is (only read); its Mss32.dll doesn't load .mix files until the game
// starts its sound, so a tome.mix deployed there stays out (checked).
#include "game_dir.h"
#include "harness.h"
#include "test.h"

#include "core/command_card.h"
#include "core/widescreen.h"
#include "events.h"
#include "frames.h"
#include "game_build.h"
#include "handshake.h"
#include "hooks.h"
#include "mappings.h"
#include "mod_manager.h"
#include "mod_menu.h"
#include "mods_ready.h"
#include "natives.h"
#include "patches.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

using host::Home;
using host::Session;
using tome::mod_manager;

namespace {

// game.dll loaded from the install, identified and verified; null (and the
// test skipped or failed) otherwise.
HMODULE load_game(tome::GameBuild& build) {
    std::wstring dir = host::win_path(test::game_dir());
    if (GetFileAttributesW((dir + L"\\game.dll").c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::printf("  skipped: no game.dll in %s\n", test::game_dir().c_str());
        return nullptr;
    }
    SetDllDirectoryW(dir.c_str());
    HMODULE game = LoadLibraryW((dir + L"\\game.dll").c_str());
    if (!game) {
        test::fail(__FILE__, __LINE__, "LoadLibrary(game.dll) failed: error " + std::to_string(GetLastError()));
        return nullptr;
    }
    if (GetModuleHandleW(L"tome.mix")) {
        test::fail(__FILE__, __LINE__, "a tome.mix got loaded along with game.dll");
        return nullptr;
    }
    if (!tome::identify_game_build(game, build) || !build.known) {
        std::printf("  skipped: game.dll isn't a build Tome maps (sha256 %s)\n", build.sha256.c_str());
        return nullptr;
    }
    return game;
}

} // namespace

TEST(game_dll_matches_its_mapping) {
    tome::GameBuild build;
    HMODULE game = load_game(build);
    if (!game) return;
    CHECK_EQ(build.file_version, build.known->data->file_version);
    // The folder's name is the build's (`make test 1.26a` reads input/1.26a).
    // Only when make chose the folder too: GAME_VERSION alone (left in the
    // shell, say) doesn't change what the tests read.
    const char* version = std::getenv("GAME_VERSION");
    const char* dir = std::getenv("TOME_GAME_DIR");
    if (version && *version && dir && *dir) CHECK_EQ(build.known->data->name, std::string(version));
    // Loaded where Windows put it, usually not 0x6F000000: the RVAs have to hold.
    std::vector<std::string> errors;
    CHECK(tome::verify_loaded_build(game, *build.known, errors));
    for (const std::string& e : errors) test::fail(__FILE__, __LINE__, e);
    CHECK(tome::find_build_by_header(game) == build.known);
}

// Everything start_bootstrap (bootstrap.cpp) patches from DllMain, on the
// real code, in its order: every report must say it applied. The one step
// left out, mod_menu::reload_frame_definitions, runs game code (it loads
// Tome's TOC through the game) and needs the game running.
TEST(game_startup_patches_apply) {
    tome::GameBuild build;
    HMODULE game = load_game(build);
    if (!game) return;
    Home home;
    host::open_log(home);
    const uintptr_t base = reinterpret_cast<uintptr_t>(game);
    const tome::BuildMappings& known = *build.known;
    std::string report;

    CHECK(tome::natives::install_hooks(base, known, report));
    CHECK_EQ(report, std::string("redirected 3 of 3 InitNatives call sites"));

    CHECK(tome::patches::install(base, known, report));
    // On 1.27b every patch applies; another build may leave one out, only
    // for want of its symbols (mappings/<build>.json) or because the patch's
    // variant for it says it isn't for that build.
    const std::string& name = known.data->name;
    if (name == "1.27b") CHECK(report.rfind(" applied, 0 skipped: ", std::string::npos) != std::string::npos);
    for (const char* patch : {"memhack-guard", "path-sanitizer", "blp-guard", "d3d-reset-lights", "gl-context-restore",
                              "background-mute", "widescreen", "ambient-sound", "movie-display-reset",
                              "movie-frame-length", "execute-func-guard", "global-string-guard",
                              "lan-map-guard", "jass-token-guard", "command-card-guard"}) {
        const std::string applied = std::string(patch) + " applied";
        const std::string unmapped = std::string(patch) + " skipped (" + name + " has no mapping for ";
        const std::string not_for = std::string(patch) + " skipped (not for " + name + ": ";  // its variant says why
        if (report.find(applied) == std::string::npos && report.find(unmapped) == std::string::npos &&
            report.find(not_for) == std::string::npos) {
            test::fail(__FILE__, __LINE__, applied + ": " + report);
        }
    }
    CHECK(report.find("failed") == std::string::npos);
    {
        // The widescreen patch's 2D part (1.27b, 1.26a): the conversions read
        // Tome's width, not the game's 0.8, which stays as it was (other code
        // reads it); and CreateJassString calls the string guard.
        if (report.find("the UI's proportions") == std::string::npos) test::fail(__FILE__, __LINE__, "widescreen: " + report);
        const uintptr_t ddc_width = base + known.widescreen.ddc_width;
        for (const char* conversion : {"DDCToNDCRect", "NDCToDDCRect", "DDCToNDC", "NDCToDDC", "DDCToNDCWidth",
                                       "NDCToDDCWidth"}) {
            std::vector<size_t> at;
            CHECK(tome::widescreen::constant_operands(
                reinterpret_cast<const uint8_t*>(base + known.data->find(conversion)->value), 0x60,
                static_cast<uint32_t>(ddc_width), at));
            if (!at.empty()) test::fail(__FILE__, __LINE__, std::string(conversion) + " still reads DDCWidth");
        }
        uint32_t low = 0, high = 0;
        std::memcpy(&low, reinterpret_cast<const void*>(ddc_width), 4);
        std::memcpy(&high, reinterpret_cast<const void*>(ddc_width + 4), 4);
        CHECK(low == 0x3F4CCCCDu || (low == 0xA0000000u && high == 0x3FE99999u));  // 0.8f, or 0.8f as a double
        const uintptr_t call = base + known.strings.create_string_call;
        int32_t rel = 0;
        std::memcpy(&rel, reinterpret_cast<const void*>(call + 1), 4);
        CHECK(*reinterpret_cast<const uint8_t*>(call) == 0xE8);
        CHECK(call + 5 + rel != base + known.strings.create_string);
    }
    if (report.find("unclamped") != std::string::npos) test::fail(__FILE__, __LINE__, "BLP header copy: " + report);
    if (report.find("gamma ramp left") != std::string::npos || report.find("gamma restore left") != std::string::npos) {
        test::fail(__FILE__, __LINE__, "movie gamma: " + report);
    }

    CHECK(tome::mods_ready::install_sound_wait(report));

    tome::frames::init(base, known, GetCurrentThreadId());
    if (!tome::mod_menu::install_hooks(base, known, report)) test::fail(__FILE__, __LINE__, "mod menu: " + report);

    if (!tome::events::install(base, known, report)) test::fail(__FILE__, __LINE__, "events: " + report);
    CHECK_EQ(report, std::string("hooked screens, map load, game start and end, frames, game ticks, shutdown, chat, "
                                 "pause, units, spells, damage, experience, players, resources, keys, menus, "
                                 "dialogs, levels and saved games"));
    if (!tome::handshake::install(base, known, report)) test::fail(__FILE__, __LINE__, "handshake: " + report);
    if (!tome::mod_menu::install_game_hooks(report)) test::fail(__FILE__, __LINE__, "Esc menu: " + report);
}

// lan-map-guard on the game's own check of a host's map path
// (MapTransfer::IsValidName).
// The check reads only its transfer object's lock (+0x2C2, a
// CRITICAL_SECTION) and extension list (+0x2DA, ".w3m;.w3x" as the game
// registers it), so a zeroed object with those two will do.
TEST(game_lan_map_guard_refuses_host_paths) {
    tome::GameBuild build;
    HMODULE game = load_game(build);
    if (!game) return;
    const tome::mappings::Symbol* sym = build.known->data->find("MapTransfer::IsValidName");
    if (!sym) {
        std::printf("  skipped: %s doesn't map MapTransfer::IsValidName\n", build.known->data->name.c_str());
        return;
    }
    using IsValidName = int(__thiscall*)(void* self, const char* name);
    const auto is_valid = reinterpret_cast<IsValidName>(reinterpret_cast<uintptr_t>(game) + sym->value);
    alignas(16) static char transfer[0x600];
    std::memset(transfer, 0, sizeof(transfer));
    auto* lock = reinterpret_cast<CRITICAL_SECTION*>(transfer + 0x2C2);
    InitializeCriticalSection(lock);
    std::strcpy(transfer + 0x2DA, ".w3m;.w3x");

    // The game alone (each test is a process of its own: nothing hooked yet)
    // lets '..' and a device name with a second dot through.
    CHECK(!tome::hooks::find("MapTransfer::IsValidName"));
    CHECK_EQ(is_valid(transfer, "..\\..\\Users\\x.w3x"), 1);
    CHECK_EQ(is_valid(transfer, "Maps\\com1.x.w3x"), 1);
    CHECK_EQ(is_valid(transfer, "Maps\\con.w3x"), 0);

    Home home;
    host::open_log(home);
    std::string report;
    CHECK(tome::patches::install(reinterpret_cast<uintptr_t>(game), *build.known, report));
    if (report.find("lan-map-guard applied") == std::string::npos) {
        test::fail(__FILE__, __LINE__, "lan-map-guard: " + report);
        DeleteCriticalSection(lock);
        return;
    }

    // A host's real paths, and the game's own refusals (unchanged).
    CHECK_EQ(is_valid(transfer, "Maps\\FrozenThrone\\(2)EchoIsles.w3x"), 1);
    CHECK_EQ(is_valid(transfer, "Maps\\Download\\my map.w3m"), 1);
    CHECK_EQ(is_valid(transfer, "Maps\\x.txt"), 0);       // extension
    CHECK_EQ(is_valid(transfer, "C:\\Maps\\x.w3x"), 0);   // absolute
    CHECK_EQ(is_valid(transfer, "Maps\\con.w3x"), 0);     // device
    // What only the guard refuses: the game lets these through.
    CHECK_EQ(is_valid(transfer, "..\\..\\Users\\x.w3x"), 0);
    CHECK_EQ(is_valid(transfer, "Maps\\..\\..\\x.w3x"), 0);
    CHECK_EQ(is_valid(transfer, "Maps\\com1.x.w3x"), 0);
    CHECK_EQ(is_valid(transfer, "Maps\\conout$.w3x"), 0);
    CHECK_EQ(is_valid(transfer, ("Maps\\" + std::string(300, 'a') + ".w3x").c_str()), 0);
    CHECK_LOG("lan-map-guard: refused the map path \"..\\..\\Users\\x.w3x\" from a game's host");
    DeleteCriticalSection(lock);
}

// The hook engine on every mapped function's real prologue: each one is
// patched (bytes checked, instructions moved, short branches widened). A
// prologue the x86 decoder gets wrong would crash the game the first time a
// mod hooks it.
TEST(game_every_mapped_function_hooks) {
    tome::GameBuild build;
    HMODULE game = load_game(build);
    if (!game) return;
    Home home;
    host::open_log(home);
    const tome::mappings::Build& data = *build.known->data;
    tome::hooks::init(reinterpret_cast<uintptr_t>(game), &data);
    int hooked = 0;
    for (const tome::mappings::Symbol& sym : data.symbols) {
        if (sym.kind != tome::mappings::Kind::Function) continue;
        std::string error;
        if (tome::hooks::find_or_install(sym.name, error)) {
            ++hooked;
        } else {
            test::fail(__FILE__, __LINE__, sym.name + ": " + error);
        }
    }
    int functions = 0;
    for (const tome::mappings::Symbol& sym : data.symbols) functions += sym.kind == tome::mappings::Kind::Function;
    std::printf("  %d of %d mapped functions hooked\n", hooked, functions);
    CHECK_EQ(hooked, functions);
}

// Two mods' handlers around the game's own RCString::GetText (real code, run
// through Tome's hook): the same order the e2e test checks in the game.
TEST(game_hooks_run_around_real_code) {
    tome::GameBuild build;
    HMODULE game = load_game(build);
    if (!game) return;
    Home home;
    CHECK(home.install_mod("hook-a"));
    CHECK(home.install_mod("hook-b"));
    Session session;
    CHECK(session.start(home, build));
    mod_manager().set_enabled("hook-a", true);
    mod_manager().set_enabled("hook-b", true);
    CHECK_LOG("[hook-b] probe returned \"TomeHookProbe from hook-a\"");
    CHECK_LOG("(5 bytes moved, 1 arguments)");

    mod_manager().set_enabled("hook-a", false);
    mod_manager().set_enabled("hook-b", false);
    mod_manager().set_enabled("hook-b", true);
    CHECK_LOG("[hook-b] probe returned \"TomeHookProbe\"");
}

// A mod's crash in a REPLACE handler of real game code: the original runs.
TEST(game_crash_in_hook_runs_the_real_original) {
    tome::GameBuild build;
    HMODULE game = load_game(build);
    if (!game) return;
    Home home;
    CHECK(home.install_mod("crash"));
    CHECK(home.write(L"tome\\mods\\crash\\mode.txt", "hook"));
    Session session;
    CHECK(session.start(home, build));
    mod_manager().set_enabled("crash", true);
    CHECK_LOG("crash crashed in replace hook RCString::GetText: access violation");
    CHECK_LOG("[crash] probe returned \"TomeCrashProbe\"");
}

namespace {

// A JASS scanner (made by JassScanner::ctor) reading `text`: our own vtable (read a character,
// wrap at the end) and token buffers of kBuffer bytes, larger than the
// scanner's capacity + 1, so what it writes past capacity + 1 lands in them
// and can be seen.
struct Scanner {
    static constexpr size_t kBuffer = 8192;
    alignas(16) uint8_t object[0x400] = {};
    void* vtable[2] = {};
    uint8_t text_buffer[kBuffer];
    uint8_t state_buffer[kBuffer];
    const char* text = "";
    size_t at = 0;
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
Scanner* g_scanner = nullptr;
int __thiscall scanner_read(void*) {
    Scanner& s = *g_scanner;
    return s.text[s.at] ? static_cast<unsigned char>(s.text[s.at++]) : -1;
}
int __thiscall scanner_wrap(void*) { return 1; }
#pragma GCC diagnostic pop

// Reads one token of `text` with the real scanner; `past` gets how many
// bytes it wrote past capacity + 1 in either buffer.
uint32_t scan_one(HMODULE game, const tome::BuildMappings& known, Scanner& s, const char* text, size_t& past) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(game);
    using Ctor = void(__thiscall*)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t);
    using NextToken = uint32_t(__thiscall*)(void*);
    const uint32_t ctor = known.data->find("JassScanner::ctor")->value;
    void* files[4] = {const_cast<char*>("war3map.j"), const_cast<char*>(text), nullptr, nullptr};
    constexpr uint32_t kCapacity = 0x834;
    reinterpret_cast<Ctor>(base + ctor)(s.object, files, 0, 0, 0, kCapacity);
    s.vtable[0] = reinterpret_cast<void*>(&scanner_read);
    s.vtable[1] = reinterpret_cast<void*>(&scanner_wrap);
    void** vt = s.vtable;
    std::memcpy(s.object, &vt, 4);
    std::memset(s.text_buffer, 0xCC, sizeof s.text_buffer);
    std::memset(s.state_buffer, 0xCC, sizeof s.state_buffer);
    uint8_t* text_ptr = s.text_buffer;
    uint8_t* state_ptr = s.state_buffer;
    std::memcpy(s.object + 0x98, &text_ptr, 4);  // the scanner's own buffers stay allocated, unused
    std::memcpy(s.object + 0x8, &state_ptr, 4);
    s.text = text;
    s.at = 0;
    g_scanner = &s;
    const uint32_t token = reinterpret_cast<NextToken>(base + known.data->find("JassScanner::nextToken")->value)(s.object);
    past = 0;
    for (size_t i = kCapacity + 1; i < Scanner::kBuffer; ++i) {
        if (s.text_buffer[i] != 0xCC || s.state_buffer[i] != 0xCC) past = i - kCapacity;
    }
    return token;
}

} // namespace

// jass-token-guard on the real scanner: a 3000-character string literal
// writes past the scanner's 2101-byte buffers without it, and with it the
// scanner returns -2 (the parser stops the compile) and writes nothing past
// them; a short one still scans.
TEST(game_jass_token_guard_stops_long_tokens) {
    tome::GameBuild build;
    HMODULE game = load_game(build);
    if (!game) return;
    const tome::BuildMappings& known = *build.known;
    if (!known.data->find("JassScanner::nextToken.overflow_check") || !known.data->find("JassScanner::ctor")) {
        std::printf("  skipped: %s doesn't map the scanner's length check and constructor\n", known.data->name.c_str());
        return;
    }
    const std::string literal = "\"" + std::string(3000, 'a') + "\"\n";
    static Scanner s;
    size_t past = 0;
    // The game alone (each test is a process of its own: nothing patched yet).
    scan_one(game, known, s, literal.c_str(), past);
    CHECK(past > 0);

    Home home;
    host::open_log(home);
    std::string report;
    CHECK(tome::patches::install(reinterpret_cast<uintptr_t>(game), known, report));
    if (report.find("jass-token-guard applied") == std::string::npos) {
        test::fail(__FILE__, __LINE__, "jass-token-guard: " + report);
        return;
    }
    CHECK_EQ(scan_one(game, known, s, literal.c_str(), past), 0xFFFFFFFEu);
    CHECK_EQ(past, size_t{0});
    CHECK_LOG("jass-token-guard: a script has a token (a string, comment or name) longer than the JASS scanner's "
              "2100 characters");
    // Up to the capacity, a string is a token as before.
    const std::string fits = "\"" + std::string(2000, 'a') + "\"\n";
    const uint32_t token = scan_one(game, known, s, fits.c_str(), past);
    CHECK(token != 0xFFFFFFFEu && token != 0);
    CHECK_EQ(past, size_t{0});
}

namespace {

// What run_site saw after the site ran.
struct SiteRegs {
    uint32_t eax, ecx, esi, esp_before, esp_after;
};
SiteRegs g_site_regs;
constexpr uint32_t kEsiMarker = 0x5E5E5E5E;

void emit(std::vector<uint8_t>& code, std::initializer_list<uint8_t> bytes) { code.insert(code.end(), bytes); }
void emit32(std::vector<uint8_t>& code, const void* p) {
    const uint32_t v = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
    for (int i = 0; i < 4; ++i) code.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void emit32(std::vector<uint8_t>& code, uint32_t v) {
    for (int i = 0; i < 4; ++i) code.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

// Runs the `size` bytes of game.dll code at `site` (copied, as they are now:
// patched or not) the way their function reaches them: ebp at `frame`, eax
// and ecx given, and for a site that ends in the function's pop esi, a pushed
// esi (kEsiMarker) for it to pop. Then falls through to code that records the
// registers and the stack pointer (g_site_regs) and returns. Only for sites
// whose jumps stay inside them.
void run_site(uintptr_t site, size_t size, uint32_t* frame, uint32_t eax, uint32_t ecx, bool pops_esi) {
    std::vector<uint8_t> code;
    emit(code, {0x55, 0x53, 0x56, 0x57});              // push ebp, ebx, esi, edi
    emit(code, {0xbd}); emit32(code, frame);           // mov ebp, frame
    emit(code, {0xb8}); emit32(code, eax);             // mov eax, ...
    emit(code, {0xb9}); emit32(code, ecx);             // mov ecx, ...
    emit(code, {0x89, 0x25}); emit32(code, &g_site_regs.esp_before);  // mov [esp_before], esp
    if (pops_esi) {
        emit(code, {0xbe}); emit32(code, kEsiMarker);  // mov esi, kEsiMarker
        emit(code, {0x56});                            // push esi
    }
    emit(code, {0xbe}); emit32(code, 0u);              // mov esi, 0
    const auto* bytes = reinterpret_cast<const uint8_t*>(site);
    code.insert(code.end(), bytes, bytes + size);
    emit(code, {0xa3}); emit32(code, &g_site_regs.eax);              // mov [eax_out], eax
    emit(code, {0x89, 0x0d}); emit32(code, &g_site_regs.ecx);        // mov [ecx_out], ecx
    emit(code, {0x89, 0x35}); emit32(code, &g_site_regs.esi);        // mov [esi_out], esi
    emit(code, {0x89, 0x25}); emit32(code, &g_site_regs.esp_after);  // mov [esp_after], esp
    if (pops_esi) emit(code, {0x8b, 0x25}), emit32(code, &g_site_regs.esp_before);  // mov esp, [esp_before]
    emit(code, {0x5f, 0x5e, 0x5b, 0x5d, 0xc3});        // pop edi, esi, ebx, ebp; ret
    void* exec = VirtualAlloc(nullptr, code.size(), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!exec) return;
    std::memcpy(exec, code.data(), code.size());
    FlushInstructionCache(GetCurrentProcess(), exec, code.size());
    reinterpret_cast<void (*)()>(exec)();
    VirtualFree(exec, 0, MEM_RELEASE);
}

} // namespace

// command-card-guard's setter rewrites (1.27b), run as game.dll has them:
// each of the three store sequences, copied out of the patched game.dll,
// stores what core/command_card.h's stored() says (the hiding positions
// (0, -11) and (INT_MIN, INT_MIN) too), keeps the registers the function
// uses after it and leaves the stack as it found it. The game's own
// sequences, before the patch, drop the hiding positions.
TEST(game_command_card_guard_rewrites_the_setters) {
    tome::GameBuild build;
    HMODULE game = load_game(build);
    if (!game) return;
    const tome::BuildMappings& known = *build.known;
    const auto& card = known.command_card;
    if (!card.add_check || !card.move_check_button || !card.move_check_data) {
        std::printf("  skipped: %s's setters keep any position (nothing to rewrite)\n", known.data->name.c_str());
        return;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(game);
    constexpr size_t kAddSize = 28, kMoveButtonSize = 43, kMoveDataSize = 30;  // the sites (mappings/1.27b.json)
    constexpr uint32_t kUnset = 0x77777777;
    constexpr uint32_t kButtonData = 0x190;  // CCommandButton.data
    static uint8_t data[0x800];
    static uint8_t button[0x200];
    static uint32_t frame[16];
    static uint32_t pos[2];
    const uint32_t data_addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(data));
    std::memcpy(button + kButtonData, &data_addr, 4);
    auto get = [&](uint32_t offset) {
        uint32_t v = 0;
        std::memcpy(&v, data + offset, 4);
        return v;
    };
    // Runs one site with (x, y) and checks what it stored and left.
    enum Which { kAdd, kMoveButton, kMoveData };
    auto check = [&](Which which, uint32_t x, uint32_t y, bool patched) {
        std::memcpy(data + card.x_offset, &kUnset, 4);
        std::memcpy(data + card.y_offset, &kUnset, 4);
        std::memset(frame, 0, sizeof frame);
        frame[7] = x;  // [ebp+0x1C]
        frame[8] = y;  // [ebp+0x20]
        pos[0] = x;
        pos[1] = y;
        frame[4] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pos));  // [ebp+0x10]
        const char* name = which == kAdd ? "AddButton" : which == kMoveButton ? "SetCommandButtonPos (button)"
                                                                              : "SetCommandButtonPos (data)";
        if (which == kAdd) run_site(base + card.add_check, kAddSize, frame, 0, data_addr, false);
        if (which == kMoveButton) {
            run_site(base + card.move_check_button, kMoveButtonSize, frame,
                     static_cast<uint32_t>(reinterpret_cast<uintptr_t>(button)), 0, true);
        }
        if (which == kMoveData) run_site(base + card.move_check_data, kMoveDataSize, frame, data_addr, 0, false);
        const tome::command_card::Stored want =
            patched ? tome::command_card::stored(x, y) : tome::command_card::Stored{x < 4, y < 3};
        char what[160];
        std::snprintf(what, sizeof what, "%s%s at (%d, %d)", name, patched ? "" : " (the game's)",
                      static_cast<int>(x), static_cast<int>(y));
        if (get(card.x_offset) != (want.x ? x : kUnset) || get(card.y_offset) != (want.y ? y : kUnset)) {
            test::fail(__FILE__, __LINE__, std::string(what) + ": stored (" + std::to_string(get(card.x_offset)) +
                                               ", " + std::to_string(get(card.y_offset)) + ")");
        }
        if (!patched) return;
        // What the function reads after the site: AddButton the data in
        // ecx; SetCommandButtonPos the data in eax and its saved esi popped.
        if (which == kAdd && g_site_regs.ecx != data_addr) test::fail(__FILE__, __LINE__, std::string(what) + ": ecx");
        if (which != kAdd && g_site_regs.eax != data_addr) test::fail(__FILE__, __LINE__, std::string(what) + ": eax");
        if (which == kMoveButton && g_site_regs.esi != kEsiMarker) {
            test::fail(__FILE__, __LINE__, std::string(what) + ": esi not popped");
        }
        if (g_site_regs.esp_after != g_site_regs.esp_before) test::fail(__FILE__, __LINE__, std::string(what) + ": esp");
    };
    struct Pos {
        uint32_t x, y;
    };
    const Pos positions[] = {{2, 1}, {0, 0}, {3, 2}, {0, static_cast<uint32_t>(-11)}, {0x80000000u, 0x80000000u},
                             {5, 1}, {1, 7}, {0xFFFFFFFFu, 0}, {0x80000000u, 0}, {0, 0x80000000u}};
    // The game's own: (its button path jumps out of the site for a position
    // outside the grid, so only the other two run here).
    for (const Pos& p : positions) {
        check(kAdd, p.x, p.y, false);
        check(kMoveData, p.x, p.y, false);
    }

    Home home;
    host::open_log(home);
    std::string report;
    CHECK(tome::patches::install(base, known, report));
    if (report.find("command-card-guard applied") == std::string::npos ||
        report.find("keeps the hiding positions") == std::string::npos) {
        test::fail(__FILE__, __LINE__, "command-card-guard: " + report);
        return;
    }
    for (const Pos& p : positions) {
        for (Which w : {kAdd, kMoveButton, kMoveData}) check(w, p.x, p.y, true);
    }
}
