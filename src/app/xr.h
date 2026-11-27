// The headset, through OpenXR on Direct3D 11.
//
// Each frame: the room (sky, floor, handles, pointer beams) is drawn for each eye into a projection layer, and
// each live window goes to the compositor as its own quad layer, written only when the window changes. The
// compositor samples those at the lens, which keeps text sharp and costs us next to nothing. The pointers'
// tips are tiny quad layers on top.
#pragma once
#include "app/renderer.h"
#include <map>
#include <memory>
#include <string>

namespace fi {

struct XrHand {
  bool tracked = false;
  Pose aim;  // in the room's space; the pointer runs along -z
  bool select = false, squeeze = false, alt = false;  // trigger, grip, A (Index) or menu (simple controllers)
  float stick_x = 0, stick_y = 0;
  Ray ray() const { return {aim.position, normalize(rotate(aim.orientation, {0, 0, -1}))}; }
};

struct XrFrame {
  bool should_render = false;
  Pose head;
  View eyes[2];
  XrHand hands[2];  // left, right
  float dt = 0;     // seconds since the last frame
};

struct PanelLayer {
  uint64_t key = 0;  // the window, so its swapchain is kept between frames
  const Panel* panel = nullptr;
  ID3D11ShaderResourceView* picture = nullptr;  // sRGB view
  UINT width = 0, height = 0;                   // the picture's size in pixels
  bool fresh = false;                           // a new frame since the last one
};

struct DotLayer {
  Vec3 at;
  float yaw = 0;  // the panel it's on
};

class Xr {
 public:
  Xr();
  ~Xr();
  bool init(std::string& err);  // the runtime and the headset
  LUID adapter() const;         // the GPU the runtime wants the device on
  bool start(ID3D11Device* device, Renderer* renderer, std::string& err);
  bool poll();  // events: false once the session has ended and the app should stop
  bool running() const;
  bool begin(XrFrame& frame);  // waits for the headset; false if this frame can't be drawn at all
  void end(const XrFrame& frame, const std::vector<PanelDraw>& room, const std::vector<Beam>& beams,
           const std::vector<PanelLayer>& panels, const std::vector<DotLayer>& dots);
  void pulse(int hand, float seconds, float amplitude);
  std::string runtime_name() const;
  uint32_t max_layers() const;
  float floor_y() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> m;
};

}  // namespace fi
