#include "common/input.h"
#include <vector>

namespace fi {

POINT uv_to_screen(HWND hwnd, float u, float v) {
  RECT r = frame_bounds(hwnd);
  return POINT{r.left + LONG(u * float(r.right - r.left)), r.top + LONG(v * float(r.bottom - r.top))};
}

static bool owns_point(HWND hwnd, POINT pt) {
  HWND hit = WindowFromPoint(pt);
  return hit && GetAncestor(hit, GA_ROOT) == hwnd;
}

InputResult raise_and_focus(HWND hwnd) {
  if (!IsWindow(hwnd)) return {false, "the window is gone"};
  if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
  if (GetForegroundWindow() == hwnd) return {true, {}};
  SetForegroundWindow(hwnd);
  if (GetForegroundWindow() != hwnd) {
    // Windows refuses focus to a process that isn't in front; raising it lets the click that follows take focus.
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
  }
  Sleep(50);
  return {true, {}};
}

InputResult click(HWND hwnd, float u, float v, bool restore_cursor) {
  auto r = raise_and_focus(hwnd);
  if (!r.ok) return r;
  POINT pt = uv_to_screen(hwnd, u, v);
  if (!owns_point(hwnd, pt)) return {false, "something else is on top of the window at that point"};
  POINT was{};
  GetCursorPos(&was);
  SetCursorPos(pt.x, pt.y);
  INPUT in[2]{};
  in[0].type = in[1].type = INPUT_MOUSE;
  in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
  in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
  UINT sent = SendInput(2, in, sizeof(INPUT));
  Sleep(40);
  if (restore_cursor) SetCursorPos(was.x, was.y);
  if (sent != 2) return {false, "SendInput was blocked (a window running as admin, or the secure desktop)"};
  return {true, {}};
}

InputResult type_text(HWND hwnd, std::wstring_view text) {
  if (GetForegroundWindow() != hwnd) {
    auto r = raise_and_focus(hwnd);
    if (!r.ok) return r;
  }
  if (GetForegroundWindow() != hwnd) return {false, "the window wouldn't take the keyboard, so nothing was typed"};
  std::vector<INPUT> in;
  for (wchar_t c : text) {
    INPUT d{}, u{};
    d.type = u.type = INPUT_KEYBOARD;
    if (c == L'\n') {
      d.ki.wVk = u.ki.wVk = VK_RETURN;
    } else {
      d.ki.wScan = u.ki.wScan = c;
      d.ki.dwFlags = KEYEVENTF_UNICODE;
    }
    u.ki.dwFlags = d.ki.dwFlags | KEYEVENTF_KEYUP;
    in.push_back(d);
    in.push_back(u);
  }
  UINT sent = SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
  if (sent != in.size()) return {false, "SendInput was blocked partway"};
  return {true, {}};
}

}  // namespace fi
