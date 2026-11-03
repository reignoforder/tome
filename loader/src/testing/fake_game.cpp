// SPDX-License-Identifier: GPL-3.0-or-later
#include "testing/fake_game.h"

#include "core/mappings.h"
#include "events.h"
#include "frames.h"
#include "log.h"
#include "mappings.h"
#include "mod_manager.h"
#include "mods_ready.h"
#include "natives.h"
#include "overlay.h"
#include "paths.h"
#include "storm.h"

#include <windows.h>

#include <cstring>
#include <deque>
#include <map>

namespace tome::testing {
namespace {

std::map<std::string, void*> g_bound;  // BindNative's latest thunk per name
unsigned g_engine_inits = 0;

// CGameWar3's instance while a map runs: zeroed, so whatever Tome reads of
// it (the map info) reads as "none".
alignas(16) char g_game_object[0x1000];

void __attribute__((fastcall)) fake_bind_native(void* fn, const char* name, const char* signature) {
    (void)signature;
    g_bound[name] = fn;
}

uint32_t fake_init_natives() {
    ++g_engine_inits;
    return 0;
}

// String arguments: what RCString::GetText reads, [[this+8]+0x1C].
struct FakeRep {
    char unknown[0x1C];
    const char* text;
};
struct FakeRCString {
    char unknown[8];
    const FakeRep* rep;
};
struct FakeArg {
    std::string text;
    FakeRep rep{};
    FakeRCString string{};
};
std::deque<FakeArg> g_string_args;  // a deque: their addresses stay put

// String results: ids from 1, as the game's string table has them (0 is null).
// A deque: string_result's pointers stay valid as more are made (a vector
// would move short strings, whose text lives inside the std::string).
std::deque<std::string> g_strings;

uint32_t __attribute__((fastcall)) fake_create_string(const char* text) {
    g_strings.push_back(text ? text : "");
    return static_cast<uint32_t>(g_strings.size());
}

// Code values (natives::jass_code_call, jass_code_execute): each trigger
// runs one code value, as Tome uses them.
uint32_t g_next_handle = 0x100000;
std::map<uint32_t, uint32_t> g_trigger_code;    // trigger -> code
std::map<uint32_t, uint32_t> g_condition_code;  // conditionfunc -> code
std::map<uint32_t, unsigned> g_code_runs;
std::map<uint32_t, bool> g_code_results;

uint32_t fake_create_trigger() {
    return ++g_next_handle;
}
uint32_t fake_destroy_trigger(uint32_t trigger) {
    g_trigger_code.erase(trigger);
    return 0;
}
uint32_t fake_condition(uint32_t code) {
    uint32_t handle = ++g_next_handle;
    g_condition_code[handle] = code;
    return handle;
}
uint32_t fake_trigger_add_condition(uint32_t trigger, uint32_t condition) {
    auto it = g_condition_code.find(condition);
    if (it == g_condition_code.end()) return 0;
    g_trigger_code[trigger] = it->second;
    return ++g_next_handle;
}
uint32_t fake_trigger_evaluate(uint32_t trigger) {
    auto it = g_trigger_code.find(trigger);
    if (it == g_trigger_code.end()) return 0;
    ++g_code_runs[it->second];
    auto result = g_code_results.find(it->second);
    return result == g_code_results.end() || result->second ? 1 : 0;
}
uint32_t fake_trigger_add_action(uint32_t trigger, uint32_t code) {
    g_trigger_code[trigger] = code;
    return ++g_next_handle;
}
uint32_t fake_trigger_execute(uint32_t trigger) {
    auto it = g_trigger_code.find(trigger);
    if (it != g_trigger_code.end()) ++g_code_runs[it->second];
    return 0;
}

// CGameUI::HandleSlashCommand's stack cookie (put_slash_command).
uint32_t g_cookie = 0;

} // namespace

std::wstring win_path(const std::string& path) {
    std::wstring out = widen(path);
    if (!out.empty() && out[0] == L'/') out = L"Z:" + out;
    for (wchar_t& c : out) {
        if (c == L'/') c = L'\\';
    }
    return out;
}

bool copy_tree(const std::wstring& from, const std::wstring& to) {
    CreateDirectoryW(to.c_str(), nullptr);
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((from + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ok = copy_tree(from + L"\\" + name, to + L"\\" + name) && ok;
        } else {
            ok = CopyFileW((from + L"\\" + name).c_str(), (to + L"\\" + name).c_str(), FALSE) && ok;
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return ok;
}

void remove_tree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            std::wstring path = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                remove_tree(path);
            } else {
                SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(path.c_str());
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    RemoveDirectoryW(dir.c_str());
}

bool path_exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// --- FakeGame ---

bool FakeGame::create() {
    mappings_ = find_build_by_name("1.27b");
    if (!mappings_) return false;
    // Zeroed, so data the loader reads (the game instance, the focused
    // frame, ...) reads as "none" rather than as garbage.
    void* image = VirtualAlloc(nullptr, mappings_->data->size_of_image, MEM_RESERVE | MEM_COMMIT,
                               PAGE_EXECUTE_READWRITE);
    if (!image) return false;
    base_ = reinterpret_cast<uintptr_t>(image);
    set_edition(Edition::FrozenThrone);

    // Every mapped function's checked bytes, so Tome's hooks install
    // (events::install hooks dozens); none of them runs but these:
    for (const mappings::Symbol& sym : mappings_->data->symbols) {
        if (sym.kind == mappings::Kind::Function) put_bytes(sym.name.c_str());
    }
    if (!put_slash_command()) return false;
    if (!put_bytes("RCString::GetText")) return false;
    if (!put_jump("InitNatives", reinterpret_cast<const void*>(&fake_init_natives))) return false;
    if (!put_jump("BindNative", reinterpret_cast<const void*>(&fake_bind_native))) return false;
    const auto& jass = mappings_->jass;
    const std::pair<uint32_t, const void*> fakes[] = {
        {jass.create_string, reinterpret_cast<const void*>(&fake_create_string)},
        {jass.create_trigger, reinterpret_cast<const void*>(&fake_create_trigger)},
        {jass.destroy_trigger, reinterpret_cast<const void*>(&fake_destroy_trigger)},
        {jass.condition, reinterpret_cast<const void*>(&fake_condition)},
        {jass.trigger_add_condition, reinterpret_cast<const void*>(&fake_trigger_add_condition)},
        {jass.trigger_evaluate, reinterpret_cast<const void*>(&fake_trigger_evaluate)},
        {jass.trigger_add_action, reinterpret_cast<const void*>(&fake_trigger_add_action)},
        {jass.trigger_execute, reinterpret_cast<const void*>(&fake_trigger_execute)},
    };
    for (const auto& [rva, fn] : fakes) {
        if (!put_jump_at(rva, fn)) return false;
    }
    // CGameWar3::GetPlayer (thiscall, ret 4) finds no player in any slot, so
    // once load_map sets the game instance, what Tome reads of players (the
    // selection, names, resources) is "none" instead of running the zeroed
    // rest of the function.
    const uint32_t get_player = mappings_->play.get_player;
    if (!get_player || get_player + 5 > mappings_->data->size_of_image) return false;
    const uint8_t no_player[] = {0x33, 0xC0, 0xC2, 0x04, 0x00};  // xor eax, eax; ret 4
    std::memcpy(reinterpret_cast<uint8_t*>(base_ + get_player), no_player, sizeof(no_player));
    for (uint32_t rva : jass.init_natives_calls) {
        auto site = reinterpret_cast<uint8_t*>(base_ + rva);
        int32_t rel = static_cast<int32_t>(base_ + jass.init_natives - (base_ + rva + 5));
        site[0] = 0xE8;
        std::memcpy(site + 1, &rel, 4);
        site[5] = 0xC3;
    }
    FlushInstructionCache(GetCurrentProcess(), image, mappings_->data->size_of_image);
    return true;
}

// CGameUI::HandleSlashCommand as a whole function that handles nothing
// (returns 0), for the lines Tome's hook passes on to the game: its mapped
// prologue, its stack cookie read from g_cookie, and an epilogue of ours.
bool FakeGame::put_slash_command() {
    const mappings::Symbol* sym = mappings_->data->find("CGameUI::HandleSlashCommand");
    if (!sym) return false;
    auto at = reinterpret_cast<uint8_t*>(base_ + sym->value);
    size_t n = sym->bytes.size();
    for (size_t i = 0; i < n; ++i) at[i] = sym->bytes[i] < 0 ? 0 : static_cast<uint8_t>(sym->bytes[i]);
    // 55 8b ec 81 ec 4c 01 00 00 a1 [cookie] 33 c5 89 45 fc 53 8b: mov eax, [cookie] at +9.
    if (n != 21 || at[9] != 0xA1 || at[20] != 0x8B) return false;
    const uint32_t cookie = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_cookie));
    std::memcpy(at + 10, &cookie, 4);
    const uint8_t tail[] = {
        0xD9,              // (8b) d9: mov ebx, ecx
        0x5B,              // pop ebx
        0x33, 0xC0,        // xor eax, eax: not handled
        0x8B, 0xE5,        // mov esp, ebp
        0x5D,              // pop ebp
        0xC2, 0x04, 0x00,  // ret 4 (thiscall, the text)
    };
    std::memcpy(at + n, tail, sizeof(tail));
    return true;
}

