#include "watcher.h"

#include "log.h"

#include <windows.h>

#include <cwctype>
#include <string>

namespace tome {
namespace {

// Editors and compilers write files in bursts; wait for this much quiet first.
constexpr DWORD kSettleMs = 400;
// Backstop for missed notifications. Wine doesn't report changes inside
// subfolders of a recursive watch (seen with a replaced mods/<id>/mod.dll).
// Windows does, so there the watcher only wakes up for notifications.
constexpr DWORD kPollMs = 1500;

bool running_on_wine() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version");
}

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

bool iequals(const std::wstring& a, const wchar_t* b) {
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    }
    return i == a.size() && b[i] == L'\0';
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
        if (iequals(name, w.only_file)) return true;
        if (info->NextEntryOffset == 0) return false;
        p += info->NextEntryOffset;
    }
}

DWORD WINAPI watcher_thread(LPVOID param) {
    auto state = static_cast<WatcherState*>(param);
    HANDLE events[2] = {state->watches[0].overlapped.hEvent, state->watches[1].overlapped.hEvent};
    const DWORD idle_ms = running_on_wine() ? kPollMs : INFINITE;
    bool pending = false;

    for (;;) {
        DWORD wait = WaitForMultipleObjects(2, events, FALSE, pending ? kSettleMs : idle_ms);
        if (wait == WAIT_TIMEOUT) {
            pending = false;
            state->on_change();
            continue;
        }
        if (wait < WAIT_OBJECT_0 || wait >= WAIT_OBJECT_0 + 2) {
            TOME_ERROR("File watcher stopped (wait failed: %lu)", GetLastError());
            return 1;
        }
        Watch& w = state->watches[wait - WAIT_OBJECT_0];
        DWORD bytes = 0;
        if (GetOverlappedResult(w.handle, &w.overlapped, &bytes, FALSE) && relevant(w, bytes)) {
            pending = true;  // restart the settle timer
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
