// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_thread.h"

#include "log.h"
#include "unwind.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <utility>

namespace tome::main_thread {
namespace {

HWND g_window = nullptr;
WNDPROC g_original_proc = nullptr;
bool g_unicode = false;
UINT g_message = 0;
KeyHook g_key_hook = nullptr;
ActivateHook g_activate_hook = nullptr;
DestroyHook g_destroy_hook = nullptr;
std::atomic<uint32_t> g_client_size{0};  // width in the low half, height in the high half

void store_client_size(uint32_t width, uint32_t height) {
    g_client_size = (width & 0xFFFF) | (height << 16);
}

SRWLOCK g_lock = SRWLOCK_INIT;
std::deque<std::function<void()>> g_queue;

void drain() {
    for (;;) {
        std::function<void()> fn;
        {
            ExclusiveLock hold(g_lock);
            if (g_queue.empty()) return;
            fn = std::move(g_queue.front());
            g_queue.pop_front();
        }
        fn();
    }
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == g_message) {
        drain();
        return 0;
    }
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && g_key_hook && g_key_hook(msg, wparam, lparam)) return 0;
    if (msg == WM_ACTIVATEAPP && g_activate_hook) g_activate_hook(wparam != 0);
    if (msg == WM_DESTROY && g_destroy_hook) g_destroy_hook();
    if (msg == WM_SIZE) store_client_size(LOWORD(lparam), HIWORD(lparam));
    return g_unicode ? CallWindowProcW(g_original_proc, hwnd, msg, wparam, lparam)
                     : CallWindowProcA(g_original_proc, hwnd, msg, wparam, lparam);
}

} // namespace

bool install(HWND game_window) {
    g_message = RegisterWindowMessageW(L"Tome.MainThreadCall");
    if (!g_message) return false;

    // Keep the window's character set: subclassing an ANSI window through the
    // W function would make Windows translate its messages.
    g_unicode = IsWindowUnicode(game_window);
    LONG_PTR previous = g_unicode
        ? SetWindowLongPtrW(game_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&window_proc))
        : SetWindowLongPtrA(game_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&window_proc));
    if (!previous) {
        TOME_ERROR("Couldn't subclass the game window (error %lu)", GetLastError());
        return false;
    }
    g_original_proc = reinterpret_cast<WNDPROC>(previous);
    g_window = game_window;
    RECT client{};
    if (GetClientRect(game_window, &client)) {
        store_client_size(static_cast<uint32_t>(client.right - client.left),
                          static_cast<uint32_t>(client.bottom - client.top));
    }

    // Run anything queued before the window existed.
    PostMessageW(g_window, g_message, 0, 0);
    return true;
}

void client_size(int& width, int& height) {
    uint32_t size = g_client_size.load(std::memory_order_relaxed);
    width = static_cast<int>(size & 0xFFFF);
    height = static_cast<int>(size >> 16);
}

bool installed() {
    return g_window != nullptr;
}

HWND window() {
    return g_window;
}

void set_key_hook(KeyHook hook) {
    g_key_hook = hook;
}

void set_activate_hook(ActivateHook hook) {
    g_activate_hook = hook;
}

void set_destroy_hook(DestroyHook hook) {
    g_destroy_hook = hook;
}

void post(std::function<void()> fn) {
    if (!g_window) {
        fn();
        return;
    }
    {
        ExclusiveLock hold(g_lock);
        g_queue.push_back(std::move(fn));
    }
    PostMessageW(g_window, g_message, 0, 0);
}

namespace {

struct Delayed {
    unsigned delay_ms;
    std::function<void()> fn;
};

DWORD WINAPI delayed_thread(LPVOID param) {
    auto delayed = static_cast<Delayed*>(param);
    Sleep(delayed->delay_ms);
    post(std::move(delayed->fn));
    delete delayed;
    return 0;
}

} // namespace

void post_delayed(unsigned delay_ms, std::function<void()> fn) {
    auto delayed = new Delayed{delay_ms, std::move(fn)};
    HANDLE thread = CreateThread(nullptr, 0, delayed_thread, delayed, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        post(std::move(delayed->fn));
        delete delayed;
    }
}

} // namespace tome::main_thread