bool FakeGame::put_bytes(const char* symbol) {
    const mappings::Symbol* sym = mappings_->data->find(symbol);
    if (!sym) return false;
    auto at = reinterpret_cast<uint8_t*>(base_ + sym->value);
    for (size_t i = 0; i < sym->bytes.size(); ++i) {
        at[i] = sym->bytes[i] < 0 ? 0 : static_cast<uint8_t>(sym->bytes[i]);
    }
    return true;
}

bool FakeGame::put_jump_at(uint32_t rva, const void* target) {
    if (!rva || rva + 5 > mappings_->data->size_of_image) return false;
    auto at = reinterpret_cast<uint8_t*>(base_ + rva);
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (base_ + rva + 5));
    at[0] = 0xE9;
    std::memcpy(at + 1, &rel, 4);
    return true;
}

bool FakeGame::put_jump(const char* symbol, const void* target) {
    const mappings::Symbol* sym = mappings_->data->find(symbol);
    return sym && put_jump_at(sym->value, target);
}

GameBuild FakeGame::build() const {
    GameBuild out;
    out.base = base_;
    out.path = L"game.dll";
    out.file_version = mappings_->data->file_version;
    out.pe_timestamp = mappings_->data->pe_timestamp;
    out.sha256 = mappings_->data->sha256;
    out.known = mappings_;
    out.edition = read_edition(base_, mappings_);
    return out;
}

