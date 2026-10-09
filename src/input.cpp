#include "input.h"

#include <windows.h>

#include <windowsx.h>

#include <cstring>

#include "config.h"
#include "log.h"
#include "ui.h"

namespace input {
namespace {

enum Owner : uint8_t { kNobody, kGame, kMod };

HWND g_hwnd = nullptr;
WNDPROC g_originalWndProc = nullptr;
double g_nextWindowSearch = 0;
std::vector<MouseEvent> g_events;
bool g_buttonMine[5] = {};  // L, R, M, X1, X2: the mod swallowed the down, so it swallows the up
uint8_t g_keyOwner[256] = {};

int ButtonIndex(UINT msg, WPARAM wp) {
  switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONUP: return 0;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
    case WM_RBUTTONUP: return 1;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
    case WM_MBUTTONUP: return 2;
    default: return GET_XBUTTON_WPARAM(wp) == XBUTTON2 ? 4 : 3;
  }
}

bool IsXButton(UINT msg) { return msg == WM_XBUTTONDOWN || msg == WM_XBUTTONDBLCLK || msg == WM_XBUTTONUP; }

void Push(MouseEvent::Type type, LPARAM lp, int wheel = 0) {
  g_events.push_back({type, static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp)), wheel,
                      static_cast<uint32_t>(GetMessageTime())});
}

bool IsMouseRawInput(LPARAM lp) {
  RAWINPUTHEADER header{};
  UINT size = sizeof header;
  if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_HEADER, &header, &size, sizeof(RAWINPUTHEADER)) ==
      static_cast<UINT>(-1)) {
    return false;
  }
  return header.dwType == RIM_TYPEMOUSE;
}

// Focus loss: matching "up" messages may never arrive, so forget what we were holding.
void ForgetHeldInput() {
  memset(g_buttonMine, 0, sizeof g_buttonMine);
  memset(g_keyOwner, 0, sizeof g_keyOwner);
  g_events.push_back({MouseEvent::Cancel, 0.f, 0.f, 0, 0});
}

LRESULT CALLBACK GameWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
      const int vk = static_cast<int>(wp & 0xFF);
      if (msg == WM_SYSKEYDOWN && vk == VK_F4) break;  // Alt+F4 always reaches the game
      const bool repeat = (lp & (1 << 30)) != 0;
      // With Alt held only hotkeys are the mod's: Alt+Enter (fullscreen), Alt+Space etc. stay the game's.
      const bool alt = (HIWORD(lp) & KF_ALTDOWN) != 0;
      if (g_keyOwner[vk] == kMod || (g_keyOwner[vk] != kGame && ui::WantsKey(vk, alt))) {
        g_keyOwner[vk] = kMod;
        ui::OnKeyDown(vk, repeat);
        return 0;
      }
      g_keyOwner[vk] = kGame;
      break;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
      const int vk = static_cast<int>(wp & 0xFF);
      const bool mine = g_keyOwner[vk] == kMod;
      g_keyOwner[vk] = kNobody;
      if (mine) return 0;
      break;
    }
    case WM_CHAR:
      // Characters typed by the menu's own keys (Enter, Space, Backspace, Esc).
      if (ui::CapturesInput() && (wp == '\r' || wp == ' ' || wp == '\b' || wp == 27)) return 0;
      break;
    case WM_INPUT:
      // Relative mouse motion drives camera look; DefWindowProc does the required raw-input cleanup.
      if (ui::CapturesInput() && IsMouseRawInput(lp)) return DefWindowProcW(hwnd, msg, wp, lp);
      break;
    case WM_MOUSEMOVE:
      if (ui::CapturesInput()) return 0;
      break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONDBLCLK: {
      const int button = ButtonIndex(msg, wp);
      if (!ui::CapturesInput()) {
        g_buttonMine[button] = false;  // the game owns this press, even if an older release got lost
        break;
      }
      g_buttonMine[button] = true;
      if (button == 0) Push(MouseEvent::LeftDown, lp);
      if (button == 1) Push(MouseEvent::RightDown, lp);
      return IsXButton(msg) ? TRUE : 0;
    }
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_XBUTTONUP: {
      const int button = ButtonIndex(msg, wp);
      if (!g_buttonMine[button]) break;
      g_buttonMine[button] = false;
      if (button == 0) Push(MouseEvent::LeftUp, lp);
      if (button == 1) Push(MouseEvent::RightUp, lp);
      return IsXButton(msg) ? TRUE : 0;
    }
    case WM_MOUSEWHEEL: {
      if (!ui::CapturesInput()) break;
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};  // screen coordinates for wheel messages
      ScreenToClient(hwnd, &pt);
      g_events.push_back({MouseEvent::Wheel, static_cast<float>(pt.x), static_cast<float>(pt.y),
                          GET_WHEEL_DELTA_WPARAM(wp), static_cast<uint32_t>(GetMessageTime())});
      return 0;
    }
    case WM_MOUSEHWHEEL:
      if (ui::CapturesInput()) return 0;
      break;
    case WM_ACTIVATEAPP:
      if (!wp) ForgetHeldInput();
      break;
    case WM_KILLFOCUS:
      ForgetHeldInput();
      break;
  }
  return g_originalWndProc ? CallWindowProcW(g_originalWndProc, hwnd, msg, wp, lp)
                           : DefWindowProcW(hwnd, msg, wp, lp);
}

