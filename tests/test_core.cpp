// The room's core, tested without a window: picking, placing, layouts, finding windows again.
#include "core/json.h"
#include "core/agents.h"
#include "core/room.h"
#include <cstdio>
#include <cmath>

using namespace fi;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    ++g_checks;                                                         \
    if (!(cond)) {                                                      \
      ++g_failed;                                                       \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
    }                                                                   \
  } while (0)
static bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }

static void test_pick_straight_ahead() {
  Room room;
  Panel p;
  p.width = 1.6f;
  p.aspect = 0.5f;  // 1.6 x 0.8 m, 1.4 m ahead
  room.panels.push_back(p);
  auto h = room.pick({{0, 0, 0}, {0, 0, -1}});
  CHECK(h && h->panel == 0 && near(h->u, 0.5f) && near(h->v, 0.5f) && near(h->t, 1.4f));
  // The top left corner, aimed at from the eye.
  auto c = room.pick({{0, 0, 0}, normalize({-0.79f, 0.39f, -1.4f})});
  CHECK(c && c->u < 0.02f && c->v < 0.02f);
  // Just outside to the right misses.
  CHECK(!room.pick({{0, 0, 0}, normalize({0.82f, 0, -1.4f})}));
  // Behind you misses.
  CHECK(!room.pick({{0, 0, 0}, {0, 0, 1}}));
}

static void test_pick_turned_and_nearest() {
  Room room;
  Panel right;
  right.yaw = kPi / 2;  // to your right, at +x
  right.distance = 2;
  room.panels.push_back(right);
  Panel front_near, front_far;
  front_near.distance = 1;
  front_far.distance = 3;
  front_far.width = 4;
  room.panels.push_back(front_far);
  room.panels.push_back(front_near);
  auto h = room.pick({{0, 0, 0}, {1, 0, 0}});
  CHECK(h && h->panel == 0 && near(h->u, 0.5f) && near(h->t, 2));
  // u grows to the right as you look at it: a little forward of +x is the panel's left side.
  auto l = room.pick({{0, 0, 0}, normalize({1, 0, -0.1f})});
  CHECK(l && l->panel == 0 && l->u < 0.5f);
  auto f = room.pick({{0, 0, 0}, {0, 0, -1}});
  CHECK(f && f->panel == 2);
}

static void test_pick_handle() {
  Room room;
  Panel p;  // 1 m wide, 0.5625 m tall, 1.4 m ahead
  room.panels.push_back(p);
  float y = -p.panel_height() / 2 - Panel::kHandleGap - Panel::kHandleHeight / 2;
  auto h = room.pick({{0, 0, 0}, normalize({0, y, -1.4f})});
  CHECK(h && h->panel == 0 && h->handle);
  auto picture = room.pick({{0, 0, 0}, normalize({0, 0.1f, -1.4f})});
  CHECK(picture && !picture->handle);
  // Beside the handle, below the panel: nothing.
  CHECK(!room.pick({{0, 0, 0}, normalize({0.4f, y, -1.4f})}));
}

static void test_camera_round_trip() {
  Camera cam;
  cam.yaw = 0.4f;
  cam.pitch = -0.2f;
  float px, py;
  Ray r = cam.ray(300, 200, 1920, 1080);
  CHECK(cam.project(r.dir * 3.0f, 1920, 1080, px, py) && near(px, 300, 0.01f) && near(py, 200, 0.01f));
  CHECK(!cam.project(cam.look() * -1.0f, 1920, 1080, px, py));
  // A pixel aimed at a panel's point picks that point back.
  Room room;
  Panel p;
  p.yaw = 0.3f;
  room.panels.push_back(p);
  room.camera = cam;
  CHECK(cam.project(point_on(p, 0.25f, 0.75f), 1920, 1080, px, py));
  auto h = room.pick(cam.ray(px, py, 1920, 1080));
  CHECK(h && !h->handle && near(h->u, 0.25f, 0.01f) && near(h->v, 0.75f, 0.01f));
  CHECK(cam.project(handle_point(p), 1920, 1080, px, py));
  auto hh = room.pick(cam.ray(px, py, 1920, 1080));
  CHECK(hh && hh->handle);
}

static void test_place_on_ray() {
  float yaw, height;
  Room::place_on_ray({{0, 0, 0}, normalize({1, 0, -1})}, 2, yaw, height);
  CHECK(near(yaw, kPi / 4) && near(height, 0));
  Room::place_on_ray({{0, 0, 0}, normalize({0, 1, -2})}, 2, yaw, height);
  CHECK(near(yaw, 0) && near(height, 1));
  Panel p;
  p.yaw = 0.3f;
  p.height = -0.2f;
  Room room;
  room.panels.push_back(p);
  // A panel put back on the ray through its own middle lands where it was.
  Room::place_on_ray({{0, 0, 0}, normalize(p.centre())}, p.distance, yaw, height);
  CHECK(near(yaw, 0.3f) && near(height, -0.2f));
}

