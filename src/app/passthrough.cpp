#include "app/passthrough.h"

namespace fi {

bool Passthrough::line_up(HWND target, float u, float v) {
  if (!IsWindow(target) || IsIconic(target)) return false;
  POINT c;
  GetCursorPos(&c);
  RECT frame = frame_bounds(target), win;
  GetWindowRect(target, &win);
  LONG fw = frame.right - frame.left, fh = frame.bottom - frame.top;
  LONG want_x = c.x - LONG(u * float(fw)), want_y = c.y - LONG(v * float(fh));
  LONG dx = want_x - frame.left, dy = want_y - frame.top;
  // Async: a window that has stopped answering can never hang the room.
  UINT flags = SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS;
  if (dx == 0 && dy == 0) flags |= SWP_NOMOVE;
  // To the top of the ordinary windows (the room, topmost, stays above it), so the hole shows it and no other.
  SetWindowPos(target, HWND_TOP, win.left + dx, win.top + dy, 0, 0, flags);
  target_ = target;

  POINT local = c;
  ScreenToClient(room_, &local);
  if (local.x != hole_.x || local.y != hole_.y) {
    RECT rc;
    GetClientRect(room_, &rc);
    HRGN all = CreateRectRgn(0, 0, rc.right, rc.bottom);
    HRGN dot = CreateRectRgn(local.x, local.y, local.x + 1, local.y + 1);
    CombineRgn(all, all, dot, RGN_DIFF);
    DeleteObject(dot);
    SetWindowRgn(room_, all, FALSE);  // the system owns `all` from here
    hole_ = local;
  }
  return true;
}

void Passthrough::close() {
  if (hole_.x != -1) SetWindowRgn(room_, nullptr, FALSE);
  hole_ = {-1, -1};
  target_ = nullptr;
}

void Passthrough::send_move() {
  POINT c;
  GetCursorPos(&c);
  int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  INPUT in{};
  in.type = INPUT_MOUSE;
  in.mi.dx = LONG((double(c.x - vx) * 65535.0) / double(vw - 1) + 0.5);
  in.mi.dy = LONG((double(c.y - vy) * 65535.0) / double(vh - 1) + 0.5);
  in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
  SendInput(1, &in, sizeof in);
}

void Passthrough::send_button(UINT msg) {
  INPUT in{};
  in.type = INPUT_MOUSE;
  switch (msg) {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: in.mi.dwFlags = MOUSEEVENTF_LEFTDOWN; break;
    case WM_LBUTTONUP: in.mi.dwFlags = MOUSEEVENTF_LEFTUP; break;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: in.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN; break;
    case WM_RBUTTONUP: in.mi.dwFlags = MOUSEEVENTF_RIGHTUP; break;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: in.mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN; break;
    case WM_MBUTTONUP: in.mi.dwFlags = MOUSEEVENTF_MIDDLEUP; break;
    default: return;
  }
  SendInput(1, &in, sizeof in);
}

void Passthrough::send_wheel(int delta, bool horizontal) {
  INPUT in{};
  in.type = INPUT_MOUSE;
  in.mi.mouseData = DWORD(delta);
  in.mi.dwFlags = horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
  SendInput(1, &in, sizeof in);
}

}  // namespace fi
