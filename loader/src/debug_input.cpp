// SPDX-License-Identifier: GPL-3.0-or-later
#include "debug_input.h"

#include "core/debug_script.h"
#include "core/keys.h"
#include "events.h"
#include "frames.h"
#include "hooks.h"
#include "main_thread.h"
#include "log.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tome::debug_input {
namespace {

using debug_script::Kind;
using debug_script::Step;

// A watchdog, not a pace: how long a step may wait for the game before the
// script gives up (a map's loading screen under software rendering takes
// well over a minute).
constexpr DWORD kWaitMs = 300000;
// An until step's tries before it gives up.
constexpr unsigned kUntilTries = 20;

struct Script {
    HWND window;
    std::vector<Step> steps;
};

// Parsed by prepare(), played by start().
std::vector<Step> g_steps;
bool g_waits = false;  // the script has wait steps: the log keeps its lines

// Lets the game handle what was posted: n frames, so input is paced by the
// game and not a clock. If it presents none (nothing has hooked PresentFrame
// yet), at most 2 s.
void pause_frames(uint32_t n) {
    const uint32_t from = events::frames();
    const DWORD began = GetTickCount();
    while (events::frames() - from < n && GetTickCount() - began < 2000) Sleep(2);
}

void press(HWND window, WPARAM key) {
    PostMessageW(window, WM_KEYDOWN, key, 1);
    pause_frames(1);
    PostMessageW(window, WM_KEYUP, key, 1 | (1u << 30) | (1u << 31));
    pause_frames(1);
}

// Real key presses (keybd_event), so the game and Tome see the modifiers in
// GetKeyState. The game window has to have the focus.
void press_key(const std::string& name) {
    tome::keys::Key key;
    if (!tome::keys::parse(name, key) || !key.bound()) {
        TOME_WARN("TOME_DEBUG_CLICKS: unknown key \"%s\"", name.c_str());
        return;
    }
    const BYTE modifiers[] = {VK_CONTROL, VK_SHIFT, VK_MENU};
    const uint8_t flags[] = {tome::keys::kCtrl, tome::keys::kShift, tome::keys::kAlt};
    for (int i = 0; i < 3; ++i) {
        if (key.mods & flags[i]) keybd_event(modifiers[i], 0, 0, 0);
    }
    pause_frames(1);
    keybd_event(key.vk, 0, 0, 0);
    pause_frames(1);
    keybd_event(key.vk, 0, KEYEVENTF_KEYUP, 0);
    for (int i = 2; i >= 0; --i) {
        if (key.mods & flags[i]) keybd_event(modifiers[i], 0, KEYEVENTF_KEYUP, 0);
    }
    pause_frames(1);
}

// Enter opens the chat box, the characters go in as WM_CHAR, Enter sends.
void type_line(HWND window, const std::string& text) {
    press(window, VK_RETURN);
    pause_frames(3);  // the chat box opens
    for (unsigned char c : text) PostMessageW(window, WM_CHAR, c, 1);
    pause_frames(2);
    press(window, VK_RETURN);
}

// The cursor at a fraction of the client area, clamped to its last pixel
// (1 is the right or bottom edge, for edge scrolling).
POINT client_point(HWND window, double fx, double fy) {
    RECT rc;
    GetClientRect(window, &rc);
    int x = static_cast<int>(rc.right * fx), y = static_cast<int>(rc.bottom * fy);
    if (x >= rc.right) x = rc.right - 1;
    if (y >= rc.bottom) y = rc.bottom - 1;
    return {x < 0 ? 0 : x, y < 0 ? 0 : y};
}

void move(HWND window, double fx, double fy) {
    POINT client = client_point(window, fx, fy);
    POINT screen = client;
    ClientToScreen(window, &screen);
    SetCursorPos(screen.x, screen.y);
    PostMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(client.x, client.y));
    TOME_INFO("Debug move to %.3f,%.3f (client %ld,%ld)", fx, fy, client.x, client.y);
}

