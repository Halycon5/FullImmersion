// The room: the panels around you, where you're looking, and what your pointer is on.
// You stand at the origin with your eyes at y = 0; -z is straight ahead; y is up.
// Panels stand upright on a circle round you, facing you. No platform, no GPU: tested on its own.
#pragma once
#include "core/vec.h"
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fi {

struct Panel {
  uint64_t window = 0;  // the real window's handle (HWND) as a number; 0 while not found
  std::string exe;      // e.g. "chrome.exe": how a panel finds its window again next time
  std::string title;
  float yaw = 0;          // radians round you; 0 straight ahead, positive to the right
  float height = 0;       // metres, the panel's middle against your eyes
  float distance = 1.4f;  // metres from you
  float width = 1.0f;     // metres
  float aspect = 9.0f / 16.0f;  // height / width, from the window's size

  Vec3 forward() const;  // from you towards the panel, flat
  Vec3 right() const;
  Vec3 centre() const;
  float panel_height() const { return width * aspect; }

  // The handle under each panel: grab it to move the panel, scroll on it to resize. In metres.
  float handle_width() const { return std::fmin(0.3f, width * 0.4f); }
  static constexpr float kHandleGap = 0.025f, kHandleHeight = 0.022f;
};

struct Hit {
  int panel = -1;  // index into Room::panels
  bool handle = false;  // on the panel's handle rather than its picture (u, v are then 0)
  float u = 0, v = 0;  // 0..1 across and down the panel, as the window's pixels run
  float t = 0;  // distance along the ray
};

struct Camera {
  float yaw = 0;    // radians, positive turns right
  float pitch = 0;  // radians, positive looks up
  float fov_y = 70.0f * kPi / 180.0f;
  Vec3 look() const;
  void basis(Vec3& fwd, Vec3& right, Vec3& up) const;
  // A pixel of a w x h view into the room (pixel centres at +0.5), and back again.
  Ray ray(float px, float py, float w, float h) const;
  bool project(Vec3 p, float w, float h, float& px, float& py) const;  // false if behind you
};

inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

// The point on the room's side of a panel at (u, v): the inverse of Room::pick.
Vec3 point_on(const Panel& p, float u, float v);
Vec3 handle_point(const Panel& p);  // the middle of the panel's handle

struct Room {
  std::vector<Panel> panels;
  Camera camera;

  std::optional<Hit> pick(const Ray& ray) const;  // the nearest panel the ray meets, front side only
  // Where a panel at `distance` from you would go to sit on the ray: its yaw and height. The ray may start
  // anywhere (a hand); it's followed out to that distance round you, flat.
  static void place_on_ray(const Ray& ray, float distance, float& yaw, float& height);
  // A new panel's spot: straight where you look, nudged so it doesn't sit exactly on another.
  void place_new(Panel& p) const;

  std::string to_json() const;
  bool from_json(const std::string& text);  // false (and nothing changed) if it isn't a layout
};

// Finding saved panels' windows again: the open window with the same exe and title, else the same exe.
struct OpenWindow {
  uint64_t window = 0;
  std::string exe, title;
};
// For each panel with window == 0, picks an open window not already taken. Returns how many were found.
int reattach(std::vector<Panel>& panels, const std::vector<OpenWindow>& open);

}  // namespace fi