BOOL CALLBACK FindGameWindowProc(HWND hwnd, LPARAM param) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
  wchar_t cls[64] = {};
  GetClassNameW(hwnd, cls, 64);
  if (wcscmp(cls, L"UnrealWindow") != 0) return TRUE;
  *reinterpret_cast<HWND*>(param) = hwnd;
  return FALSE;
}

}  // namespace

void Maintain(double now) {
  if (g_hwnd && IsWindow(g_hwnd)) return;
  if (now < g_nextWindowSearch) return;
  g_nextWindowSearch = now + 1.0;
  HWND found = nullptr;
  EnumWindows(FindGameWindowProc, reinterpret_cast<LPARAM>(&found));
  if (!found) return;
  const LONG_PTR previous = GetWindowLongPtrW(found, GWLP_WNDPROC);
  if (!previous) return;
  g_originalWndProc = reinterpret_cast<WNDPROC>(previous);
  if (!SetWindowLongPtrW(found, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&GameWndProc))) {
    LOG("Couldn't subclass the game window (error %lu)", GetLastError());
    return;
  }
  g_hwnd = found;
  memset(g_buttonMine, 0, sizeof g_buttonMine);
  memset(g_keyOwner, 0, sizeof g_keyOwner);
  LOG("Listening for input on window %p", static_cast<void*>(found));
}

std::vector<MouseEvent> TakeEvents() {
  std::vector<MouseEvent> events;
  events.swap(g_events);
  return events;
}

bool CursorClientPos(float& x, float& y, float& clientW, float& clientH) {
  if (!g_hwnd) return false;
  POINT pt;
  RECT rc;
  if (!GetCursorPos(&pt) || !ScreenToClient(g_hwnd, &pt) || !GetClientRect(g_hwnd, &rc)) return false;
  if (rc.right <= 0 || rc.bottom <= 0) return false;
  x = static_cast<float>(pt.x);
  y = static_cast<float>(pt.y);
  clientW = static_cast<float>(rc.right);
  clientH = static_cast<float>(rc.bottom);
  return true;
}

bool OsCursorVisible() {
  CURSORINFO info{};
  info.cbSize = sizeof info;
  return GetCursorInfo(&info) && (info.flags & CURSOR_SHOWING) && info.hCursor;
}

void MoveCursorToClient(float x, float y) {
  if (!g_hwnd) return;
  POINT pt{static_cast<LONG>(x), static_cast<LONG>(y)};
  if (ClientToScreen(g_hwnd, &pt)) SetCursorPos(pt.x, pt.y);
}

}  // namespace input
