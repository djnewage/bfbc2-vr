#include "os_input.h"

namespace osinput {
namespace {

HWND g_hwnd = nullptr;

BOOL CALLBACK find_game_window(HWND h, LPARAM)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT r = {};
    GetWindowRect(h, &r);
    if (r.right - r.left < 200 || r.bottom - r.top < 200) return TRUE;
    g_hwnd = h;
    return FALSE;
}

} // namespace

HWND game_window()
{
    if (!g_hwnd || !IsWindow(g_hwnd)) { g_hwnd = nullptr; EnumWindows(find_game_window, 0); }
    return g_hwnd;
}

bool game_is_foreground()
{
    HWND h = game_window();
    return h && GetForegroundWindow() == h;
}

bool focus_game()
{
    HWND h = game_window();
    if (!h) return false;
    if (GetForegroundWindow() == h) return true;
    const DWORD fg_thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD me = GetCurrentThreadId();
    if (fg_thread && fg_thread != me) AttachThreadInput(me, fg_thread, TRUE);
    AllowSetForegroundWindow(ASFW_ANY);
    BringWindowToTop(h);
    SetForegroundWindow(h);
    SetActiveWindow(h);
    if (fg_thread && fg_thread != me) AttachThreadInput(me, fg_thread, FALSE);
    return GetForegroundWindow() == h;
}

void send_key(WORD vk, bool down)
{
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    in.ki.dwExtraInfo = kInjectedTag;
    SendInput(1, &in, sizeof(in));
}

void send_mouse_button(bool right, bool down)
{
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = right ? (down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP)
                          : (down ? MOUSEEVENTF_LEFTDOWN  : MOUSEEVENTF_LEFTUP);
    in.mi.dwExtraInfo = kInjectedTag;
    SendInput(1, &in, sizeof(in));
}

void send_mouse_move(int dx, int dy)
{
    if (dx == 0 && dy == 0) return;
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;      // relative; no MOUSEEVENTF_ABSOLUTE
    in.mi.dwExtraInfo = kInjectedTag;
    SendInput(1, &in, sizeof(in));
}

} // namespace osinput
