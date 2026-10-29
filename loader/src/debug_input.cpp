#include "debug_input.h"

#include "core/debug_script.h"
#include "core/keys.h"
#include "log.h"

#include <string>
#include <vector>

namespace tome::debug_input {
namespace {

using debug_script::Kind;
using debug_script::Step;

// How long a wait step waits for its line: a map's loading screen under
// software rendering takes well over a minute.
constexpr DWORD kWaitMs = 300000;

struct Script {
    HWND window;
    std::vector<Step> steps;
};

// Parsed by prepare(), played by start().
std::vector<Step> g_steps;
bool g_waits = false;  // the script has wait steps: the log keeps its lines

void press(HWND window, WPARAM key) {
    PostMessageW(window, WM_KEYDOWN, key, 1);
    Sleep(30);
    PostMessageW(window, WM_KEYUP, key, 1 | (1u << 30) | (1u << 31));
    Sleep(30);
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
    Sleep(30);
    keybd_event(key.vk, 0, 0, 0);
    Sleep(30);
    keybd_event(key.vk, 0, KEYEVENTF_KEYUP, 0);
    for (int i = 2; i >= 0; --i) {
        if (key.mods & flags[i]) keybd_event(modifiers[i], 0, KEYEVENTF_KEYUP, 0);
    }
}

// Enter opens the chat box, the characters go in as WM_CHAR, Enter sends.
void type_line(HWND window, const std::string& text) {
    press(window, VK_RETURN);
    Sleep(300);
    for (unsigned char c : text) {
        PostMessageW(window, WM_CHAR, c, 1);
        Sleep(20);
    }
    Sleep(200);
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
    Sleep(150);
    PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, pos);
    Sleep(80);
    PostMessageW(window, WM_LBUTTONUP, 0, pos);
    TOME_INFO("Debug click at %.3f,%.3f (client %d,%d)", fx, fy, x, y);
}

DWORD WINAPI run(LPVOID param) {
    auto script = static_cast<Script*>(param);
    DWORD start = GetTickCount();  // of the script, then of the last wait's end
    size_t log_pos = 0;            // in the kept log: after the last wait's line
    for (const Step& step : script->steps) {
        DWORD elapsed = GetTickCount() - start;
        if (step.at_ms > elapsed) Sleep(step.at_ms - elapsed);
        switch (step.kind) {
        case Kind::Wait: {
            // The text isn't logged here: a later wait for it would match this line.
            DWORD began = GetTickCount();
            while (!log::find_kept(log_pos, step.text)) {
                if (GetTickCount() - began > kWaitMs) {
                    TOME_WARN("TOME_DEBUG_CLICKS: no log line with \"%s\" in %lu s; the script stops",
                              step.text.c_str(), kWaitMs / 1000);
                    log::keep_lines(false);
                    delete script;
                    return 0;
                }
                Sleep(50);
            }
            start = GetTickCount();
            break;
        }
        case Kind::Close:
            TOME_INFO("Debug close");
            PostMessageW(script->window, WM_CLOSE, 0, 0);
            break;
        case Kind::Minimize:
        case Kind::Restore:
            TOME_INFO("Debug %s", step.kind == Kind::Minimize ? "minimize" : "restore");
            ShowWindowAsync(script->window, step.kind == Kind::Minimize ? SW_MINIMIZE : SW_RESTORE);
            if (step.kind == Kind::Restore) SetForegroundWindow(script->window);
            break;
        case Kind::Text:
            TOME_INFO("Debug text \"%s\"", step.text.c_str());
            for (unsigned char c : step.text) {
                PostMessageW(script->window, WM_CHAR, c, 1);
                Sleep(20);
            }
            break;
        case Kind::Key:
            TOME_INFO("Debug key %s", step.text.c_str());
            press_key(step.text);
            break;
        case Kind::Type:
            TOME_INFO("Debug typing \"%s\"", step.text.c_str());
            type_line(script->window, step.text);
            break;
        case Kind::Click:
            click(script->window, step.x, step.y);
            break;
        case Kind::Move:
            move(script->window, step.x, step.y);
            break;
        }
    }
    if (g_waits) log::keep_lines(false);
    delete script;
    return 0;
}

std::string script_spec() {
    std::string spec(8192, '\0');
    DWORD len = GetEnvironmentVariableA("TOME_DEBUG_CLICKS", spec.data(), static_cast<DWORD>(spec.size()));
    if (len >= spec.size()) {
        TOME_WARN("TOME_DEBUG_CLICKS: longer than %zu characters; ignored", spec.size() - 1);
        len = 0;
    }
    spec.resize(len);
    return spec;
}

} // namespace

void prepare() {
    std::string spec = script_spec();
    if (spec.empty()) return;
    std::vector<std::string> bad;
    g_steps = debug_script::parse(spec, bad);
    for (const std::string& item : bad) TOME_WARN("TOME_DEBUG_CLICKS: can't parse \"%s\"", item.c_str());
    for (const Step& step : g_steps) g_waits = g_waits || step.kind == Kind::Wait;
    if (g_waits) log::keep_lines(true);
}

void start(HWND game_window) {
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
