// tome_test.dll: sdk/include/tome/tome_test.h, the real loader against the
// fake game of testing/fake_game.h, for mods' own tests.
#define TOME_TEST_BUILDING
#include <tome/tome_test.h>

#include "core/json.h"
#include "events.h"
#include "keybinds.h"
#include "log.h"
#include "mod_manager.h"
#include "paths.h"
#include "testing/fake_game.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>

using tome::testing::FakeGame;

namespace {

std::unique_ptr<tome::testing::Home> g_home;
FakeGame g_game;
bool g_started = false;
bool g_stopped = false;
size_t g_log_from = 0;     // tome_test_clear_log: kept lines before this are forgotten
std::string g_text;        // the string the last call returned
std::deque<float> g_reals;  // real arguments: their addresses stay put
int g_failures = 0;

tome::testing::Home& home() {
    if (!g_home) {
        tome::testing::Home::remove_leftovers(L"tome-test");
        g_home = std::make_unique<tome::testing::Home>(L"tome-test");
    }
    return *g_home;
}

std::wstring log_path() {
    return tome::testing::log_path(home());
}

bool find_status(const char* id, tome::ModStatus& out) {
    if (!g_started || !id) return false;
    for (tome::ModStatus& s : tome::mod_manager().status()) {
        if (s.id == id) {
            out = std::move(s);
            return true;
        }
    }
    return false;
}

const char* keep(std::string text) {
    g_text = std::move(text);
    return g_text.c_str();
}

void remove_home_if_passed() {
    if (!g_home || g_failures != 0) return;
    tome::log::close();
    g_home->remove();
}

} // namespace

extern "C" {

// --- Setting up ---

int TOME_CALL tome_test_install(const char* path) {
    if (!path) return 0;
    if (g_started) {
        std::printf("  tome_test_install(%s): install mods before tome_test_start\n", path);
        return 0;
    }
    bool ok = tome::testing::install(home(), tome::testing::win_path(path));
    if (!ok) std::printf("  tome_test_install(%s): couldn't copy it\n", path);
    return ok ? 1 : 0;
}

const char* TOME_CALL tome_test_folder(void) {
    return keep(tome::narrow(home().paths().home_dir));
}

int TOME_CALL tome_test_start(void) {
    if (g_started) return 1;
    if (!g_game.create()) {
        std::printf("  tome_test_start: couldn't lay out the fake game\n");
        return 0;
    }
    tome::testing::StartOptions options;
    options.events = true;
    options.fake_storm = true;
    if (!tome::testing::start(home(), g_game.build(), options)) {
        std::printf("  tome_test_start: Tome didn't start; see %s\n", tome::narrow(log_path()).c_str());
        return 0;
    }
    g_started = true;
    std::atexit(remove_home_if_passed);
    // Allowed and turned on, as a player who installed them would.
    for (const tome::ModStatus& s : tome::mod_manager().status()) {
        if (!s.id.empty()) tome::mod_manager().set_enabled(s.id, true);
    }
    return 1;
}

void TOME_CALL tome_test_stop(void) {
    if (!g_started || g_stopped) return;
    g_stopped = true;
    tome::mod_manager().shutdown();
}

// --- Mods ---

const char* TOME_CALL tome_test_state(const char* id) {
    tome::ModStatus s;
    return find_status(id, s) ? keep(s.state) : "missing";
}

const char* TOME_CALL tome_test_reason(const char* id) {
    tome::ModStatus s;
    if (!find_status(id, s)) return "";
    std::string reason = s.reason;
    for (const std::string& e : s.errors) {
        if (reason.find(e) == std::string::npos) reason += (reason.empty() ? "" : "; ") + e;
    }
    return keep(reason);
}

int TOME_CALL tome_test_enable(const char* id, int enabled) {
    tome::ModStatus s;
    if (!find_status(id, s) || g_stopped) return 0;
    tome::mod_manager().set_enabled(id, enabled != 0);
    return 1;
}

int TOME_CALL tome_test_reload(const char* id) {
    if (!id || !g_started || g_stopped) return 0;
    return tome::mod_manager().reload(id).empty() ? 0 : 1;
}

// --- The log ---

int TOME_CALL tome_test_logged(const char* text) {
    size_t from = g_log_from;
    return text && tome::log::find_kept(from, text) ? 1 : 0;
}

unsigned TOME_CALL tome_test_log_count(const char* text) {
    if (!text) return 0;
    unsigned count = 0;
    size_t from = g_log_from;
    while (tome::log::find_kept(from, text)) ++count;
    return count;
}

void TOME_CALL tome_test_clear_log(void) {
    g_log_from = tome::log::kept_size();
}

const char* TOME_CALL tome_test_log(void) {
    std::string text;
    if (g_home) tome::read_file(log_path(), text);
    return keep(std::move(text));
}

// --- JASS ---

void TOME_CALL tome_test_load_map(void) {
    if (g_started) g_game.load_map();
}

int TOME_CALL tome_test_has_native(const char* name) {
    return name && g_started && g_game.native(name) ? 1 : 0;
}

int TOME_CALL tome_test_call(const char* name, const uint32_t* args, size_t count, uint32_t* result) {
    if (result) *result = 0;
    if (!name || !g_started || count > 16 || (count && !args)) return 0;
    void* native = g_game.native(name);
    if (!native) return 0;
    // cdecl, the caller pops: a native that takes fewer never sees the rest.
    uint32_t a[16] = {};
    for (size_t i = 0; i < count; ++i) a[i] = args[i];
    using Fn = uint32_t (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                            uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    uint32_t value = reinterpret_cast<Fn>(native)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9],
                                                  a[10], a[11], a[12], a[13], a[14], a[15]);
    if (result) *result = value;
    return 1;
}

