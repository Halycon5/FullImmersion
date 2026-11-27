// FullImmersion M1: the flat room. A 3D room over your monitor with your real windows pinned round you.
//
//   On a panel: your mouse and keyboard work the real window, as if it were flat on the desk.
//   Drag a panel's handle (the bar under it) to move it round you; scroll on the handle to resize it
//   (Shift+scroll: nearer or farther). Right-click the handle for the panel's menu.
//   Drag empty space to look round; scroll on empty space to zoom. Right-click empty space to pin a window.
//   Ctrl+Alt+F hides or shows the room; Ctrl+Alt+Q quits. The layout is kept in %APPDATA%\FullImmersion.
#include "app/passthrough.h"
#include "app/renderer.h"
#include "common/capture.h"
#include <windowsx.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>

using namespace fi;

namespace {

constexpr int kHotkeyToggle = 1, kHotkeyQuit = 2;
constexpr UINT kMenuPinBase = 1000, kMenuQuit = 1, kMenuHide = 2;
constexpr UINT kMenuCloser = 10, kMenuFarther = 11, kMenuBigger = 12, kMenuSmaller = 13, kMenuUnpin = 14,
               kMenuNative = 15;

std::wstring data_dir() {
  PWSTR p = nullptr;
  std::wstring dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) dir = std::wstring(p) + L"\\FullImmersion";
  CoTaskMemFree(p);
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

FILE* g_log = nullptr;
void logf(const char* fmt, ...) {
  if (!g_log) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  fprintf(g_log, "%02d:%02d:%02d.%03d  ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
  va_list a;
  va_start(a, fmt);
  vfprintf(g_log, fmt, a);
  va_end(a);
  fputc('\n', g_log);
  fflush(g_log);
}

enum class Drag { None, Look, Move };

struct App {
  HWND hwnd = nullptr;
  winrt::com_ptr<ID3D11Device> device;
  Renderer renderer;
  std::unique_ptr<Passthrough> pass;
  Room room;
  std::map<uint64_t, std::unique_ptr<WindowCapture>> captures;
  std::wstring layout_path;
  bool dirty = false;
  double dirty_at = 0, last_reattach = 0;
  bool visible = true, running = true;

  Drag drag = Drag::None;
  int drag_panel = -1;
  POINT drag_from{};
  float drag_yaw = 0, drag_pitch = 0;

  std::optional<Hit> hover;  // what the cursor is on now

  void touch() {
    dirty = true;
    dirty_at = now_ms();
  }

  // --- the layout ---
  void load() {
    std::ifstream f(layout_path, std::ios::binary);
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    if (room.from_json(ss.str())) logf("layout loaded: %zu panels", room.panels.size());
    else logf("layout unreadable: starting empty");
  }
  void save() {
    std::wstring tmp = layout_path + L".tmp";
    {
      std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
      f << room.to_json();
    }
    MoveFileExW(tmp.c_str(), layout_path.c_str(), MOVEFILE_REPLACE_EXISTING);
    dirty = false;
  }

  std::vector<OpenWindow> open_windows() {
    std::vector<OpenWindow> out;
    for (auto& w : list_windows())
      if (w.hwnd != hwnd) out.push_back({(uint64_t)(uintptr_t)w.hwnd, narrow(w.exe), narrow(w.title)});
    return out;
  }

  // --- panels and their windows ---
  static HWND win_of(const Panel& p) { return (HWND)(uintptr_t)p.window; }

  void ready_window(HWND w) {
    if (IsIconic(w) || IsZoomed(w)) ShowWindow(w, SW_RESTORE);  // a minimised window sends no pictures
  }

  void pin(const OpenWindow& w) {
    HWND h = (HWND)(uintptr_t)w.window;
    ready_window(h);
    RECT fr = frame_bounds(h);
    float px_w = float(std::max<LONG>(1, fr.right - fr.left)), px_h = float(std::max<LONG>(1, fr.bottom - fr.top));
    Panel p;
    p.window = w.window;
    p.exe = w.exe;
    p.title = w.title;
    p.aspect = px_h / px_w;
    p.width = std::clamp(px_w / 1400.0f, 0.4f, 2.5f);
    room.place_new(p);
    room.panels.push_back(p);
    logf("pinned %s \"%s\"", p.exe.c_str(), p.title.c_str());
    touch();
  }

  void unpin(int i) {
    if (i < 0 || i >= (int)room.panels.size()) return;
    captures.erase(room.panels[i].window);
    logf("unpinned %s", room.panels[i].exe.c_str());
    room.panels.erase(room.panels.begin() + i);
    pass->close();
    touch();
  }

  void update_windows() {
    for (auto& p : room.panels) {
      if (!p.window) continue;
      HWND w = win_of(p);
      if (!IsWindow(w)) {
        logf("window gone: %s", p.exe.c_str());
        captures.erase(p.window);
        p.window = 0;
        continue;
      }
      auto it = captures.find(p.window);
      if (it == captures.end()) {
        try {
          ready_window(w);
          it = captures.emplace(p.window, std::make_unique<WindowCapture>(device.get(), w, true)).first;
        } catch (winrt::hresult_error const& e) {
          logf("can't capture %s: 0x%08X", p.exe.c_str(), (unsigned)e.code().value);
          p.window = 0;
          continue;
        }
      }
      auto& cap = *it->second;
      if (cap.poll() && cap.width() && cap.height()) {
        float a = float(cap.height()) / float(cap.width());
        if (std::fabs(a - p.aspect) > 0.002f) {
          p.aspect = a;
          touch();
        }
      }
    }
    if (now_ms() - last_reattach > 2000) {
      last_reattach = now_ms();
      bool missing = std::any_of(room.panels.begin(), room.panels.end(), [](auto& p) { return p.window == 0; });
      if (missing && reattach(room.panels, open_windows()) > 0) logf("found windows again");
    }
  }

  ID3D11ShaderResourceView* picture_of(const Panel& p) {
    auto it = captures.find(p.window);
    if (!p.window || it == captures.end() || !it->second->texture()) return nullptr;
    return it->second->srv(true);
  }

  // --- pointing ---
  bool cursor_in_room(POINT& local) {
    GetCursorPos(&local);
    ScreenToClient(hwnd, &local);
    return local.x >= 0 && local.y >= 0 && local.x < (LONG)renderer.width() && local.y < (LONG)renderer.height();
  }
  std::optional<Hit> pick_at(POINT local) {
    return room.pick(renderer.ray_through(room.camera, float(local.x), float(local.y)));
  }
  bool on_live_picture(const std::optional<Hit>& h) {
    return h && !h->handle && room.panels[h->panel].window && picture_of(room.panels[h->panel]);
  }

  // The mouse, as it reached the room.
  void on_mouse(UINT msg, WPARAM wp, LPARAM lp) {
    POINT local{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) {  // wheel positions come in screen coordinates
      ScreenToClient(hwnd, &local);
    }

    if (drag != Drag::None) {
      if (msg == WM_MOUSEMOVE) {
        if (drag == Drag::Look) {
          room.camera.yaw = drag_yaw + float(local.x - drag_from.x) * 0.0035f;
          room.camera.pitch = std::clamp(drag_pitch - float(local.y - drag_from.y) * 0.0035f, -1.45f, 1.45f);
        } else if (drag == Drag::Move && drag_panel >= 0 && drag_panel < (int)room.panels.size()) {
          Panel& p = room.panels[drag_panel];
          float yaw, h;
          Room::place_on_ray(renderer.ray_through(room.camera, float(local.x), float(local.y)), p.distance, yaw, h);
          p.yaw = yaw;
          p.height = h + p.panel_height() / 2 + Panel::kHandleGap + Panel::kHandleHeight / 2;
        }
        touch();
      } else if (msg == WM_LBUTTONUP) {
        drag = Drag::None;
        drag_panel = -1;
        ReleaseCapture();
      }
      return;
    }

    auto hit = pick_at(local);
    if (on_live_picture(hit)) {
      // On a window's picture: it's the window's mouse.
      HWND target = win_of(room.panels[hit->panel]);
      if (pass->line_up(target, hit->u, hit->v)) {
        switch (msg) {
          case WM_MOUSEMOVE: pass->send_move(); break;
          case WM_MOUSEWHEEL: pass->send_wheel(GET_WHEEL_DELTA_WPARAM(wp), false); break;
          case WM_MOUSEHWHEEL: pass->send_wheel(GET_WHEEL_DELTA_WPARAM(wp), true); break;
          default: pass->send_button(msg); break;
        }
      }
      return;
    }
    pass->close();

    switch (msg) {
      case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        drag_from = local;
        if (hit && hit->handle) {
          drag = Drag::Move;
          drag_panel = hit->panel;
        } else {
          drag = Drag::Look;
          drag_yaw = room.camera.yaw;
          drag_pitch = room.camera.pitch;
        }
        break;
      case WM_MOUSEWHEEL: {
        float steps = float(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
        if (hit && hit->handle) {
          Panel& p = room.panels[hit->panel];
          if (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) p.distance = std::clamp(p.distance * std::pow(0.92f, steps), 0.4f, 8.0f);
          else p.width = std::clamp(p.width * std::pow(1.08f, steps), 0.15f, 8.0f);
        } else {
          room.camera.fov_y = std::clamp(room.camera.fov_y * std::pow(0.93f, steps), 0.35f, 1.9f);
        }
        touch();
        break;
      }
      case WM_RBUTTONUP:
        if (hit && hit->handle) panel_menu(hit->panel);
        else room_menu();
        break;
      default: break;
    }
  }

  // --- menus ---
  void room_menu() {
    auto wins = open_windows();
    HMENU pinm = CreatePopupMenu();
    int n = 0;
    for (size_t i = 0; i < wins.size() && i < 200; ++i) {
      bool pinned = std::any_of(room.panels.begin(), room.panels.end(), [&](auto& p) { return p.window == wins[i].window; });
      RECT fr = frame_bounds((HWND)(uintptr_t)wins[i].window);
      if (pinned) continue;
      std::wstring label = widen(wins[i].title).substr(0, 70) + L"\t" + widen(wins[i].exe);
      if (IsIconic((HWND)(uintptr_t)wins[i].window) || fr.right - fr.left < 200) label += L" (minimised)";
      AppendMenuW(pinm, MF_STRING, kMenuPinBase + UINT(i), label.c_str());
      ++n;
    }
    if (!n) AppendMenuW(pinm, MF_STRING | MF_GRAYED, 0, L"(every window is pinned)");
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_POPUP, (UINT_PTR)pinm, L"Pin a window");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kMenuHide, L"Hide the room\tCtrl+Alt+F");
    AppendMenuW(m, MF_STRING, kMenuQuit, L"Quit FullImmersion\tCtrl+Alt+Q");
    UINT cmd = track(m);
    DestroyMenu(m);
    if (cmd >= kMenuPinBase && cmd - kMenuPinBase < wins.size()) pin(wins[cmd - kMenuPinBase]);
    else if (cmd == kMenuHide) toggle();
    else if (cmd == kMenuQuit) running = false;
  }

  void panel_menu(int i) {
    Panel& p = room.panels[i];
    HMENU m = CreatePopupMenu();
    std::wstring head = widen(p.title.empty() ? p.exe : p.title).substr(0, 60) + (p.window ? L"" : L" (closed)");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, head.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kMenuBigger, L"Bigger\tscroll up on the handle");
    AppendMenuW(m, MF_STRING, kMenuSmaller, L"Smaller\tscroll down");
    AppendMenuW(m, MF_STRING, kMenuCloser, L"Closer\tShift+scroll");
    AppendMenuW(m, MF_STRING, kMenuFarther, L"Farther");
    AppendMenuW(m, MF_STRING, kMenuNative, L"Window's own size (1:1 at this distance)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kMenuUnpin, L"Unpin");
    UINT cmd = track(m);
    DestroyMenu(m);
    if (i >= (int)room.panels.size()) return;
    Panel& q = room.panels[i];
    switch (cmd) {
      case kMenuBigger: q.width *= 1.25f; break;
      case kMenuSmaller: q.width /= 1.25f; break;
      case kMenuCloser: q.distance = std::max(0.4f, q.distance * 0.8f); break;
      case kMenuFarther: q.distance = std::min(8.0f, q.distance * 1.25f); break;
      case kMenuNative: {
        // As many metres as the window's pixels take up on the monitor at this distance and zoom.
        if (auto it = captures.find(q.window); it != captures.end() && it->second->width()) {
          float px_per_m = float(renderer.height()) / (2 * std::tan(room.camera.fov_y / 2) * q.distance);
          q.width = float(it->second->width()) / px_per_m;
        }
        break;
      }
      case kMenuUnpin: unpin(i); return;
      default: return;
    }
    touch();
  }

  UINT track(HMENU m) {
    POINT c;
    GetCursorPos(&c);
    SetForegroundWindow(hwnd);  // menus need their owner in front; we just had the click, so Windows allows it
    UINT cmd = (UINT)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, c.x, c.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    return cmd;
  }

  void toggle() {
    visible = !visible;
    pass->close();
    ShowWindow(hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    logf(visible ? "room shown" : "room hidden");
  }

  // --- the frame ---
  void frame() {
    update_windows();
    POINT local;
    hover.reset();
    if (visible && drag == Drag::None && cursor_in_room(local)) hover = pick_at(local);
    if (!on_live_picture(hover) && pass->target() && GetCapture() == nullptr && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000))
      pass->close();  // the pointer has left the picture: the room takes the mouse back

    std::vector<PanelDraw> draws;
    for (int i = 0; i < (int)room.panels.size(); ++i) {
      PanelDraw d;
      d.panel = &room.panels[i];
      d.picture = picture_of(room.panels[i]);
      d.hovered = hover && hover->panel == i && !hover->handle;
      d.handle_hovered = hover && hover->panel == i && hover->handle;
      d.grabbed = drag == Drag::Move && drag_panel == i;
      draws.push_back(d);
    }
    if (visible) {
      renderer.draw(room.camera, draws);
      renderer.present();
    } else {
      Sleep(16);
    }
    if (dirty && now_ms() - dirty_at > 800) save();
  }
};

App* g_app = nullptr;

LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  App* app = g_app;
  switch (msg) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;  // clicking the room never takes the keyboard from your window
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
      if (app && app->pass) app->on_mouse(msg, wp, lp);
      return 0;
    case WM_HOTKEY:
      if (app && wp == kHotkeyToggle) app->toggle();
      if (app && wp == kHotkeyQuit) app->running = false;
      return 0;
    case WM_SIZE:
      if (app) app->renderer.resize(LOWORD(lp), HIWORD(lp));
      return 0;
    case WM_CLOSE:
      if (app) app->running = false;
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
  make_dpi_aware();
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  App app;
  g_app = &app;
  std::wstring dir = data_dir();
  app.layout_path = dir + L"\\layout.json";
  _wfopen_s(&g_log, (dir + L"\\log.txt").c_str(), L"a");
  logf("--- FullImmersion starting");

