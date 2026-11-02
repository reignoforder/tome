// SPDX-License-Identifier: GPL-3.0-or-later
#include "harness.h"

#include "log.h"
#include "test.h"

#include <windows.h>

#ifndef TOME_TEST_BUILD_DIR
#error "TOME_TEST_BUILD_DIR must be set (tests/CMakeLists.txt)"
#endif

namespace host {
namespace {

std::wstring g_log_path;

} // namespace

std::wstring win_path(const std::string& path) {
    return tome::testing::win_path(path);
}

// --- Home ---

Home::Home() = default;

Home::~Home() {
    // Kept for a look when the test failed (its path is in the log's first lines).
    if (test::failures() == 0) home_.remove();
}

bool Home::install_mod(const char* folder) {
    std::wstring name = tome::widen(folder);
    std::wstring built = win_path(TOME_TEST_BUILD_DIR "/mods/") + name;
    std::wstring fixture = win_path(TOME_TEST_SOURCE_DIR "/tests/e2e/mods/") + name;
    std::wstring from = tome::testing::path_exists(built) ? built : fixture;
    return tome::testing::copy_tree(from, paths().mods_dir + L"\\" + name);
}

bool Home::write(const std::wstring& relative, const std::string& text) {
    return tome::write_file_atomic(paths().home_dir + L"\\" + relative, text);
}

bool Home::read(const std::wstring& relative, std::string& text) const {
    return tome::read_file(paths().home_dir + L"\\" + relative, text);
}

bool Home::remove(const std::wstring& relative) {
    std::wstring path = paths().home_dir + L"\\" + relative;
    if (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_DIRECTORY) {
        tome::testing::remove_tree(path);
        return !tome::testing::path_exists(path);
    }
    return DeleteFileW(path.c_str()) != 0;
}

bool Home::exists(const std::wstring& relative) const {
    return tome::testing::path_exists(paths().home_dir + L"\\" + relative);
}

bool Home::touch(const std::wstring& relative, int seconds) {
    HANDLE file = CreateFileW((paths().home_dir + L"\\" + relative).c_str(), FILE_WRITE_ATTRIBUTES,
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
    g_log_path = tome::testing::log_path(home.home());
    tome::testing::open_log(home.home());
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
    g_log_path = tome::testing::log_path(home.home());
    tome::testing::StartOptions options;
    options.safe_mode = safe_mode;
    options.events = events;
    return tome::testing::start(home.home(), build, options);
}

} // namespace host
