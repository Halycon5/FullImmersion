#include "common/capture.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <algorithm>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;
namespace wg3 = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace fi {

struct WindowCapture::Impl {
  HWND hwnd = nullptr;
  winrt::com_ptr<ID3D11Device> device;
  winrt::com_ptr<ID3D11DeviceContext> ctx;
  wg3::IDirect3DDevice rt_device{nullptr};
  wgc::GraphicsCaptureItem item{nullptr};
  wgc::Direct3D11CaptureFramePool pool{nullptr};
  wgc::GraphicsCaptureSession session{nullptr};
  winrt::Windows::Graphics::SizeInt32 pool_size{};
  winrt::com_ptr<ID3D11Texture2D> tex;
  winrt::com_ptr<ID3D11ShaderResourceView> srv_unorm, srv_srgb;
  UINT w = 0, h = 0;
  CaptureStats stats;
  bool border_hidden = false;

  void ensure_texture(UINT nw, UINT nh) {
    if (tex && nw == w && nh == h) return;
    w = nw;
    h = nh;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    tex = nullptr;
    srv_unorm = nullptr;
    srv_srgb = nullptr;
    winrt::check_hresult(device->CreateTexture2D(&d, nullptr, tex.put()));
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    winrt::check_hresult(device->CreateShaderResourceView(tex.get(), &sd, srv_unorm.put()));
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    winrt::check_hresult(device->CreateShaderResourceView(tex.get(), &sd, srv_srgb.put()));
  }
};

WindowCapture::WindowCapture(ID3D11Device* device, HWND hwnd) : m(std::make_unique<Impl>()) {
  m->hwnd = hwnd;
  m->device.copy_from(device);
  device->GetImmediateContext(m->ctx.put());

  auto dxgi = m->device.as<IDXGIDevice>();
  winrt::com_ptr<::IInspectable> insp;
  winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put()));
  m->rt_device = insp.as<wg3::IDirect3DDevice>();

  auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
  winrt::check_hresult(
      interop->CreateForWindow(hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(m->item)));

  m->pool_size = m->item.Size();
  m->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
      m->rt_device, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, m->pool_size);
  m->session = m->pool.CreateCaptureSession(m->item);
  try {
    m->session.IsCursorCaptureEnabled(false);  // the real cursor is drawn by us, where it belongs in 3D
  } catch (...) {
  }
  try {
    m->session.IsBorderRequired(false);  // Windows 11: no yellow frame round the real window
    m->border_hidden = true;
  } catch (...) {
  }
  m->session.StartCapture();
}

WindowCapture::~WindowCapture() {
  try {
    if (m->session) m->session.Close();
    if (m->pool) m->pool.Close();
  } catch (...) {
  }
}

bool WindowCapture::poll() {
  auto frame = m->pool.TryGetNextFrame();
  if (!frame) return false;
  // Drain to the newest frame, so a slow loop never shows the past.
  while (auto newer = m->pool.TryGetNextFrame()) frame = newer;

  auto size = frame.ContentSize();
  if (size.Width <= 0 || size.Height <= 0) return false;  // minimised

  winrt::com_ptr<ID3D11Texture2D> src;
  auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
  winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), src.put_void()));

  UINT cw = (UINT)std::min(size.Width, m->pool_size.Width);
  UINT ch = (UINT)std::min(size.Height, m->pool_size.Height);
  m->ensure_texture(cw, ch);
  D3D11_BOX box{0, 0, 0, cw, ch, 1};
  m->ctx->CopySubresourceRegion(m->tex.get(), 0, 0, 0, 0, src.get(), 0, &box);

  double lat = double(now_100ns() - frame.SystemRelativeTime().count()) / 10000.0;
  m->stats.frames++;
  m->stats.last_latency_ms = lat;
  m->stats.sum_latency_ms += lat;
  m->stats.max_latency_ms = std::max(m->stats.max_latency_ms, lat);

  if (size.Width != m->pool_size.Width || size.Height != m->pool_size.Height) {
    m->pool_size = size;  // the window was resized: next frames come at the new size
    m->pool.Recreate(m->rt_device, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
  }
  return true;
}

bool WindowCapture::wait_frame(double timeout_ms) {
  double end = now_ms() + timeout_ms;
  while (now_ms() < end) {
    if (poll()) return true;
    Sleep(1);
  }
  return false;
}

ID3D11Texture2D* WindowCapture::texture() const { return m->tex.get(); }
ID3D11ShaderResourceView* WindowCapture::srv(bool srgb) const {
  return srgb ? m->srv_srgb.get() : m->srv_unorm.get();
}
UINT WindowCapture::width() const { return m->w; }
UINT WindowCapture::height() const { return m->h; }
HWND WindowCapture::hwnd() const { return m->hwnd; }
const CaptureStats& WindowCapture::stats() const { return m->stats; }
bool WindowCapture::border_hidden() const { return m->border_hidden; }

}  // namespace fi
