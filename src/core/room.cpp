#include "core/room.h"
#include "core/json.h"
#include <algorithm>

namespace fi {

Vec3 Panel::forward() const { return {std::sin(yaw), 0, -std::cos(yaw)}; }
Vec3 Panel::right() const { return {std::cos(yaw), 0, std::sin(yaw)}; }
Vec3 Panel::centre() const { return forward() * distance + Vec3{0, height, 0}; }

Vec3 Camera::look() const {
  float cp = std::cos(pitch);
  return {std::sin(yaw) * cp, std::sin(pitch), -std::cos(yaw) * cp};
}

void Camera::basis(Vec3& f, Vec3& r, Vec3& u) const {
  f = normalize(look());
  r = normalize(cross(f, {0, 1, 0}));
  u = cross(r, f);
}

Ray Camera::ray(float px, float py, float w, float h) const {
  Vec3 f, r, u;
  basis(f, r, u);
  float ty = std::tan(fov_y / 2), tx = ty * w / h;
  float x = (px + 0.5f) / w * 2 - 1, y = 1 - (py + 0.5f) / h * 2;
  return {{0, 0, 0}, normalize(f + r * (x * tx) + u * (y * ty))};
}

bool Camera::project(Vec3 p, float w, float h, float& px, float& py) const {
  Vec3 f, r, u;
  basis(f, r, u);
  float z = dot(p, f);
  if (z <= 1e-4f) return false;
  float ty = std::tan(fov_y / 2), tx = ty * w / h;
  float x = dot(p, r) / (z * tx), y = dot(p, u) / (z * ty);
  px = (x + 1) / 2 * w - 0.5f;
  py = (1 - y) / 2 * h - 0.5f;
  return true;
}

Vec3 point_on(const Panel& p, float u, float v) {
  return p.centre() + p.right() * ((u - 0.5f) * p.width) + Vec3{0, (0.5f - v) * p.panel_height(), 0};
}

Vec3 handle_point(const Panel& p) {
  return p.centre() + Vec3{0, -p.panel_height() / 2 - Panel::kHandleGap - Panel::kHandleHeight / 2, 0};
}

std::optional<Hit> Room::pick(const Ray& ray) const {
  std::optional<Hit> best;
  for (int i = 0; i < (int)panels.size(); ++i) {
    const Panel& p = panels[i];
    Vec3 n = p.forward() * -1.0f;  // the panel faces you
    float denom = dot(ray.dir, n);
    if (denom >= -1e-6f) continue;  // edge on, or from behind
    float t = dot(p.centre() - ray.origin, n) / denom;
    if (t <= 0) continue;
    Vec3 local = ray.origin + ray.dir * t - p.centre();
    float x = dot(local, p.right());
    float u = x / p.width + 0.5f;
    float v = 0.5f - local.y / p.panel_height();
    Hit h{i, false, u, v, t};
    if (u < 0 || u > 1 || v < 0 || v > 1) {
      float top = -p.panel_height() / 2 - Panel::kHandleGap;
      bool on_handle = std::fabs(x) <= p.handle_width() / 2 && local.y <= top && local.y >= top - Panel::kHandleHeight;
      if (!on_handle) continue;
      h = Hit{i, true, 0, 0, t};
    }
    if (!best || t < best->t) best = h;
  }
  return best;
}

void Room::place_on_ray(const Ray& ray, float distance, float& yaw, float& height) {
  float flat = std::sqrt(ray.dir.x * ray.dir.x + ray.dir.z * ray.dir.z);
  yaw = std::atan2(ray.dir.x, -ray.dir.z);
  // Along the ray till it's `distance` out from you, flat; looking near straight up or down, stop at 60 degrees.
  float t = distance / std::max(flat, 0.5f);
  height = ray.origin.y + ray.dir.y * t;
}

void Room::place_new(Panel& p) const {
  Ray r{{0, 0, 0}, normalize(camera.look())};
  place_on_ray(r, p.distance, p.yaw, p.height);
  // Step to the side until it isn't on top of another panel's middle.
  for (int tries = 0; tries < 12; ++tries) {
    bool clash = false;
    for (auto& q : panels)
      if (std::fabs(q.yaw - p.yaw) < 0.08f && std::fabs(q.height - p.height) < 0.1f) clash = true;
    if (!clash) return;
    p.yaw += (p.width * 0.35f) / p.distance;
  }
}

std::string Room::to_json() const {
  using json::number;
  using json::quote;
  std::string o = "{\n  \"version\": 1,\n  \"camera\": {\"yaw\": " + number(camera.yaw) +
                  ", \"pitch\": " + number(camera.pitch) + ", \"fov_y\": " + number(camera.fov_y) +
                  "},\n  \"panels\": [";
  for (size_t i = 0; i < panels.size(); ++i) {
    const Panel& p = panels[i];
    o += i ? ",\n    " : "\n    ";
    o += "{\"exe\": " + quote(p.exe) + ", \"title\": " + quote(p.title) + ", \"yaw\": " + number(p.yaw) +
         ", \"height\": " + number(p.height) + ", \"distance\": " + number(p.distance) +
         ", \"width\": " + number(p.width) + ", \"aspect\": " + number(p.aspect) + "}";
  }
  o += panels.empty() ? "]\n}\n" : "\n  ]\n}\n";
  return o;
}

bool Room::from_json(const std::string& text) {
  json::Value v;
  if (!json::parse(text, v) || v.kind != json::Value::Kind::Object || v.num("version", 0) != 1) return false;
  Room r;
  if (auto c = v.get("camera")) {
    r.camera.yaw = (float)c->num("yaw", 0);
    r.camera.pitch = (float)c->num("pitch", 0);
    r.camera.fov_y = std::clamp((float)c->num("fov_y", r.camera.fov_y), 0.3f, 2.5f);
  }
  if (auto ps = v.get("panels"); ps && ps->kind == json::Value::Kind::Array) {
    for (auto& e : ps->a) {
      Panel p;
      p.exe = e.str("exe");
      p.title = e.str("title");
      p.yaw = (float)e.num("yaw", 0);
      p.height = (float)e.num("height", 0);
      p.distance = std::clamp((float)e.num("distance", p.distance), 0.3f, 20.0f);
      p.width = std::clamp((float)e.num("width", p.width), 0.1f, 20.0f);
      p.aspect = std::clamp((float)e.num("aspect", p.aspect), 0.05f, 20.0f);
      if (!p.exe.empty()) r.panels.push_back(std::move(p));
    }
  }
  *this = std::move(r);
  return true;
}

int reattach(std::vector<Panel>& panels, const std::vector<OpenWindow>& open) {
  std::vector<bool> taken(open.size(), false);
  for (auto& p : panels)
    for (size_t i = 0; i < open.size(); ++i)
      if (p.window == open[i].window) taken[i] = true;
  int found = 0;
  for (int pass = 0; pass < 2; ++pass) {  // exact titles first, then any window of the same program
    for (auto& p : panels) {
      if (p.window) continue;
      for (size_t i = 0; i < open.size(); ++i) {
        if (taken[i] || open[i].exe != p.exe || (pass == 0 && open[i].title != p.title)) continue;
        p.window = open[i].window;
        p.title = open[i].title;
        taken[i] = true;
        ++found;
        break;
      }
    }
  }
  return found;
}

}  // namespace fi
