// The room's core, tested without a window: picking, placing, layouts, finding windows again.
#include "core/json.h"
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

int main() {
  test_pick_straight_ahead();
  test_pick_turned_and_nearest();
  test_pick_handle();
  test_place_on_ray();
  test_place_new_steps_aside();
  test_json_round_trip();
  test_json_rejects();
  test_reattach();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