void FakeGame::set_edition(Edition edition) const {
    *reinterpret_cast<int32_t*>(base_ + mappings_->game.expansion) = static_cast<int32_t>(edition);
}

uintptr_t FakeGame::address(const char* symbol) const {
    const mappings::Symbol* sym = mappings_->data->find(symbol);
    return sym ? base_ + sym->value : 0;
}

void FakeGame::load_map(int n) {
    *reinterpret_cast<void**>(base_ + mappings_->game.game_instance) = g_game_object;
    using Fn = void (*)();
    reinterpret_cast<Fn>(base_ + mappings_->jass.init_natives_calls[n])();
}

unsigned FakeGame::engine_init_count() const {
    return g_engine_inits;
}

// GCC warns that thiscall is meant for methods, but applies it (as in loader/src).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
int FakeGame::chat(const char* text) const {
    using Fn = int(__attribute__((thiscall)) *)(void* game_ui, const char* text);
    static char game_ui[0x400];  // never read: the fake handles nothing
    return reinterpret_cast<Fn>(address("CGameUI::HandleSlashCommand"))(game_ui, text);
}
#pragma GCC diagnostic pop

void* FakeGame::native(const char* name) const {
    auto it = g_bound.find(name);
    return it == g_bound.end() ? nullptr : it->second;
}

uint32_t FakeGame::string_arg(const char* text) {
    FakeArg& arg = g_string_args.emplace_back();
    arg.text = text ? text : "";
    arg.rep.text = arg.text.c_str();
    arg.string.rep = &arg.rep;
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&arg.string));
}

const char* FakeGame::string_result(uint32_t id) {
    return id >= 1 && id <= g_strings.size() ? g_strings[id - 1].c_str() : nullptr;
}

unsigned FakeGame::code_runs(uint32_t code) {
    auto it = g_code_runs.find(code);
    return it == g_code_runs.end() ? 0 : it->second;
}

