// SPDX-License-Identifier: GPL-3.0-or-later
#include "watcher.h"

#include "core/text.h"
#include "game_build.h"
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <string>

namespace tome {
namespace {

// Editors and compilers write files in bursts; wait for this much quiet first.
constexpr DWORD kSettleMs = 400;
// Backstop for missed notifications. Wine doesn't report changes inside
// subfolders of a recursive watch (seen with a replaced mods/<id>/mod.dll).
// Windows does, so there the watcher only wakes up for notifications.
constexpr DWORD kPollMs = 1500;
// A poll lists every file under mods/; with large asset folders that takes a
// while, so the next one waits at least this many times as long.
constexpr DWORD kPollCostFactor = 50;

struct Watch {
    std::wstring dir;
    bool recursive;
    const wchar_t* only_file;  // if set, ignore changes to other files
    HANDLE handle = INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped = {};
    alignas(DWORD) unsigned char buffer[16 * 1024];
};

struct WatcherState {
    Watch watches[2];
    std::function<void()> on_change;
};

bool arm(Watch& w) {
    ResetEvent(w.overlapped.hEvent);
    return ReadDirectoryChangesW(w.handle, w.buffer, sizeof(w.buffer), w.recursive,
                                 FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                     FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
                                 nullptr, &w.overlapped, nullptr);
}

// True if the completed notification batch contains a change we care about.
bool relevant(Watch& w, DWORD bytes) {
    if (bytes == 0) {
        return true;  // buffer overflowed; changes were lost, so assume relevant
    }
    if (!w.only_file) {
        return true;
    }
    unsigned char* p = w.buffer;
    for (;;) {
        auto info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(p);
        std::wstring name(info->FileName, info->FileNameLength / sizeof(wchar_t));
        if (text::iequals(name, w.only_file)) return true;
        if (info->NextEntryOffset == 0) return false;
        p += info->NextEntryOffset;
    }
}

DWORD WINAPI watcher_thread(LPVOID param) {
    auto state = static_cast<WatcherState*>(param);
    HANDLE events[2] = {state->watches[0].overlapped.hEvent, state->watches[1].overlapped.hEvent};
    const bool poll = running_on_wine();
    // Absolute deadlines: notifications that don't count (tome.log is written
    // next to state.json all the time) wake the thread too, and mustn't push
    // back a change that's settling or the next poll.
    ULONGLONG settle_at = 0;  // when a pending change has settled; 0: none pending
    ULONGLONG poll_at = poll ? GetTickCount64() + kPollMs : 0;

    for (;;) {
        const ULONGLONG due = settle_at ? settle_at : poll_at;
        DWORD timeout = INFINITE;
        if (due) {
            const ULONGLONG now = GetTickCount64();
            timeout = due > now ? static_cast<DWORD>(due - now) : 0;
        }
        DWORD wait = WaitForMultipleObjects(2, events, FALSE, timeout);
        if (wait == WAIT_TIMEOUT) {
            settle_at = 0;
            const ULONGLONG started = GetTickCount64();
            state->on_change();
            if (poll) {
                const ULONGLONG now = GetTickCount64();
                poll_at = now + std::max<ULONGLONG>(kPollMs, (now - started) * kPollCostFactor);
            }
            continue;
        }
        if (wait < WAIT_OBJECT_0 || wait >= WAIT_OBJECT_0 + 2) {
            TOME_ERROR("File watcher stopped (wait failed: %lu)", GetLastError());
            return 1;
        }
        Watch& w = state->watches[wait - WAIT_OBJECT_0];
        DWORD bytes = 0;
        if (GetOverlappedResult(w.handle, &w.overlapped, &bytes, FALSE) && relevant(w, bytes)) {
            settle_at = GetTickCount64() + kSettleMs;  // restart the settle timer
        }
        if (!arm(w)) {
            TOME_ERROR("File watcher stopped watching %s", narrow(w.dir).c_str());
            return 1;
        }
    }
}

} // namespace

bool start_watcher(const Paths& paths, std::function<void()> on_change) {
    // Lives for the rest of the process.
    auto state = new WatcherState();
    state->on_change = std::move(on_change);
    state->watches[0].dir = paths.tome_dir;
    state->watches[0].recursive = false;
    state->watches[0].only_file = L"state.json";
    state->watches[1].dir = paths.mods_dir;
    state->watches[1].recursive = true;
    state->watches[1].only_file = nullptr;

    for (Watch& w : state->watches) {
        w.handle = CreateFileW(w.dir.c_str(), FILE_LIST_DIRECTORY,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        w.overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (w.handle == INVALID_HANDLE_VALUE || !w.overlapped.hEvent || !arm(w)) {
            TOME_ERROR("Can't watch %s for changes; live mod toggling is off", narrow(w.dir).c_str());
            return false;
        }
    }

    HANDLE thread = CreateThread(nullptr, 0, watcher_thread, state, 0, nullptr);
    if (!thread) {
        TOME_ERROR("Can't start the file watcher thread; live mod toggling is off");
        return false;
    }
    CloseHandle(thread);
    return true;
}

} // namespace tome
