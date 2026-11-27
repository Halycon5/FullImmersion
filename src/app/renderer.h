// The room drawn with Direct3D 11: the sky and floor, the panels far to near, and pointer beams.
// The same code draws the flat window (from the camera) and each eye in the headset (from its pose and its
// lopsided field of view).
#pragma once
#include "common/desktop.h"
#include "core/room.h"

namespace fi {

struct PanelDraw {
  const Panel* panel = nullptr;
  ID3D11ShaderResourceView* picture = nullptr;  // sRGB view of the window's capture; null draws a placeholder
  bool hovered = false;
  bool handle_hovered = false;
  bool grabbed = false;
  bool picture_elsewhere = false;  // the headset composites the picture itself (a quad layer): draw the handle only
  bool bare = false;               // no handle: a card, not a window
};

struct Beam {  // a pointer: from a hand to what it points at
  Vec3 from, to;
  float width = 0.004f;
  float rgba[4] = {0.8f, 0.6f, 1.0f, 0.8f};  // premultiplied by the shader
};

struct View {  // where a picture is seen from
  Vec3 pos;
  Vec3 fwd{0, 0, -1}, right{1, 0, 0}, up{0, 1, 0};
  float tan_left = -1, tan_right = 1, tan_down = -1, tan_up = 1;  // the field of view's edges
  float floor_y = -1.2f;                                          // the floor, in the same space

  static View of_camera(const Camera& cam, float aspect);
  static View of_pose(const Pose& pose, float tl, float tr, float td, float tu);
};

class Renderer {
 public:
  bool init(HWND hwnd, ID3D11Device* device);  // hwnd may be null: no window, eyes only
  void resize(UINT w, UINT h);
  // Into the window, from the camera.
  void draw(const Camera& cam, const std::vector<PanelDraw>& panels, const std::vector<Beam>& beams = {});
  // Into the window, from a view (the headset's mirror).
  void draw_window(const View& v, const std::vector<PanelDraw>& panels, const std::vector<Beam>& beams);
  // Into any target (an eye's swapchain image), w x h.
  void draw_view(const View& v, ID3D11RenderTargetView* rtv, UINT w, UINT h, const std::vector<PanelDraw>& panels,
                 const std::vector<Beam>& beams);
  void present(UINT sync_interval = 1);  // 0 in the headset: the monitor must never hold its frames back
  UINT width() const { return w_; }
  UINT height() const { return h_; }
  Ray ray_through(const Camera& cam, float px, float py) const;  // a pixel of the window into the room

  // A small round dot drawn into any target: the pointer's tip, for a quad layer.
  void draw_dot(ID3D11RenderTargetView* rtv, UINT size);
  // A picture copied (and scaled) into any target: a window into its quad layer's swapchain.
  void blit(ID3D11ShaderResourceView* srv, ID3D11RenderTargetView* rtv, UINT w, UINT h);

 private:
  void make_targets();
  winrt::com_ptr<ID3D11Device> device_;
  winrt::com_ptr<ID3D11DeviceContext> ctx_;
  winrt::com_ptr<IDXGISwapChain1> swap_;
  winrt::com_ptr<ID3D11RenderTargetView> rtv_;
  winrt::com_ptr<ID3D11VertexShader> vs_full_, vs_panel_, vs_beam_;
  winrt::com_ptr<ID3D11PixelShader> ps_blit_;
  winrt::com_ptr<ID3D11PixelShader> ps_world_, ps_panel_, ps_beam_, ps_dot_;
  winrt::com_ptr<ID3D11Buffer> cb_frame_, cb_panel_;
  winrt::com_ptr<ID3D11SamplerState> sampler_;
  winrt::com_ptr<ID3D11BlendState> blend_;
  winrt::com_ptr<ID3D11RasterizerState> raster_;
  UINT w_ = 0, h_ = 0;
};

}  // namespace fi