static void test_place_from_a_hand() {
  // A hand 0.3 m right and 0.4 m down, pointing straight ahead: the panel lands ahead of the hand.
  float yaw, height;
  Room::place_on_ray({{0.3f, -0.4f, 0}, {0, 0, -1}}, 2, yaw, height);
  Vec3 at{std::sin(yaw) * 2, height, -std::cos(yaw) * 2};
  CHECK(near(at.x, 0.3f) && near(at.y, -0.4f) && at.z < 0);
  // A hand ray rotated by a quaternion, checked against the plain direction.
  Quat q = quat_axis_angle({0, 1, 0}, -kPi / 2);  // turned right
  Vec3 d = rotate(q, {0, 0, -1});
  CHECK(near(d.x, 1) && near(d.z, 0));
}

static void test_place_new_steps_aside() {
  Room room;
  Panel a;
  room.place_new(a);
  room.panels.push_back(a);
  Panel b;
  room.place_new(b);
  CHECK(near(a.yaw, 0) && b.yaw > 0.1f);
}

static void test_json_round_trip() {
  Room room;
  room.camera.yaw = 0.25f;
  room.camera.pitch = -0.1f;
  Panel p;
  p.exe = "chrome.exe";
  p.title = "Tab \"one\" \\ ü";
  p.yaw = -0.5f;
  p.height = 0.3f;
  p.width = 1.25f;
  p.aspect = 0.5625f;
  p.window = 1234;  // never saved: handles don't outlive a session
  room.panels.push_back(p);
  Room back;
  CHECK(back.from_json(room.to_json()));
  CHECK(back.panels.size() == 1);
  if (!back.panels.empty()) {
    auto& q = back.panels[0];
    CHECK(q.exe == "chrome.exe" && q.title == p.title && q.window == 0);
    CHECK(near(q.yaw, -0.5f) && near(q.height, 0.3f) && near(q.width, 1.25f) && near(q.aspect, 0.5625f));
  }
  CHECK(near(back.camera.yaw, 0.25f) && near(back.camera.pitch, -0.1f));
  Room empty;
  CHECK(back.from_json(empty.to_json()) && back.panels.empty());
}

static void test_json_rejects() {
  Room room;
  room.panels.push_back(Panel{});
  CHECK(!room.from_json("{"));
  CHECK(!room.from_json("{\"version\": 2}"));
  CHECK(!room.from_json("[1,2]"));
  CHECK(!room.from_json("{\"version\": 1} trailing"));
  CHECK(room.panels.size() == 1);  // untouched by a bad layout
  json::Value v;
  CHECK(json::parse("{\"a\": [1, -2.5e2, true, null, \"\\u00e9\\ud83e\\udd8a\"]}", v));
  CHECK(v.get("a") && v.get("a")->a.size() == 5 && v.get("a")->a[1].n == -250);
  CHECK(v.get("a")->a[4].s == "\xC3\xA9\xF0\x9F\xA6\x8A");
}

static void test_reattach() {
  std::vector<Panel> panels(3);
  panels[0].exe = "notepad.exe";
  panels[0].title = "b.txt - Notepad";
  panels[1].exe = "notepad.exe";
  panels[1].title = "gone.txt - Notepad";
  panels[2].exe = "discord.exe";
  panels[2].title = "Discord";
  std::vector<OpenWindow> open = {{10, "notepad.exe", "a.txt - Notepad"},
                                  {11, "notepad.exe", "b.txt - Notepad"},
                                  {12, "chrome.exe", "x"}};
  int n = reattach(panels, open);
  CHECK(n == 2);
  CHECK(panels[0].window == 11);  // the exact title wins over the first of its program
  CHECK(panels[1].window == 10 && panels[1].title == "a.txt - Notepad");
  CHECK(panels[2].window == 0);
  CHECK(reattach(panels, open) == 0);  // nothing found twice
}

static AgentRequest post(const char* path, const char* body, const char* who = "Ada") {
  AgentRequest r;
  r.method = "POST";
  r.path = path;
  r.body = body;
  r.agent = who;
  return r;
}

