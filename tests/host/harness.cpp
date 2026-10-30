#include "harness.h"

#include "core/json.h"
#include "core/mappings.h"
#include "events.h"
#include "log.h"
#include "mappings.h"
#include "mod_manager.h"
#include "mods_ready.h"
#include "natives.h"
#include "overlay.h"
#include "test.h"

#include <windows.h>

#include <cstring>
#include <map>

#ifndef TOME_TEST_BUILD_DIR
#error "TOME_TEST_BUILD_DIR must be set (tests/CMakeLists.txt)"
#endif

namespace host {
namespace {

std::map<std::string, void*> g_bound;  // BindNative's latest thunk per name
unsigned g_engine_inits = 0;
std::wstring g_log_path;

void __attribute__((fastcall)) fake_bind_native(void* fn, const char* name, const char* signature) {
    (void)signature;
    g_bound[name] = fn;
}

uint32_t fake_init_natives() {
    ++g_engine_inits;
    return 0;
}

// The 1.27b mapping file, parsed from the source tree: it names the SHA-256
// the embedded copy is looked up by.
const tome::mappings::Build* source_mappings() {
    static tome::mappings::Build build;
    static bool parsed = [] {
        std::string text;
        if (!tome::read_file(win_path(TOME_TEST_SOURCE_DIR "/mappings/1.27b.json"), text)) return false;
        tome::json::Value root;
        if (!tome::json::parse(text, root).ok) return false;
        return tome::mappings::parse_build(root, build).ok;
    }();
    return parsed ? &build : nullptr;
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

bool exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

} // namespace

std::wstring win_path(const std::string& path) {
    std::wstring out = tome::widen(path);
    if (!out.empty() && out[0] == L'/') out = L"Z:" + out;
    for (wchar_t& c : out) {
        if (c == L'/') c = L'\\';
    }
    return out;
}

// --- FakeGame ---

bool FakeGame::create() {
    const tome::mappings::Build* data = source_mappings();
    if (!data) return false;
    mappings_ = tome::find_build_by_sha256(data->sha256.c_str());
    if (!mappings_) return false;
    // Zeroed, so data the loader reads (the game instance, the focused
    // frame, ...) reads as "none" rather than as garbage.
    void* image = VirtualAlloc(nullptr, mappings_->data->size_of_image, MEM_RESERVE | MEM_COMMIT,
                               PAGE_EXECUTE_READWRITE);
    if (!image) return false;
    base_ = reinterpret_cast<uintptr_t>(image);

    // Every mapped function's checked bytes, so Tome's hooks install
    // (events::install hooks dozens); none of them runs but these:
    for (const tome::mappings::Symbol& sym : mappings_->data->symbols) {
        if (sym.kind == tome::mappings::Kind::Function) put_bytes(sym.name.c_str());
    }
    if (!put_slash_command()) return false;
    if (!put_bytes("RCString::GetText")) return false;
    if (!put_jump("InitNatives", reinterpret_cast<const void*>(&fake_init_natives))) return false;
    if (!put_jump("BindNative", reinterpret_cast<const void*>(&fake_bind_native))) return false;
    for (uint32_t rva : mappings_->jass.init_natives_calls) {
        auto site = reinterpret_cast<uint8_t*>(base_ + rva);
        int32_t rel = static_cast<int32_t>(base_ + mappings_->jass.init_natives - (base_ + rva + 5));
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
static uint32_t g_cookie = 0;

bool FakeGame::put_slash_command() {
    const tome::mappings::Symbol* sym = mappings_->data->find("CGameUI::HandleSlashCommand");
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
    const tome::mappings::Symbol* sym = mappings_->data->find(symbol);
    if (!sym) return false;
    auto at = reinterpret_cast<uint8_t*>(base_ + sym->value);
    for (size_t i = 0; i < sym->bytes.size(); ++i) {
        at[i] = sym->bytes[i] < 0 ? 0 : static_cast<uint8_t>(sym->bytes[i]);
    }
    return true;
}

bool FakeGame::put_jump(const char* symbol, const void* target) {
    const tome::mappings::Symbol* sym = mappings_->data->find(symbol);
    if (!sym) return false;
    auto at = reinterpret_cast<uint8_t*>(base_ + sym->value);
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (base_ + sym->value + 5));
    at[0] = 0xE9;
    std::memcpy(at + 1, &rel, 4);
    return true;
}

tome::GameBuild FakeGame::build() const {
    tome::GameBuild out;
    out.base = base_;
    out.path = L"game.dll";
    out.file_version = mappings_->data->file_version;
    out.pe_timestamp = mappings_->data->pe_timestamp;
    out.sha256 = mappings_->data->sha256;
    out.known = mappings_;
    return out;
}

uintptr_t FakeGame::address(const char* symbol) const {
    const tome::mappings::Symbol* sym = mappings_->data->find(symbol);
    return sym ? base_ + sym->value : 0;
}

void FakeGame::load_map(int n) const {
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

// --- Home ---

Home::Home() {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    wchar_t dir[64];
    swprintf(dir, 64, L"tome-host-%lu-%lu", GetCurrentProcessId(), GetTickCount());
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

Home::~Home() {
    // Kept for a look when the test failed (its path is in the log's first lines).
    if (test::failures() == 0) remove_tree(paths_.home_dir);
}

bool Home::install_mod(const char* folder) {
    std::wstring name = tome::widen(folder);
    std::wstring built = win_path(TOME_TEST_BUILD_DIR "/mods/") + name;
    std::wstring fixture = win_path(TOME_TEST_SOURCE_DIR "/tests/e2e/mods/") + name;
    std::wstring from = host::exists(built) ? built : fixture;
    return copy_tree(from, paths_.mods_dir + L"\\" + name);
}

bool Home::write(const std::wstring& relative, const std::string& text) {
    return tome::write_file_atomic(paths_.home_dir + L"\\" + relative, text);
}

bool Home::read(const std::wstring& relative, std::string& text) const {
    return tome::read_file(paths_.home_dir + L"\\" + relative, text);
}

bool Home::remove(const std::wstring& relative) {
    std::wstring path = paths_.home_dir + L"\\" + relative;
    if (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_DIRECTORY) {
        remove_tree(path);
        return !host::exists(path);
    }
    return DeleteFileW(path.c_str()) != 0;
}

bool Home::exists(const std::wstring& relative) const {
    return host::exists(paths_.home_dir + L"\\" + relative);
}

bool Home::touch(const std::wstring& relative, int seconds) {
    HANDLE file = CreateFileW((paths_.home_dir + L"\\" + relative).c_str(), FILE_WRITE_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER t;
    t.LowPart = now.dwLowDateTime;
    t.HighPart = now.dwHighDateTime;
    t.QuadPart += static_cast<ULONGLONG>(seconds) * 10000000ull;
    FILETIME when{t.LowPart, t.HighPart};
    bool ok = SetFileTime(file, nullptr, nullptr, &when) != 0;
    CloseHandle(file);
    return ok;
}

// --- the log ---

void open_log(const Home& home) {
    g_log_path = home.paths().tome_dir + L"\\tome.log";
    tome::log::open(g_log_path);
    tome::log::keep_lines(true);
    TOME_INFO("Test home: %s", tome::narrow(home.paths().home_dir).c_str());
}

bool logged(std::string_view text) {
    size_t from = 0;
    return tome::log::find_kept(from, text);
}

std::string log_text() {
    std::string text;
    tome::read_file(g_log_path, text);
    return text;
}

// --- Session ---

bool Session::start(const Home& home) {
    if (!game.create()) {
        open_log(home);
        TOME_ERROR("Test: couldn't lay out the fake game image");
        return false;
    }
    return start(home, game.build());
}

bool Session::start(const Home& home, const tome::GameBuild& build) {
    open_log(home);
    tome::overlay::init(home.paths());
    std::string report;
    if (!tome::natives::install_hooks(build.base, *build.known, report)) {
        TOME_ERROR("Test: natives not hooked: %s", report.c_str());
        return false;
    }
    TOME_INFO("JASS natives: %s", report.c_str());
    if (events) {
        if (!tome::events::install(build.base, *build.known, report)) {
            TOME_ERROR("Test: events not hooked: %s", report.c_str());
            return false;
        }
        TOME_INFO("Events: %s", report.c_str());
    }
    tome::ModManager& mods = tome::mod_manager();
    if (!safe_mode.empty()) mods.set_safe_mode(safe_mode);
    mods.init(home.paths(), build);
    mods.refresh();
    tome::mods_ready::set();
    return true;
}

} // namespace host
