// FullImmersion M1: the flat room. A 3D room over your monitor with your real windows pinned round you.
//
//   On a panel: your mouse and keyboard work the real window, as if it were flat on the desk.
//   Drag a panel's handle (the bar under it) to move it round you; scroll on the handle to resize it
//   (Shift+scroll: nearer or farther). Right-click the handle for the panel's menu.
//   Drag empty space to look round; scroll on empty space to zoom. Right-click empty space to pin a window.
//   Ctrl+Alt+F hides or shows the room; Ctrl+Alt+Q quits. The layout is kept in %APPDATA%\FullImmersion.
//
// M2, `FullImmersion --vr`: the same room in the headset. Each controller is a pointer.
//   Trigger on a window: the window's left button (the cursor follows that hand). A: its right button.
//   Stick on a window: scroll it. Grip on a panel (or trigger on its handle): grab it and carry it round you;
//   while holding, stick up/down pushes it away or pulls it in, left/right makes it smaller or bigger.
//   A small window on the monitor mirrors the headset; right-click it to pin a window or quit.
//
// M3: agents come in through a door on localhost (src/app/agent_door.h, src/core/agents.h). They can read the
// scene, pin, move and arrange panels, leave notes, say what they're doing and get a picture of the room. They
// show as name cards round you, with a line to what they're looking at. They can't click or type into windows.
#include "app/agent_door.h"
#include "app/cards.h"
#include "app/passthrough.h"
#include "app/renderer.h"
#include "app/xr.h"
#include "common/capture.h"
#include "common/input.h"
#include <set>
#include <windowsx.h>
#include <shellapi.h>
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
  std::set<uint64_t> fresh;  // windows with a new picture this frame

  // --- agents (M3) ---
  AgentDesk desk{room};
  AgentDoor door;
  std::unique_ptr<Cards> cards;
  std::vector<Panel> extras;  // this frame's agent cards and notes, drawn like panels
  std::vector<ID3D11ShaderResourceView*> extra_pictures;
  std::vector<Beam> agent_beams;

  // --- the headset (M2) ---
  bool vr = false;
  std::unique_ptr<Xr> xr;
  struct HandState {
    bool select = false, squeeze = false, alt = false;  // last frame's buttons, to see presses
    int grabbed = -1;                                   // the panel this hand carries
    float grab_dyaw = 0, grab_dh = 0;
    HWND button_down = nullptr, right_down = nullptr;  // a real window holding this hand's button
    float scroll = 0;                                   // stick scroll not sent yet, in wheel units
    std::optional<Hit> hit;
  } hands[2];
  int mouse_hand = 1;  // the hand the cursor follows: the last one to click (the right to begin with)
  Pose head;
  double vr_stat_at = 0, vr_cpu_ms = 0;
  int vr_frames = 0, vr_dropped = 0;

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

  int pin(const OpenWindow& w) {
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
    return int(room.panels.size()) - 1;
  }

  void unpin(int i) {
    if (i < 0 || i >= (int)room.panels.size()) return;
    captures.erase(room.panels[i].window);
    logf("unpinned %s", room.panels[i].exe.c_str());
    room.panels.erase(room.panels.begin() + i);
    for (auto& a : desk.agents) {
      if (a.looking_at == i) a.looking_at = -1;
      else if (a.looking_at > i) --a.looking_at;
    }
    for (auto& h : hands) {
      if (h.grabbed == i) h.grabbed = -1;
      else if (h.grabbed > i) --h.grabbed;
      h.hit.reset();
    }
    pass->close();
    touch();
  }

  void update_windows() {
    fresh.clear();
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
        fresh.insert(p.window);
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
    std::wstring title = widen(p.title.empty() ? p.exe : p.title).substr(0, 60) + (p.window ? L"" : L" (closed)");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, title.c_str());
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

  // --- agents ---
  void open_door(const std::wstring& dir) {
    cards = std::make_unique<Cards>(device.get());
    desk.open_windows = [this] { return open_windows(); };
    desk.pin = [this](const OpenWindow& w) { return pin(w); };
    desk.unpin = [this](int i) { unpin(i); };
    desk.is_live = [this](int i) { return picture_of(room.panels[i]) != nullptr; };
    std::string err;
    if (door.open(dir, err)) logf("agents' door open on 127.0.0.1:%d", door.port());
    else logf("agents' door didn't open: %s", err.c_str());
  }

  // Requests from agents, then their cards, notes and lines for this frame.
  void agents_frame() {
    double now = now_ms();
    door.serve([&](const AgentRequest& req) {
      if (req.method == "GET" && req.path == "/look") return look(req);
      AgentReply r = desk.handle(req, now);
      if (r.status == 200 && req.method == "POST") logf("agent %s: %s %s", req.agent.c_str(), req.path.c_str(), req.body.substr(0, 200).c_str());
      if (desk.changed) {
        desk.changed = false;
        touch();
      }
      return r;
    });
    desk.tick(now);

    static const uint32_t palette[] = {0xC08CFF, 0x6FD3FF, 0xFF9F6F, 0x7FE0A0, 0xFFD36F, 0xFF7FB8};
    extras.clear();
    extra_pictures.clear();
    agent_beams.clear();
    extras.reserve(desk.agents.size() + desk.notes.size());
    for (auto& a : desk.agents) {
      uint32_t colour = palette[std::hash<std::string>{}(a.name) % 6];
      Panel c;
      c.exe = "agent";
      c.yaw = a.yaw;
      c.height = 0.42f;
      c.distance = 1.75f;
      c.width = 0.32f;
      auto* pic = cards->get(widen(a.name + ", " + a.state), widen(a.says), colour, c.aspect);
      extras.push_back(c);
      extra_pictures.push_back(pic);
      if (a.looking_at >= 0 && a.looking_at < (int)room.panels.size()) {
        const Panel& t = room.panels[a.looking_at];
        Beam b;
        b.from = c.centre() - Vec3{0, c.panel_height() / 2, 0};
        b.to = t.centre() + Vec3{0, t.panel_height() / 2, 0};
        b.width = 0.003f;
        b.rgba[0] = float((colour >> 16) & 255) / 255, b.rgba[1] = float((colour >> 8) & 255) / 255;
        b.rgba[2] = float(colour & 255) / 255, b.rgba[3] = 0.6f;
        agent_beams.push_back(b);
      }
    }
    for (auto& n : desk.notes) {
      Panel c;
      c.exe = "note";
      c.yaw = n.yaw;
      c.height = n.height;
      c.distance = n.distance;
      c.width = n.width;
      auto* pic = cards->get(widen("Note from " + n.by), widen(n.text), 0xFFD36F, c.aspect);
      extras.push_back(c);
      extra_pictures.push_back(pic);
    }
    cards->sweep();
  }

  void add_extras(std::vector<PanelDraw>& draws) {
    for (size_t i = 0; i < extras.size(); ++i) {
      PanelDraw d;
      d.panel = &extras[i];
      d.picture = extra_pictures[i];
      d.bare = true;
      draws.push_back(d);
    }
  }

  // GET /look?yaw=&pitch=&fov=&w=&h= : a PNG of the room from where you stand (degrees; defaults: where you
  // look, 70 degrees tall, 1280 x 720).
  AgentReply look(const AgentRequest& req) {
    auto q = [&](const char* k, double fallback) {
      auto it = req.query.find(k);
      return it == req.query.end() || it->second.empty() ? fallback : std::atof(it->second.c_str());
    };
    UINT w = UINT(std::clamp(q("w", 1280), 64.0, 2560.0)), h = UINT(std::clamp(q("h", 720), 64.0, 2560.0));
    Camera c = room.camera;
    c.yaw = float(q("yaw", c.yaw * 180 / kPi) * kPi / 180);
    c.pitch = std::clamp(float(q("pitch", c.pitch * 180 / kPi) * kPi / 180), -1.5f, 1.5f);
    c.fov_y = std::clamp(float(q("fov", 70) * kPi / 180), 0.2f, 2.6f);
    View v = View::of_camera(c, float(w) / float(h));
    if (vr) {
      v.pos = head.position;
      v.floor_y = xr->floor_y();
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    winrt::com_ptr<ID3D11Texture2D> tex;
    winrt::com_ptr<ID3D11RenderTargetView> rtv;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(device->CreateTexture2D(&td, nullptr, tex.put())) ||
        FAILED(device->CreateRenderTargetView(tex.get(), &rd, rtv.put())))
      return {500, "application/json", "{\"error\": \"no picture\"}"};
    std::vector<PanelDraw> draws;
    for (auto& p : room.panels) {
      PanelDraw d;
      d.panel = &p;
      d.picture = picture_of(p);
      draws.push_back(d);
    }
    add_extras(draws);
    renderer.draw_view(v, rtv.get(), w, h, draws, agent_beams);
    std::string png = encode_png(read_pixels(device.get(), tex.get(), w, h), w, h);
    if (png.empty()) return {500, "application/json", "{\"error\": \"couldn't encode the picture\"}"};
    return {200, "image/png", std::move(png)};
  }

  // --- the headset ---

  // The cursor onto (u, v) of a real window, for a controller. A point off every monitor (a panel can show a
  // window that is partly off screen) is brought on by sliding the window. True if the window, and no other,
  // is under the cursor there.
  bool cursor_to(HWND target, float u, float v) {
    if (!IsWindow(target) || IsIconic(target)) return false;
    POINT pt = uv_to_screen(target, u, v);
    if (!MonitorFromPoint(pt, MONITOR_DEFAULTTONULL)) {
      MONITORINFO mi{sizeof mi};
      GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
      POINT c{(mi.rcWork.left + mi.rcWork.right) / 2, (mi.rcWork.top + mi.rcWork.bottom) / 2};
      RECT win;
      GetWindowRect(target, &win);
      SetWindowPos(target, nullptr, win.left + (c.x - pt.x), win.top + (c.y - pt.y), 0, 0,
                   SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER | SWP_ASYNCWINDOWPOS);
      pt = c;
    }
    SetCursorPos(pt.x, pt.y);
    HWND under = WindowFromPoint(pt);
    return under && GetAncestor(under, GA_ROOT) == target;
  }

  void vr_hand(int i, const XrHand& in, float dt) {
    HandState& h = hands[i];
    bool sel_down = in.select && !h.select, sel_up = !in.select && h.select;
    bool sq_down = in.squeeze && !h.squeeze, sq_up = !in.squeeze && h.squeeze;
    bool alt_down = in.alt && !h.alt, alt_up = !in.alt && h.alt;
    h.select = in.select, h.squeeze = in.squeeze, h.alt = in.alt;

    // Buttons let go always reach the window that had them, tracked or not.
    if (sel_up && h.button_down) {
      pass->send_button(WM_LBUTTONUP);
      h.button_down = nullptr;
    }
    if (alt_up && h.right_down) {
      pass->send_button(WM_RBUTTONUP);
      h.right_down = nullptr;
    }
    if ((sq_up || (sel_up && !in.squeeze)) && h.grabbed >= 0) {
      h.grabbed = -1;
      touch();
    }
    if (!in.tracked) {
      h.hit.reset();
      return;
    }
    Ray ray = in.ray();

    // Carrying a panel.
    if (h.grabbed >= 0 && h.grabbed < (int)room.panels.size()) {
      Panel& p = room.panels[h.grabbed];
      p.distance = std::clamp(p.distance * std::pow(2.0f, in.stick_y * dt), 0.4f, 8.0f);
      p.width = std::clamp(p.width * std::pow(2.0f, in.stick_x * dt), 0.15f, 8.0f);
      float yaw, height;
      Room::place_on_ray(ray, p.distance, yaw, height);
      p.yaw = yaw + h.grab_dyaw;
      p.height = height + h.grab_dh;
      h.hit = room.pick(ray);
      touch();
      return;
    }

    h.hit = room.pick(ray);
    auto grab = [&](int panel) {
      Panel& p = room.panels[panel];
      float yaw, height;
      Room::place_on_ray(ray, p.distance, yaw, height);
      h.grabbed = panel;
      h.grab_dyaw = p.yaw - yaw;
      h.grab_dh = p.height - height;
      xr->pulse(i, 0.02f, 0.5f);
    };
    if (h.hit && (sq_down || (sel_down && h.hit->handle))) {
      grab(h.hit->panel);
      return;
    }

    // On a live window: this hand is its mouse.
    if (on_live_picture(h.hit) || h.button_down || h.right_down) {
      HWND target = h.button_down ? h.button_down : h.right_down ? h.right_down : win_of(room.panels[h.hit->panel]);
      float u = h.hit ? h.hit->u : 0.5f, v = h.hit ? h.hit->v : 0.5f;
      bool same = h.hit && !h.hit->handle && win_of(room.panels[h.hit->panel]) == target;
      if (sel_down || alt_down) {
        mouse_hand = i;
        raise_and_focus(target);
      }
      if (i != mouse_hand || !same) return;  // off the window it holds: the cursor stays where it was
      bool ok = cursor_to(target, u, v);
      if (!ok) return;
      if (sel_down) {
        pass->send_button(WM_LBUTTONDOWN);
        h.button_down = target;
        xr->pulse(i, 0.01f, 0.3f);
      }
      if (alt_down) {
        pass->send_button(WM_RBUTTONDOWN);
        h.right_down = target;
        xr->pulse(i, 0.01f, 0.3f);
      }
      h.scroll += in.stick_y * dt * 12.0f * WHEEL_DELTA;
      if (std::fabs(in.stick_y) < 0.15f) h.scroll = 0;
      if (std::fabs(h.scroll) >= WHEEL_DELTA / 4) {
        int step = int(h.scroll);
        pass->send_wheel(step, false);
        h.scroll -= float(step);
      }
    }
  }

  void vr_frame() {
    if (!xr->poll()) {
      logf("the headset session ended");
      running = false;
      return;
    }
    update_windows();
    agents_frame();
    XrFrame f;
    bool live = xr->begin(f);  // waits for the headset's next frame
    double t0 = now_ms();
    if (live) {
      head = f.head;
      Vec3 fwd = rotate(head.orientation, {0, 0, -1});
      room.camera.yaw = std::atan2(fwd.x, -fwd.z);  // so a new panel goes where you look
      room.camera.pitch = std::asin(std::clamp(fwd.y, -1.0f, 1.0f));
      for (int i = 0; i < 2; ++i) vr_hand(i, f.hands[i], f.dt);
    }

    // What each hand points at.
    std::vector<Beam> beams;
    std::vector<DotLayer> dots;
    for (int i = 0; i < 2 && live; ++i) {
      if (!f.hands[i].tracked) continue;
      Ray ray = f.hands[i].ray();
      Beam b;
      b.from = ray.origin;
      auto& hit = hands[i].hit;
      b.to = ray.origin + ray.dir * (hit ? hit->t : 2.0f);
      if (!hit) b.rgba[3] = 0.35f;
      beams.push_back(b);
      if (hit && !hit->handle) {
        const Panel& p = room.panels[hit->panel];
        dots.push_back({b.to - p.forward() * 0.002f, p.yaw});
      }
    }

    // Live windows go to the compositor as quad layers, the nearest first while it has room.
    std::vector<int> order;
    for (int i = 0; i < (int)room.panels.size(); ++i)
      if (picture_of(room.panels[i])) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
      return length(room.panels[a].centre() - head.position) < length(room.panels[b].centre() - head.position);
    });
    size_t budget = xr->max_layers() > 1 + dots.size() ? xr->max_layers() - 1 - dots.size() : 0;
    if (order.size() > budget) order.resize(budget);
    std::vector<PanelLayer> layers;
    std::vector<bool> layered(room.panels.size(), false);
    for (int i : order) {
      const Panel& p = room.panels[i];
      auto& cap = *captures[p.window];
      layers.push_back({p.window, &p, cap.srv(true), cap.width(), cap.height(), fresh.count(p.window) > 0});
      layered[i] = true;
    }

    std::vector<PanelDraw> eyes, mirror;
    for (int i = 0; i < (int)room.panels.size(); ++i) {
      PanelDraw d;
      d.panel = &room.panels[i];
      d.picture = picture_of(room.panels[i]);
      for (auto& h : hands) {
        if (h.hit && h.hit->panel == i) (h.hit->handle ? d.handle_hovered : d.hovered) = true;
        if (h.grabbed == i) d.grabbed = true;
      }
      mirror.push_back(d);
      d.picture_elsewhere = layered[i];
      eyes.push_back(d);
    }
    add_extras(mirror);
    add_extras(eyes);
    beams.insert(beams.end(), agent_beams.begin(), agent_beams.end());
    if (live) xr->end(f, eyes, beams, layers, dots);

    // The mirror: your view, a little wider, on the monitor.
    if (visible && renderer.width()) {
      float aspect = float(renderer.width()) / float(renderer.height());
      Camera level = room.camera;  // where you look, without the head's roll, so the monitor's horizon stays level
      level.fov_y = 2 * std::atan(0.7f);
      View v = View::of_camera(level, aspect);
      v.pos = head.position;
      v.floor_y = xr->floor_y();
      renderer.draw_window(v, mirror, beams);
      renderer.present(0);
    }
    double cpu = now_ms() - t0;
    if (!live) Sleep(10);
    if (dirty && now_ms() - dirty_at > 800) save();

    if (live) {
      ++vr_frames;
      vr_cpu_ms += cpu;
      if (!f.should_render) ++vr_dropped;
    }
    if (now_ms() - vr_stat_at > 5000) {
      double secs = (now_ms() - vr_stat_at) / 1000.0;
      if (vr_stat_at > 0)
      {
        Vec3 hf = rotate(head.orientation, {0, 0, -1}), hu = rotate(head.orientation, {0, 1, 0});
        logf("vr: %.1f frames/s, %.2f ms CPU each, %d not shown, %zu window layers, floor %.2f m; head at "
             "(%.2f %.2f %.2f) looking (%.2f %.2f %.2f) up (%.2f %.2f %.2f)",
             vr_frames / secs, vr_frames ? vr_cpu_ms / vr_frames : 0.0, vr_dropped, layers.size(), xr->floor_y(),
             head.position.x, head.position.y, head.position.z, hf.x, hf.y, hf.z, hu.x, hu.y, hu.z);
      }
      vr_stat_at = now_ms();
      vr_frames = vr_dropped = 0;
      vr_cpu_ms = 0;
    }
  }

  // --- the frame ---
  void frame() {
    if (vr) return vr_frame();
    update_windows();
    agents_frame();
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
      add_extras(draws);
      renderer.draw(room.camera, draws, agent_beams);
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
      if (app && app->vr) break;
      return MA_NOACTIVATE;  // clicking the room never takes the keyboard from your window
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
      if (app && app->vr) {
        if (msg == WM_RBUTTONUP) app->room_menu();
        return 0;
      }
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
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; i < argc; ++i)
    if (std::wstring_view(argv[i]) == L"--vr") app.vr = true;
  LocalFree(argv);

  // The headset first: it says which GPU everything has to be on.
  winrt::com_ptr<IDXGIAdapter> xr_adapter;
  if (app.vr) {
    app.xr = std::make_unique<Xr>();
    std::string err;
    if (!app.xr->init(err)) {
      logf("headset: %s", err.c_str());
      MessageBoxA(nullptr, err.c_str(), "FullImmersion", MB_ICONERROR);
      return 1;
    }
    logf("headset: %s, up to %u layers", app.xr->runtime_name().c_str(), app.xr->max_layers());
    winrt::com_ptr<IDXGIFactory1> factory;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void());
    winrt::com_ptr<IDXGIAdapter1> a;
    for (UINT i = 0; factory && factory->EnumAdapters1(i, a.put()) != DXGI_ERROR_NOT_FOUND; ++i, a = nullptr) {
      DXGI_ADAPTER_DESC1 d;
      a->GetDesc1(&d);
      LUID want = app.xr->adapter();
      if (d.AdapterLuid.LowPart == want.LowPart && d.AdapterLuid.HighPart == want.HighPart) {
        xr_adapter = a.as<IDXGIAdapter>();
        logf("headset GPU: %s", narrow(d.Description).c_str());
        break;
      }
    }
  }

  // The room covers the monitor the cursor is on.
  POINT c;
  GetCursorPos(&c);
  MONITORINFO mi{sizeof mi};
  GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTOPRIMARY), &mi);
  RECT r = mi.rcMonitor;
  if (app.vr) {  // a plain window, the headset's mirror
    LONG w = 1280, h = 720;
    r = {mi.rcWork.left + 40, mi.rcWork.top + 40, mi.rcWork.left + 40 + w, mi.rcWork.top + 40 + h};
    AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
  }

  WNDCLASSEXW wc{sizeof wc};
  wc.lpfnWndProc = wndproc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = L"FullImmersionRoom";
  wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  RegisterClassExW(&wc);
  if (app.vr)
    app.hwnd = CreateWindowExW(0, wc.lpszClassName, L"FullImmersion (headset)", WS_OVERLAPPEDWINDOW, r.left, r.top,
                               r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
  else
    app.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, wc.lpszClassName, L"FullImmersion", WS_POPUP,
                               r.left, r.top, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
  if (!app.hwnd) return 1;

  try {
    app.device = make_device(xr_adapter.get());
    app.renderer.init(app.hwnd, app.device.get());
    if (app.vr) {
      std::string err;
      if (!app.xr->start(app.device.get(), &app.renderer, err)) {
        logf("headset: %s", err.c_str());
        MessageBoxA(nullptr, err.c_str(), "FullImmersion", MB_ICONERROR);
        return 1;
      }
      logf("headset session started");
    }
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
  app.open_door(dir);

  if (!RegisterHotKey(app.hwnd, kHotkeyToggle, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'F'))
    logf("Ctrl+Alt+F is taken by another program");
  if (!RegisterHotKey(app.hwnd, kHotkeyQuit, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q'))
    logf("Ctrl+Alt+Q is taken by another program");
  ShowWindow(app.hwnd, app.vr ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
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
  for (auto& h : app.hands) {  // never leave a real button held down
    if (h.button_down) app.pass->send_button(WM_LBUTTONUP);
    if (h.right_down) app.pass->send_button(WM_RBUTTONUP);
  }
  app.door.close();
  app.pass->close();
  app.save();
  app.xr.reset();
  app.captures.clear();
  logf("--- FullImmersion closed");
  DestroyWindow(app.hwnd);
  if (g_log) fclose(g_log);
  return 0;
}