uint32_t TOME_CALL tome_test_real(float value) {
    g_reals.push_back(value);
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_reals.back()));
}

uint32_t TOME_CALL tome_test_string(const char* text) {
    return FakeGame::string_arg(text);
}

const char* TOME_CALL tome_test_string_result(uint32_t value) {
    return FakeGame::string_result(value);
}

unsigned TOME_CALL tome_test_code_runs(uint32_t code) {
    return FakeGame::code_runs(code);
}

void TOME_CALL tome_test_set_code_result(uint32_t code, int result) {
    FakeGame::set_code_result(code, result != 0);
}

// --- Playing ---

int TOME_CALL tome_test_chat(const char* line) {
    return line && g_started ? g_game.chat(line) : 0;
}

void TOME_CALL tome_test_event(int32_t event) {
    if (!g_started) return;
    tome_event_info info = {};
    info.event = event;  // size 0: the defaults
    tome::events::dispatch_for_test(info);
}

void TOME_CALL tome_test_event_defaults(int32_t event, tome_event_info* out) {
    if (out) *out = tome::events::default_info_for_test(event);
}

void TOME_CALL tome_test_event_info(const tome_event_info* info) {
    if (!info || !g_started) return;
    // An older header's smaller struct: its fields over the defaults.
    tome_event_info full = tome::events::default_info_for_test(info->event);
    std::memcpy(&full, info, info->size < sizeof(full) ? info->size : sizeof(full));
    full.size = sizeof(full);
    tome::events::dispatch_for_test(full);
}

int TOME_CALL tome_test_press(const char* mod, const char* id) {
    return mod && id && g_started && tome::keybinds::press_for_test(mod, id) ? 1 : 0;
}

tome_result TOME_CALL tome_test_set_setting(const char* mod, const char* key, const char* json) {
    if (!mod || !key || !json) return TOME_ERR_INVALID_ARGUMENT;
    if (!g_started) return TOME_ERR_UNAVAILABLE;
    tome::json::Value value;
    tome::json::ParseResult parsed = tome::json::parse(json, value);
    if (!parsed.ok) {
        TOME_ERROR("tome_test_set_setting(%s, %s): not JSON: %s", mod, key, parsed.error.c_str());
        return TOME_ERR_INVALID_ARGUMENT;
    }
    std::string error;
    tome_result result = tome::mod_manager().set_setting(mod, key, value, error);
    if (result != TOME_OK) TOME_ERROR("tome_test_set_setting(%s, %s): %s", mod, key, error.c_str());
    return result;
}

const char* TOME_CALL tome_test_setting(const char* mod, const char* key) {
    if (!mod || !key || !g_started) return nullptr;
    tome::json::Value value = tome::mod_manager().setting(mod, key);
    if (value.is_null()) return nullptr;
    std::string text = tome::json::serialize(value);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return keep(std::move(text));
}

// --- Running cases ---

void TOME_CALL tome_test_fail(const char* file, int line, const char* message) {
    ++g_failures;
    std::printf("  FAIL %s:%d: %s\n", file ? file : "?", line, message ? message : "");
    std::fflush(stdout);
}