  // The room covers the monitor the cursor is on.
  POINT c;
  GetCursorPos(&c);
  MONITORINFO mi{sizeof mi};
  GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTOPRIMARY), &mi);
  RECT r = mi.rcMonitor;

  WNDCLASSEXW wc{sizeof wc};
  wc.lpfnWndProc = wndproc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = L"FullImmersionRoom";
  wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  RegisterClassExW(&wc);
  app.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, wc.lpszClassName, L"FullImmersion", WS_POPUP, r.left,
                             r.top, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
  if (!app.hwnd) return 1;

  try {
    app.device = make_device();
    app.renderer.init(app.hwnd, app.device.get());
  } catch (std::exception const& e) {
    logf("renderer failed: %s", e.what());
    MessageBoxA(nullptr, e.what(), "FullImmersion", MB_ICONERROR);
    return 1;
  } catch (winrt::hresult_error const& e) {
    logf("renderer failed: 0x%08X", (unsigned)e.code().value);
    return 1;
  }
  app.pass = std::make_unique<Passthrough>(app.hwnd);
  app.load();
  reattach(app.room.panels, app.open_windows());

  if (!RegisterHotKey(app.hwnd, kHotkeyToggle, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'F'))
    logf("Ctrl+Alt+F is taken by another program");
  if (!RegisterHotKey(app.hwnd, kHotkeyQuit, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q'))
    logf("Ctrl+Alt+Q is taken by another program");
  ShowWindow(app.hwnd, SW_SHOWNOACTIVATE);
  logf("room on %ldx%ld at %ld,%ld", r.right - r.left, r.bottom - r.top, r.left, r.top);

  MSG msg;
  while (app.running) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) app.running = false;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (!app.running) break;
    try {
      app.frame();
    } catch (winrt::hresult_error const& e) {
      logf("frame failed: 0x%08X %s", (unsigned)e.code().value, narrow(e.message()).c_str());
      Sleep(100);
    }
  }
  app.pass->close();
  app.save();
  app.captures.clear();
  logf("--- FullImmersion closed");
  DestroyWindow(app.hwnd);
  if (g_log) fclose(g_log);
  return 0;
}
