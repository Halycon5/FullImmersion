// The flat room, end to end, with simulated hands: the mouse and keys go in through SendInput, the same
// pipeline a real mouse uses, and the results are read back from the windows, the captures and the layout.
//
// Start FullImmersion with a layout whose first panel is a window you don't mind typing into (the S3 test
// page in Chrome), then run this. It moves the real cursor and types: nobody should be using the PC.
#include "common/capture.h"
#include "core/room.h"
#include <shlobj.h>
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace fi;

static int g_failed = 0;
static void check(bool ok, const char* what, const char* detail = "") {
  printf("%s  %s %s\n", ok ? "PASS" : "FAIL", what, detail);
  if (!ok) ++g_failed;
}

static std::wstring layout_path() {
  PWSTR p = nullptr;
  SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p);
  std::wstring s = std::wstring(p) + L"\\FullImmersion\\layout.json";
  CoTaskMemFree(p);
  return s;
}
static bool load_room(Room& room) {
  std::ifstream f(layout_path(), std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return room.from_json(ss.str());
}

static void move_to(POINT to, int steps = 8) {
  POINT from;
  GetCursorPos(&from);
  int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  for (int i = 1; i <= steps; ++i) {
    LONG x = from.x + (to.x - from.x) * i / steps, y = from.y + (to.y - from.y) * i / steps;
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = LONG((double(x - vx) * 65535.0) / double(vw - 1) + 0.5);
    in.mi.dy = LONG((double(y - vy) * 65535.0) / double(vh - 1) + 0.5);
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &in, sizeof in);
    Sleep(16);
  }
}
static void button(DWORD flag) {
  INPUT in{};
  in.type = INPUT_MOUSE;
  in.mi.dwFlags = flag;
  SendInput(1, &in, sizeof in);
  Sleep(40);
}
static void wheel(int delta) {
  INPUT in{};
  in.type = INPUT_MOUSE;
  in.mi.dwFlags = MOUSEEVENTF_WHEEL;
  in.mi.mouseData = DWORD(delta);
  SendInput(1, &in, sizeof in);
  Sleep(40);
}
static void type(const wchar_t* s) {
  for (; *s; ++s) {
    INPUT in[2]{};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wScan = in[1].ki.wScan = *s;
    in[0].ki.dwFlags = KEYEVENTF_UNICODE;
    in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
    Sleep(10);
  }
}
static void chord(std::initializer_list<WORD> keys) {
  for (WORD k : keys) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = k;
    SendInput(1, &in, sizeof in);
  }
  for (auto it = std::rbegin(keys); it != std::rend(keys); ++it) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = *it;
    in.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof in);
  }
  Sleep(300);
}

struct View {
  RECT rc{};  // the room's client area on screen
  Camera cam;
  POINT at(Vec3 p) const {
    float px = 0, py = 0;
    cam.project(p, float(rc.right - rc.left), float(rc.bottom - rc.top), px, py);
    return {rc.left + LONG(px + 0.5f), rc.top + LONG(py + 0.5f)};
  }
};

static double changed(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, UINT w, RECT box) {
  if (a.size() != b.size()) return 1;
  size_t n = 0, total = 0;
  for (LONG y = std::max(0L, box.top); y < box.bottom; ++y)
    for (LONG x = std::max(0L, box.left); x < box.right; ++x) {
      size_t i = size_t(y) * w + size_t(x);
      if (i >= a.size()) continue;
      ++total;
      n += (a[i] & 0xFFFFFF) != (b[i] & 0xFFFFFF);
    }
  return total ? double(n) / double(total) : 0;
}

