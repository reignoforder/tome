#include "debug_input.h"

#include "core/keys.h"
#include "log.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace tome::debug_input {
namespace {

struct Step {
    double x = 0, y = 0;
    std::string text;  // set: a chat line to type instead of a click
    bool typing = false;
    bool close = false;  // close the window, like Alt-F4
    int show = -1;       // ShowWindow command: minimize or restore, like Alt-Tab away and back
    bool key = false;    // press `text` (a key name like "Ctrl+J") through the system's input
    bool chars = false;  // type `text` into whatever has the keyboard, without Enter
    DWORD at_ms = 0;
};

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
        if (step.close) {
            TOME_INFO("Debug close");
            PostMessageW(script->window, WM_CLOSE, 0, 0);
        } else if (step.show >= 0) {
            TOME_INFO("Debug %s", step.show == SW_MINIMIZE ? "minimize" : "restore");
            ShowWindowAsync(script->window, step.show);
            if (step.show == SW_RESTORE) SetForegroundWindow(script->window);
        } else if (step.chars) {
            TOME_INFO("Debug text \"%s\"", step.text.c_str());
            for (unsigned char c : step.text) {
                PostMessageW(script->window, WM_CHAR, c, 1);
                Sleep(20);
            }
        } else if (step.key) {
            TOME_INFO("Debug key %s", step.text.c_str());
            press_key(step.text);
        } else if (step.typing) {
            TOME_INFO("Debug typing \"%s\"", step.text.c_str());
            type_line(script->window, step.text);
        } else {
            click(script->window, step.x, step.y);
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

    auto script = new Script{game_window, {}};
    std::string text(spec, len);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(';', pos);
        std::string item = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        Step step;
        unsigned long ms;
        size_t at = item.rfind('@');
        if (std::sscanf(item.c_str(), "close@%lu", &ms) == 1) {
            step.close = true;
            step.at_ms = ms;
            script->steps.push_back(step);
        } else if (std::sscanf(item.c_str(), "minimize@%lu", &ms) == 1 ||
                   std::sscanf(item.c_str(), "restore@%lu", &ms) == 1) {
            step.show = item[0] == 'm' ? SW_MINIMIZE : SW_RESTORE;
            step.at_ms = ms;
            script->steps.push_back(step);
        } else if (item.rfind("text:", 0) == 0 && at != std::string::npos &&
                   std::sscanf(item.c_str() + at, "@%lu", &ms) == 1) {
            step.chars = true;
            step.text = item.substr(5, at - 5);
            step.at_ms = ms;
            script->steps.push_back(step);
        } else if (item.rfind("key:", 0) == 0 && at != std::string::npos &&
                   std::sscanf(item.c_str() + at, "@%lu", &ms) == 1) {
            step.key = true;
            step.text = item.substr(4, at - 4);
            step.at_ms = ms;
            script->steps.push_back(step);
        } else if (item.rfind("type:", 0) == 0 && at != std::string::npos &&
            std::sscanf(item.c_str() + at, "@%lu", &ms) == 1) {
            step.typing = true;
            step.text = item.substr(5, at - 5);
            step.at_ms = ms;
            script->steps.push_back(step);
        } else if (std::sscanf(item.c_str(), "%lf,%lf@%lu", &step.x, &step.y, &ms) == 3) {
            step.at_ms = ms;
            script->steps.push_back(step);
        } else if (!item.empty()) {
            TOME_WARN("TOME_DEBUG_CLICKS: can't parse \"%s\"", item.c_str());
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    HANDLE thread = CreateThread(nullptr, 0, run, script, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        delete script;
    }
}

} // namespace tome::debug_input
