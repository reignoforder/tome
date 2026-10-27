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

struct Script {
    HWND window;
    std::vector<Step> steps;
};

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

void click(HWND window, double fx, double fy) {
    RECT rc;
    GetClientRect(window, &rc);
    int x = static_cast<int>(rc.right * fx), y = static_cast<int>(rc.bottom * fy);
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
    DWORD start = GetTickCount();
    for (const Step& step : script->steps) {
        DWORD elapsed = GetTickCount() - start;
        if (step.at_ms > elapsed) Sleep(step.at_ms - elapsed);
        switch (step.kind) {
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
        }
    }
    delete script;
    return 0;
}

} // namespace

void start(HWND game_window) {
    char spec[1024];
    DWORD len = GetEnvironmentVariableA("TOME_DEBUG_CLICKS", spec, sizeof(spec));
    if (len == 0 || len >= sizeof(spec)) return;

    std::vector<std::string> bad;
    auto script = new Script{game_window, debug_script::parse(std::string_view(spec, len), bad)};
    for (const std::string& item : bad) TOME_WARN("TOME_DEBUG_CLICKS: can't parse \"%s\"", item.c_str());
    HANDLE thread = CreateThread(nullptr, 0, run, script, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        delete script;
    }
}

} // namespace tome::debug_input