static void test_agents_arrange_a_workspace() {
  Room room;
  std::vector<OpenWindow> open = {{10, "chrome.exe", "Docs - Chrome"}, {11, "Code.exe", "main.cpp - VS Code"},
                                  {12, "WindowsTerminal.exe", "Terminal"}};
  AgentDesk desk(room);
  desk.open_windows = [&] { return open; };
  desk.pin = [&](const OpenWindow& w) {
    Panel p;
    p.window = w.window, p.exe = w.exe, p.title = w.title;
    room.place_new(p);
    room.panels.push_back(p);
    return int(room.panels.size()) - 1;
  };
  CHECK(desk.handle(post("/pin", R"({"match": "vs code", "yaw": 0})"), 0).status == 200);
  CHECK(desk.handle(post("/pin", R"({"match": "terminal.exe"})"), 0).status == 200);  // by program
  CHECK(desk.handle(post("/pin", R"({"window": 10, "width": 0.8})"), 0).status == 200);
  CHECK(desk.handle(post("/pin", R"({"window": 10})"), 0).status == 409);  // already pinned
  CHECK(desk.handle(post("/pin", R"({"match": "nothing like it"})"), 0).status == 404);
  CHECK(room.panels.size() == 3 && room.panels[2].width == 0.8f);

  // Docs left, code in the middle, the terminal right, round straight ahead.
  auto r = desk.handle(post("/arrange", R"({"panels": ["docs", "main.cpp", "terminal"], "around": 0, "distance": 1.5})"), 0);
  CHECK(r.status == 200);
  CHECK(room.panels[2].yaw < room.panels[0].yaw && room.panels[0].yaw < room.panels[1].yaw);
  CHECK(std::fabs(room.panels[0].yaw) < 0.2f);  // the middle one is near straight ahead
  CHECK(room.panels[1].distance == 1.5f);
  // Neighbours don't overlap: the gap between edges along the circle is about the gap asked for.
  float edge_gap = (room.panels[0].yaw - room.panels[2].yaw) * 1.5f - (room.panels[0].width + room.panels[2].width) / 2;
  CHECK(std::fabs(edge_gap - 0.06f) < 0.001f);

  CHECK(desk.handle(post("/move", R"({"panel": "terminal", "yaw": 90, "height": 0.2})"), 0).status == 200);
  CHECK(std::fabs(room.panels[1].yaw - kPi / 2) < 1e-4f && room.panels[1].height == 0.2f);
  CHECK(desk.handle(post("/move", R"({"panel": 7})"), 0).status == 404);
  CHECK(desk.changed);
}

static void test_agents_presence_notes_and_refusals() {
  Room room;
  Panel p;
  p.exe = "chrome.exe", p.title = "Docs";
  room.panels.push_back(p);
  AgentDesk desk(room);
  CHECK(desk.handle(post("/presence", R"({"state": "working", "says": "on it", "looking_at": "docs"})"), 1000).status == 200);
  CHECK(desk.handle(post("/presence", R"({"state": "waiting"})", "Bea"), 1000).status == 200);
  CHECK(desk.agents.size() == 2 && desk.agents[0].looking_at == 0 && desk.agents[0].says == "on it");
  CHECK(desk.agents[0].yaw != desk.agents[1].yaw);  // they don't stand on each other
  desk.tick(1000 + AgentDesk::kSaysMs + 1);
  CHECK(desk.agents[0].says.empty());  // what was said fades
  desk.tick(1000 + AgentDesk::kAgentTimeoutMs + 1);
  CHECK(desk.agents.empty());  // and agents not heard from leave

  auto n = desk.handle(post("/note", R"({"text": "read this first", "near": 0})"), 0);
  CHECK(n.status == 200 && desk.notes.size() == 1 && desk.notes[0].yaw > room.panels[0].yaw);
  CHECK(desk.handle(post("/unnote", R"({"id": 1})"), 0).status == 200 && desk.notes.empty());

  CHECK(desk.handle(post("/click", R"({"panel": 0, "u": 0.5, "v": 0.5})"), 0).status == 403);
  CHECK(desk.handle(post("/type", R"({"text": "hi"})"), 0).status == 403);
  CHECK(desk.handle(post("/move", "not json"), 0).status == 400);

  AgentRequest get;
  get.method = "GET";
  get.path = "/scene";
  auto s = desk.handle(get, 0);
  json::Value v;
  CHECK(s.status == 200 && json::parse(s.body, v) && v.get("panels") && v.get("panels")->a.size() == 1);
}

int main() {
  test_pick_straight_ahead();
  test_pick_turned_and_nearest();
  test_pick_handle();
  test_camera_round_trip();
  test_place_on_ray();
  test_place_from_a_hand();
  test_place_new_steps_aside();
  test_json_round_trip();
  test_json_rejects();
  test_reattach();
  test_agents_arrange_a_workspace();
  test_agents_presence_notes_and_refusals();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