void click(HWND window, double fx, double fy) {
    POINT client = client_point(window, fx, fy);
    int x = client.x, y = client.y;
    LPARAM pos = MAKELPARAM(x, y);
    POINT screen = {x, y};
    ClientToScreen(window, &screen);
    SetCursorPos(screen.x, screen.y);
    PostMessageW(window, WM_MOUSEMOVE, 0, pos);
    pause_frames(2);  // the button under the cursor sees it
    PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, pos);
    pause_frames(1);
    PostMessageW(window, WM_LBUTTONUP, 0, pos);
    pause_frames(1);
    TOME_INFO("Debug click at %.3f,%.3f (client %d,%d)", fx, fy, x, y);
}

// Runs fn on the game's main thread and waits for it (at most 10 s: a game
// that stopped presenting frames).
void on_main(std::function<void()> fn) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    main_thread::post([fn = std::move(fn), done] {
        fn();
        done->store(true);
    });
    const DWORD began = GetTickCount();
    while (!done->load() && GetTickCount() - began < 10000) Sleep(2);
}

// Clicks (or focuses) the named frame on the main thread. The name is copied:
// on_main may give up waiting, and the call may then run after this step
// (or the whole script) is gone.
void frame_action(const std::string& name, int context, bool focus) {
    on_main([name, context, focus] {
        const char* what = focus ? "focus" : "click";
        void* frame = frames::get(name.c_str(), context);
        if (!frame) {
            TOME_WARN("Debug %s on %s (context %d): no such frame", what, name.c_str(), context);
            return;
        }
        if (!focus) {
            TOME_INFO("Debug click on %s (context %d)", name.c_str(), context);
            if (!frames::click(frame)) TOME_WARN("Debug click on %s: not a control", name.c_str());
        } else if (frames::set_focus(frame)) {
            TOME_INFO("Debug focus on %s (context %d)", name.c_str(), context);
        } else {
            TOME_WARN("Debug focus on %s: it didn't take the focus", name.c_str());
        }
    });
    pause_frames(1);
}

