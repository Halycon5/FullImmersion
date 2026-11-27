// The room drawn with Direct3D 11 into a window: the sky and floor, then the panels, far to near.
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
};

class Renderer {
 public:
  bool init(HWND hwnd, ID3D11Device* device);
  void resize(UINT w, UINT h);
  void draw(const Camera& cam, const std::vector<PanelDraw>& panels);
  void present();
  UINT width() const { return w_; }
  UINT height() const { return h_; }
  Ray ray_through(const Camera& cam, float px, float py) const;  // a pixel of the window into the room

 private:
  void make_targets();
  winrt::com_ptr<ID3D11Device> device_;
  winrt::com_ptr<ID3D11DeviceContext> ctx_;
  winrt::com_ptr<IDXGISwapChain1> swap_;
  winrt::com_ptr<ID3D11RenderTargetView> rtv_;
  winrt::com_ptr<ID3D11VertexShader> vs_full_, vs_panel_;
  winrt::com_ptr<ID3D11PixelShader> ps_world_, ps_panel_;
  winrt::com_ptr<ID3D11Buffer> cb_frame_, cb_panel_;
  winrt::com_ptr<ID3D11SamplerState> sampler_;
  winrt::com_ptr<ID3D11BlendState> blend_;
  winrt::com_ptr<ID3D11RasterizerState> raster_;
  UINT w_ = 0, h_ = 0;
};

}  // namespace fi