int TOME_CALL tome_test_check_logged(const char* file, int line, const char* text) {
    if (tome_test_logged(text)) return 1;
    std::string message = std::string("no log line with \"") + (text ? text : "(null)") + "\"; the log:\n";
    std::string log = tome_test_log();
    message += log.empty() ? "  (empty)" : log;
    tome_test_fail(file, line, message.c_str());
    return 0;
}

int TOME_CALL tome_test_check_state(const char* file, int line, const char* id, const char* state) {
    std::string got = tome_test_state(id);
    if (state && got == state) return 1;
    std::string reason = tome_test_reason(id);
    std::string message = std::string(id ? id : "(null)") + " is \"" + got + "\", not \"" + (state ? state : "") + "\"";
    if (!reason.empty()) message += ": " + reason;
    tome_test_fail(file, line, message.c_str());
    return 0;
}

int TOME_CALL tome_test_check_str(const char* file, int line, const char* got, const char* expected, const char* what) {
    if ((!got && !expected) || (got && expected && std::strcmp(got, expected) == 0)) return 1;
    std::string message = std::string(what ? what : "string") + " is " + (got ? "\"" + std::string(got) + "\"" : "NULL") +
                          ", not " + (expected ? "\"" + std::string(expected) + "\"" : "NULL");
    tome_test_fail(file, line, message.c_str());
    return 0;
}

} // extern "C"

namespace {

constexpr DWORD kTimeoutMs = 60000;  // a hang is a failure, not a wait

// A case that crashes outside Tome's guards ends its process with the
// exception code (and without starting a debugger); the parent reports it.
LONG WINAPI report_crash(EXCEPTION_POINTERS* info) {
    std::printf("  unhandled exception 0x%08lx at %p\n", info->ExceptionRecord->ExceptionCode,
                info->ExceptionRecord->ExceptionAddress);
    if (g_home) std::printf("  Tome folder kept: %s\n", tome::narrow(g_home->paths().home_dir).c_str());
    std::fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

int run_one(const tome_test_case& c) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(report_crash);
    c.run();
    tome_test_stop();
    if (g_failures && g_home) {
        // Leftover homes without a tome.log are removed by the next run
        // (Home::remove_leftovers): one that failed before Tome started gets one.
        if (!g_started) tome::write_file_atomic(log_path(), "(this case failed before tome_test_start)\r\n");
        std::printf("  Tome folder kept: %s\n", tome::narrow(g_home->paths().home_dir).c_str());
    }
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}

// 0 passed, 1 failed, 2 crashed or hung.
int spawn(const char* name) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(self) + L"\" --run \"" + tome::widen(name) + L"\"";

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi = {};
    std::fflush(stdout);
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        std::printf("  couldn't start a process for %s (error %lu)\n", name, GetLastError());
        return 2;
    }
    CloseHandle(pi.hThread);
    DWORD exit_code = 2;
    if (WaitForSingleObject(pi.hProcess, kTimeoutMs) == WAIT_TIMEOUT) {
        std::printf("  %s still running after %lu s; stopped\n", name, kTimeoutMs / 1000);
        TerminateProcess(pi.hProcess, 3);
        CloseHandle(pi.hProcess);
        return 2;
    }
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    if (exit_code > 1) std::printf("  %s crashed (exit code 0x%08lx)\n", name, exit_code);
    return exit_code == 0 ? 0 : exit_code == 1 ? 1 : 2;
}

} // namespace

extern "C" int TOME_CALL tome_test_main(int argc, char** argv, const tome_test_case* cases, size_t count) {
    if (argc == 3 && std::strcmp(argv[1], "--run") == 0) {
        for (size_t i = 0; i < count; ++i) {
            if (std::strcmp(cases[i].name, argv[2]) == 0) return run_one(cases[i]);
        }
        std::printf("no test case named %s\n", argv[2]);
        return 2;
    }
    const char* filter = argc == 2 ? argv[1] : nullptr;
    int ran = 0, failed = 0;
    for (size_t i = 0; i < count; ++i) {
        if (filter && !std::strstr(cases[i].name, filter)) continue;
        int result = spawn(cases[i].name);
        std::printf("%s %s\n", result == 0 ? "ok  " : "FAIL", cases[i].name);
        ++ran;
        if (result != 0) ++failed;
    }
    std::printf("\n%d tests, %d failed\n", ran, failed);
    std::fflush(stdout);
    return failed == 0 && ran > 0 ? 0 : 1;
}