void FakeGame::set_code_result(uint32_t code, bool result) {
    g_code_results[code] = result;
}

// --- Home ---

Home::Home(const wchar_t* prefix) {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    wchar_t dir[96];
    swprintf(dir, 96, L"%ls-%lu-%lu", prefix, GetCurrentProcessId(), GetTickCount());
    paths_.home_dir = std::wstring(temp) + dir;
    paths_.game_dir = paths_.home_dir;
    paths_.tome_dir = paths_.home_dir + L"\\tome";
    paths_.mods_dir = paths_.tome_dir + L"\\mods";
    paths_.mod_config_dir = paths_.mods_dir + L"\\config";
    paths_.packages_dir = paths_.tome_dir + L"\\packages";
    paths_.host_exe = L"war3.exe";
    CreateDirectoryW(paths_.home_dir.c_str(), nullptr);
    CreateDirectoryW(paths_.tome_dir.c_str(), nullptr);
    CreateDirectoryW(paths_.mods_dir.c_str(), nullptr);
}

void Home::remove_leftovers(const wchar_t* prefix) {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    std::wstring start = std::wstring(prefix) + L"-";
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((std::wstring(temp) + start + L"*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        // Only names Home gives (<prefix>-<process id>-<time>): never another
        // folder in %TEMP% that merely starts with the prefix.
        const wchar_t* rest = fd.cFileName + start.size();
        wchar_t* end = nullptr;
        DWORD pid = wcstoul(rest, &end, 10);
        if (end == rest || *end != L'-') continue;
        const wchar_t* time = end + 1;
        wcstoul(time, &end, 10);
        if (end == time || *end != L'\0') continue;
        std::wstring dir = std::wstring(temp) + fd.cFileName;
        // Still its process's if one with that id runs and started before the
        // folder was made (Wine reuses process ids at once).
        if (HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            FILETIME created, exited, kernel, user;
            bool owner = WaitForSingleObject(process, 0) == WAIT_TIMEOUT &&
                         GetProcessTimes(process, &created, &exited, &kernel, &user) &&
                         CompareFileTime(&created, &fd.ftCreationTime) <= 0;
            CloseHandle(process);
            if (owner) continue;
        }
        if (!path_exists(dir + L"\\tome\\tome.log")) remove_tree(dir);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

bool install(const Home& home, std::wstring from, std::wstring* name) {
    while (from.size() > 3 && from.back() == L'\\') from.pop_back();
    std::wstring folder = from.substr(from.find_last_of(L'\\') + 1);
    if (name) *name = folder;
    std::wstring to = home.paths().mods_dir + L"\\" + folder;
    DWORD attributes = GetFileAttributesW(from.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) ? copy_tree(from, to) : CopyFileW(from.c_str(), to.c_str(), FALSE) != 0;
}

// --- start ---

std::wstring log_path(const Home& home) {
    return home.paths().tome_dir + L"\\tome.log";
}

void open_log(const Home& home) {
    log::open(log_path(home));
    log::keep_lines(true);
    TOME_INFO("Test home: %s", narrow(home.paths().home_dir).c_str());
}

bool start(const Home& home, const GameBuild& build, const StartOptions& options) {
    open_log(home);
    if (!build.known) {
        TOME_ERROR("Test: no game image");
        return false;
    }
    frames::set_main_thread(GetCurrentThreadId());
    if (options.fake_storm) storm::use_fake_for_test();
    overlay::init(home.paths());
    std::string report;
    if (!natives::install_hooks(build.base, *build.known, report)) {
        TOME_ERROR("Test: natives not hooked: %s", report.c_str());
        return false;
    }
    TOME_INFO("JASS natives: %s", report.c_str());
    if (options.events) {
        if (!events::install(build.base, *build.known, report)) {
            TOME_ERROR("Test: events not hooked: %s", report.c_str());
            return false;
        }
        TOME_INFO("Events: %s", report.c_str());
    }
    ModManager& mods = mod_manager();
    if (!options.safe_mode.empty()) mods.set_safe_mode(options.safe_mode);
    mods.init(home.paths(), build);
    mods.refresh();
    mods_ready::set();
    return true;
}

} // namespace tome::testing
