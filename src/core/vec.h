// Small vector maths for the room. No platform, no GPU.
#pragma once
#include <cmath>

namespace fi {

struct Vec3 {
  float x = 0, y = 0, z = 0;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalize(Vec3 a) {
  float l = length(a);
  return l > 0 ? a * (1.0f / l) : a;
}

struct Ray {
  Vec3 origin;
  Vec3 dir;  // normalised
};

constexpr float kPi = 3.14159265358979f;

struct Quat {
  float x = 0, y = 0, z = 0, w = 1;
};
inline Vec3 rotate(Quat q, Vec3 v) {  // q v q*
  Vec3 u{q.x, q.y, q.z};
  Vec3 t{2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x)};
  Vec3 c{u.y * t.z - u.z * t.y, u.z * t.x - u.x * t.z, u.x * t.y - u.y * t.x};
  return v + t * q.w + c;
}
inline Quat quat_axis_angle(Vec3 axis, float angle) {
  Vec3 a = normalize(axis) * std::sin(angle / 2);
  return {a.x, a.y, a.z, std::cos(angle / 2)};
}

struct Pose {
  Quat orientation;
  Vec3 position;
};

}  // namespace fi