int wmain() {
  make_dpi_aware();
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  HWND room_hwnd = FindWindowW(L"FullImmersionRoom", nullptr);
  if (!room_hwnd) {
    printf("FAIL  FullImmersion isn't running\n");
    return 1;
  }
  Room room;
  if (!load_room(room) || room.panels.empty()) {
    printf("FAIL  no layout with a panel\n");
    return 1;
  }
  View view;
  GetClientRect(room_hwnd, &view.rc);
  MapWindowPoints(room_hwnd, nullptr, reinterpret_cast<POINT*>(&view.rc), 2);
  view.cam = room.camera;
  const Panel& panel = room.panels[0];
  HWND target = find_window(widen(panel.title));
  char detail[256];
  snprintf(detail, sizeof detail, "(%s)", panel.title.c_str());
  check(target != nullptr, "the first panel's window is open", detail);
  if (!target) return 1;

  auto device = make_device();
  WindowCapture room_cap(device.get(), room_hwnd), target_cap(device.get(), target);
  room_cap.wait_frame(2000);
  target_cap.wait_frame(2000);
  auto shot = [&](WindowCapture& c) {
    double end = now_ms() + 250;
    while (now_ms() < end) { c.poll(); Sleep(5); }
    return read_pixels(device.get(), c.texture(), c.width(), c.height());
  };

  // 1. Point at the panel's middle, click, type: the real window should take it.
  POINT mid = view.at(point_on(panel, 0.5f, 0.6f));
  auto room_before = shot(room_cap), target_before = shot(target_cap);
  move_to(mid);
  Sleep(150);
  button(MOUSEEVENTF_LEFTDOWN);
  button(MOUSEEVENTF_LEFTUP);
  Sleep(300);
  check(GetForegroundWindow() == target, "a click on the panel gives its window the keyboard");
  check(GetAncestor(WindowFromPoint({view.rc.left + 5, view.rc.top + 5}), GA_ROOT) == room_hwnd,
        "the room still covers the monitor");
  check((GetWindowLongW(room_hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0, "the room stays on top");
  type(L"Typed through FullImmersion's room. ");
  Sleep(600);
  auto target_after = shot(target_cap);
  RECT whole{0, 0, (LONG)target_cap.width(), (LONG)target_cap.height()};
  double t_changed = changed(target_before, target_after, target_cap.width(), whole);
  snprintf(detail, sizeof detail, "(%.2f%% of the window changed)", t_changed * 100);
  check(t_changed > 0.002, "the typing landed in the real window", detail);
  auto room_after = shot(room_cap);
  POINT tl = view.at(point_on(panel, 0, 0)), br = view.at(point_on(panel, 1, 1));
  RECT on_room{std::min(tl.x, br.x) - view.rc.left, std::min(tl.y, br.y) - view.rc.top,
               std::max(tl.x, br.x) - view.rc.left, std::max(tl.y, br.y) - view.rc.top};
  double r_changed = changed(room_before, room_after, room_cap.width(), on_room);
  snprintf(detail, sizeof detail, "(%.2f%% of the panel changed)", r_changed * 100);
  check(r_changed > 0.0005, "the panel in the room shows it live", detail);
  CreateDirectoryW(L"out", nullptr);
  save_bmp(room_after, room_cap.width(), room_cap.height(), L"out\\e2e_room.bmp");

  // 2. Scroll on the handle: the panel grows.
  POINT handle = view.at(handle_point(panel));
  move_to(handle);
  Sleep(100);
  wheel(WHEEL_DELTA * 2);
  Sleep(1200);
  Room r2;
  load_room(r2);
  snprintf(detail, sizeof detail, "(%.3f -> %.3f m)", panel.width, r2.panels.empty() ? 0.f : r2.panels[0].width);
  check(!r2.panels.empty() && r2.panels[0].width > panel.width * 1.1f, "scrolling on the handle resizes", detail);
  if (r2.panels.empty()) return 1;

  // 3. Drag the handle to the right: the panel moves round you.
  view.cam = r2.camera;
  handle = view.at(handle_point(r2.panels[0]));
  move_to(handle);
  Sleep(100);
  button(MOUSEEVENTF_LEFTDOWN);
  move_to({handle.x + 300, handle.y}, 12);
  button(MOUSEEVENTF_LEFTUP);
  Sleep(1200);
  Room r3;
  load_room(r3);
  snprintf(detail, sizeof detail, "(yaw %.3f -> %.3f)", r2.panels[0].yaw, r3.panels.empty() ? 0.f : r3.panels[0].yaw);
  check(!r3.panels.empty() && r3.panels[0].yaw > r2.panels[0].yaw + 0.1f, "dragging the handle moves the panel",
        detail);

  // 4. Drag empty sky: you look round.
  POINT sky{view.rc.left + (view.rc.right - view.rc.left) / 10, view.rc.top + (view.rc.bottom - view.rc.top) / 8};
  move_to(sky);
  Sleep(100);
  button(MOUSEEVENTF_LEFTDOWN);
  move_to({sky.x + 200, sky.y}, 10);
  button(MOUSEEVENTF_LEFTUP);
  Sleep(1200);
  Room r4;
  load_room(r4);
  snprintf(detail, sizeof detail, "(yaw %.3f -> %.3f)", r3.camera.yaw, r4.camera.yaw);
  check(r4.camera.yaw > r3.camera.yaw + 0.2f, "dragging empty space turns the view", detail);

  // 5. Ctrl+Alt+F hides and shows the room; Ctrl+Alt+Q quits and keeps the layout.
  chord({VK_CONTROL, VK_MENU, 'F'});
  check(!IsWindowVisible(room_hwnd), "Ctrl+Alt+F hides the room");
  chord({VK_CONTROL, VK_MENU, 'F'});
  check(IsWindowVisible(room_hwnd) != FALSE, "Ctrl+Alt+F shows it again");
  chord({VK_CONTROL, VK_MENU, 'Q'});
  Sleep(1500);
  check(!IsWindow(room_hwnd), "Ctrl+Alt+Q quits");
  Room r5;
  check(load_room(r5) && r5.panels.size() == r4.panels.size(), "the layout survives quitting");

  printf("%s: %d failed\n", g_failed ? "FAILED" : "ALL PASSED", g_failed);
  return g_failed ? 1 : 0;
}