void act(HWND window, const Step& step) {
    switch (step.kind) {
    case Kind::ClickFrame: frame_action(step.text, step.context, false); break;
    case Kind::Focus: frame_action(step.text, step.context, true); break;
    case Kind::Close:
        TOME_INFO("Debug close");
        PostMessageW(window, WM_CLOSE, 0, 0);
        break;
    case Kind::Minimize:
    case Kind::Restore:
        TOME_INFO("Debug %s", step.kind == Kind::Minimize ? "minimize" : "restore");
        ShowWindowAsync(window, step.kind == Kind::Minimize ? SW_MINIMIZE : SW_RESTORE);
        if (step.kind == Kind::Restore) SetForegroundWindow(window);
        break;
    case Kind::Resize: {
        TOME_INFO("Debug resize to %ux%u", step.width, step.height);
        RECT r = {0, 0, static_cast<LONG>(step.width), static_cast<LONG>(step.height)};
        AdjustWindowRectEx(&r, static_cast<DWORD>(GetWindowLongW(window, GWL_STYLE)), FALSE,
                           static_cast<DWORD>(GetWindowLongW(window, GWL_EXSTYLE)));
        SetWindowPos(window, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        break;
    }
    case Kind::Text:
        TOME_INFO("Debug text \"%s\"", step.text.c_str());
        for (unsigned char c : step.text) PostMessageW(window, WM_CHAR, c, 1);
        pause_frames(1);
        break;
    case Kind::Key:
        TOME_INFO("Debug key %s", step.text.c_str());
        press_key(step.text);
        break;
    case Kind::Type:
        TOME_INFO("Debug typing \"%s\"", step.text.c_str());
        type_line(window, step.text);
        break;
    case Kind::Click: click(window, step.x, step.y); break;
    case Kind::Move: move(window, step.x, step.y); break;
    case Kind::Wait:
    case Kind::Frames:
    case Kind::Until:
    case Kind::Settle: break;
    }
}

DWORD WINAPI run(LPVOID param) {
    auto script = static_cast<Script*>(param);
    DWORD start = GetTickCount();  // of the script, then of the last wait's end
    size_t log_pos = 0;            // in the kept log: after the last wait's line
    const Step* last_action = nullptr;
    size_t action_pos = 0;  // the kept log's end just before the last action
    uint32_t action_clicks = 0;  // events::control_clicks() then
    // Lines that repeat a step's text start with "Debug " (log doesn't keep
    // them, the e2e checks skip them); the others name steps by number, so
    // a wait or until never finds its own text in them.
    size_t number = 0;
    auto stop = [&](const char* why) {
        TOME_WARN("TOME_DEBUG_CLICKS: step %zu: %s; the script stops", number, why);
        log::keep_lines(false);
        delete script;
        return DWORD(0);
    };
    for (const Step& step : script->steps) {
        ++number;
        DWORD elapsed = GetTickCount() - start;
        if (step.at_ms > elapsed) Sleep(step.at_ms - elapsed);
        switch (step.kind) {
        case Kind::Wait: {
            // The text isn't logged here: a later wait for it would match this line.
            DWORD began = GetTickCount();
            while (!log::find_kept(log_pos, step.text)) {
                if (GetTickCount() - began > kWaitMs) return stop("its log line didn't come in time");
                Sleep(20);
            }
            start = GetTickCount();
            break;
        }
        case Kind::Frames: {
            const uint32_t from = events::frames();
            DWORD began = GetTickCount();
            while (events::frames() - from < step.frames) {
                if (GetTickCount() - began > kWaitMs) return stop("the game stopped presenting frames");
                Sleep(5);
            }
            start = GetTickCount();
            break;
        }
        case Kind::Settle: {
            // A few frames first, so an action just posted has started its animation.
            pause_frames(2);
            DWORD began = GetTickCount();
            while (events::frames_since_ui_tick() < debug_script::kSettleFrames) {
                if (GetTickCount() - began > kWaitMs) return stop("the UI never settled");
                Sleep(5);
            }
            start = GetTickCount();
            break;
        }
        case Kind::Until: {
            if (!last_action) return stop("no action before it to repeat");
            size_t from = action_pos;
            uint32_t frame = events::frames();
            unsigned tries = 1;
            bool taken_logged = false;
            DWORD began = GetTickCount();
            while (!log::find_kept(from, step.text)) {
                if (GetTickCount() - began > kWaitMs) return stop("its log line didn't come in time");
                if (events::frames() - frame >= debug_script::kUntilFrames) {
                    // A control took it: the effect is on its way (or never
                    // comes, which the watchdog reports); doing it again could
                    // do it twice. Only an action nothing took is repeated.
                    if (events::control_clicks() != action_clicks) {
                        if (!taken_logged) {
                            TOME_INFO("Debug: step %zu: a control took the action; waiting for its effect", number);
                            taken_logged = true;
                        }
                        frame = events::frames();
                        Sleep(5);
                        continue;
                    }
                    if (++tries > kUntilTries) return stop("the action had no effect after repeated tries");
                    TOME_INFO("Debug: step %zu saw no effect in %u frames; the action again (try %u)", number,
                              debug_script::kUntilFrames, tries);
                    action_clicks = events::control_clicks();
                    act(script->window, *last_action);
                    frame = events::frames();
                }
                Sleep(5);
            }
            log_pos = from;
            start = GetTickCount();
            break;
        }
        default:
            action_pos = log::kept_size();
            action_clicks = events::control_clicks();
            last_action = &step;
            act(script->window, step);
            break;
        }
    }
    TOME_INFO("Debug script finished");
    if (g_waits) log::keep_lines(false);
    delete script;
    return 0;
}

std::string script_spec() {
    if (!TOME_DEV) return {};
    std::string spec(8192, '\0');
    DWORD len = GetEnvironmentVariableA("TOME_DEBUG_CLICKS", spec.data(), static_cast<DWORD>(spec.size()));
    if (len >= spec.size()) {
        TOME_WARN("TOME_DEBUG_CLICKS: longer than %zu characters; ignored", spec.size() - 1);
        len = 0;
    }
    spec.resize(len);
    return spec;
}

// TOME_DEBUG_SKIP_MOVIES=1: a movie (Movie::Play) takes over the main
// thread, so no frame is presented and the script's frame-paced steps can't
// press Esc to skip it. Movie::Play calls Movie::SetDisplayMode before each
// movie, and its caller calls it again after (the movie's window is gone by
// then). After each call a thread waits for a movie's window, another
// visible top-level window of this process, and skips the movie with Esc
// there (a posted key, then a real one if the window ignored it, only while
// that window or one of its thread's has the focus); after a movie it finds
// none and gives up. Not counting the calls:
// a hook installed during a movie would have counted them the wrong way round.
HWND g_game_window = nullptr;
constexpr DWORD kMovieWindowWaitMs = 10000;  // a watchdog

BOOL CALLBACK find_movie_window(HWND w, LPARAM out) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId() || w == g_game_window || !IsWindowVisible(w)) return TRUE;
    *reinterpret_cast<HWND*>(out) = w;
    return FALSE;
}

// True if the window with the focus is `movie`, or another visible window
// of the thread that owns it (DirectShow's video windows share one), so a
// real Esc goes to the movie and not to some other window of this process.
bool movie_has_focus(HWND movie) {
    HWND focused = GetForegroundWindow();
    if (!focused || focused == g_game_window || !IsWindowVisible(focused)) return false;
    if (focused == movie) return true;
    DWORD focused_pid = 0, movie_pid = 0;
    const DWORD focused_thread = GetWindowThreadProcessId(focused, &focused_pid);
    const DWORD movie_thread = GetWindowThreadProcessId(movie, &movie_pid);
    return focused_pid == GetCurrentProcessId() && movie_pid == focused_pid && focused_thread == movie_thread;
}

DWORD WINAPI skip_movie_thread(LPVOID) {
    DWORD posted_at = 0;
    bool posted = false;
    for (DWORD waited = 0; waited < kMovieWindowWaitMs; waited += 50) {
        HWND movie = nullptr;
        EnumWindows(find_movie_window, reinterpret_cast<LPARAM>(&movie));
        if (!movie) {
            if (posted) return 0;  // the movie is over
        } else if (!posted) {
            TOME_INFO("Debug: skipping the movie (Esc)");
            PostMessageW(movie, WM_KEYDOWN, VK_ESCAPE, 0);
            posted = true;
            posted_at = waited;
        } else if (waited - posted_at >= 1000) {
            // 1.26a's movie window is DirectShow's, which ignores a posted
            // key: a real Esc once a movie window has the focus, as a player
            // would press it.
            if (movie_has_focus(movie)) {
                TOME_INFO("Debug: the movie is still on; pressing Esc");
                keybd_event(VK_ESCAPE, 0, 0, 0);
                keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
                return 0;
            }
        }
        Sleep(50);
    }
    return 0;
}

void TOME_CALL after_movie_display_mode(tome_mod*, tome_hook_call*, void*) {
    HANDLE thread = CreateThread(nullptr, 0, skip_movie_thread, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
}

void start_movie_skipping(HWND game_window) {
    char env[8] = {};
    if (!TOME_DEV || !GetEnvironmentVariableA("TOME_DEBUG_SKIP_MOVIES", env, sizeof(env)) || env[0] != '1') return;
    g_game_window = game_window;
    std::string error;
    hooks::Target* target = hooks::find_or_install("Movie::SetDisplayMode", error);
    if (!target) {
        TOME_WARN("TOME_DEBUG_SKIP_MOVIES: %s", error.c_str());
        return;
    }
    hooks::Handler after;
    after.phase = TOME_HOOK_AFTER;
    after.fn = after_movie_display_mode;
    hooks::add(target, after);
}

} // namespace

void prepare() {
    std::string spec = script_spec();
    if (spec.empty()) return;
    std::vector<std::string> bad;
    g_steps = debug_script::parse(spec, bad);
    for (const std::string& item : bad) TOME_WARN("TOME_DEBUG_CLICKS: can't parse \"%s\"", item.c_str());
    for (const Step& step : g_steps) g_waits = g_waits || step.kind == Kind::Wait || step.kind == Kind::Until;
    if (g_waits) log::keep_lines(true);
}

void start(HWND game_window) {
    start_movie_skipping(game_window);
    if (g_steps.empty()) return;
    auto script = new Script{game_window, std::move(g_steps)};
    g_steps.clear();
    HANDLE thread = CreateThread(nullptr, 0, run, script, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        delete script;
    }
}

} // namespace tome::debug_input
