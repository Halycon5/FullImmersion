#include "app/renderer.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace fi {

namespace {

const char* kShaders = R"(
cbuffer Frame : register(b0) {
  row_major float4x4 view_proj;
  float4 cam_right;   // xyz, w = tan(fov/2) * aspect
  float4 cam_up;      // xyz, w = tan(fov/2)
  float4 cam_fwd;     // xyz
};
cbuffer PanelCB : register(b1) {
  float4 p_centre;    // xyz
  float4 p_right;     // xyz * half width
  float4 p_up;        // xyz * half height
  float4 p_state;     // x hovered, y has picture, z handle hovered, w grabbed
  float4 p_handle;    // x half width, y top (from centre, down), z height
};

struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VO vs_full(uint id : SV_VertexID) {
  VO o; o.uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o;
}

// The sky and the floor, from each pixel's ray: no geometry.
float4 ps_world(VO i) : SV_Target {
  float2 ndc = i.uv * float2(2, -2) + float2(-1, 1);
  float3 d = normalize(cam_fwd.xyz + cam_right.xyz * ndc.x * cam_right.w + cam_up.xyz * ndc.y * cam_up.w);
  float3 zenith = float3(0.010, 0.006, 0.030), horizon = float3(0.070, 0.035, 0.110);
  float3 col = lerp(horizon, zenith, saturate(d.y * 1.6));
  // The floor, 1.2 m under your eyes, worked out for every pixel and kept only below the horizon
  // (gradients must not sit inside a branch).
  float t = -1.2 / min(d.y, -0.001);
  float2 p = d.xz * t;
  float2 g = abs(frac(p - 0.5) - 0.5) / fwidth(p);
  float grid = 1 - saturate(min(g.x, g.y));
  float fade = saturate(1 - t / 14);
  float3 floor_col = float3(0.018, 0.012, 0.030) + float3(0.55, 0.25, 0.75) * grid * 0.35 * fade;
  float rad = length(p);
  float ring = abs(rad - 1.4);                    // where panels stand by default
  floor_col += float3(0.6, 0.3, 0.9) * 0.10 * saturate(1 - ring / (0.02 + fwidth(rad))) * fade;
  float below = saturate(-d.y * 8);
  col = d.y < 0 ? lerp(horizon * 0.6, floor_col, below) : col;
  return float4(col, 1);
}

// Six vertices: the picture (0..5); then the handle (6..11).
VO vs_panel(uint id : SV_VertexID) {
  static const float2 q[6] = { float2(0,0), float2(1,0), float2(0,1), float2(0,1), float2(1,0), float2(1,1) };
  uint k = id % 6;
  float2 uv = q[k];
  float3 pos;
  if (id < 6) {
    pos = p_centre.xyz + p_right.xyz * (uv.x * 2 - 1) + p_up.xyz * (1 - uv.y * 2);
  } else {
    float3 r = normalize(p_right.xyz), u = normalize(p_up.xyz);
    float y = -length(p_up.xyz) - p_handle.y - uv.y * p_handle.z;
    pos = p_centre.xyz + r * (uv.x * 2 - 1) * p_handle.x + u * y;
    uv = float2(uv.x + 2, uv.y);  // tells the pixel shader this is the handle
  }
  VO o; o.pos = mul(view_proj, float4(pos, 1)); o.uv = uv; return o;
}

Texture2D tex : register(t0); SamplerState smp : register(s0);

float4 ps_panel(VO i) : SV_Target {
  if (i.uv.x >= 2) {
    float2 h = float2(i.uv.x - 2, i.uv.y);
    float edge = min(min(h.x, 1 - h.x) * p_handle.x * 2, min(h.y, 1 - h.y) * p_handle.z) / max(p_handle.z * 0.5, 1e-4);
    float a = saturate(edge * 4) * (p_state.z > 0 || p_state.w > 0 ? 0.95 : 0.55);
    return float4(float3(0.85, 0.75, 1.0) * a, a);
  }
  float3 col = p_state.y > 0 ? tex.Sample(smp, i.uv).rgb : float3(0.06, 0.05, 0.08);
  // A soft glow round the edge when your pointer is on it.
  float2 e = min(i.uv, 1 - i.uv);
  float border = 1 - saturate(min(e.x, e.y) * 220);
  col = lerp(col, float3(0.75, 0.55, 1.0), border * (p_state.x > 0 ? 0.9 : 0.25));
  return float4(col, 1);
}
)";

struct FrameCB {
  float view_proj[16];
  float right[4], up[4], fwd[4];
};
struct PanelCB {
  float centre[4], right[4], up[4], state[4], handle[4];
};

winrt::com_ptr<ID3DBlob> compile(const char* entry, const char* target) {
  winrt::com_ptr<ID3DBlob> code, err;
  if (FAILED(D3DCompile(kShaders, strlen(kShaders), "room", nullptr, nullptr, entry, target,
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), err.put())))
    throw std::runtime_error(err ? (const char*)err->GetBufferPointer() : "shader compile failed");
  return code;
}

Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

void basis(const Camera& cam, Vec3& f, Vec3& r, Vec3& u) {
  f = normalize(cam.look());
  r = normalize(cross(f, {0, 1, 0}));
  u = cross(r, f);
}

}  // namespace

bool Renderer::init(HWND hwnd, ID3D11Device* device) {
  device_.copy_from(device);
  device->GetImmediateContext(ctx_.put());
  RECT rc;
  GetClientRect(hwnd, &rc);
  w_ = std::max<UINT>(1, rc.right - rc.left);
  h_ = std::max<UINT>(1, rc.bottom - rc.top);

  auto dxgi = device_.as<IDXGIDevice>();
  winrt::com_ptr<IDXGIAdapter> adapter;
  dxgi->GetAdapter(adapter.put());
  winrt::com_ptr<IDXGIFactory2> factory;
  adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void());
  DXGI_SWAP_CHAIN_DESC1 sd{};
  sd.Width = w_;
  sd.Height = h_;
  sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 2;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  winrt::check_hresult(factory->CreateSwapChainForHwnd(device_.get(), hwnd, &sd, nullptr, nullptr, swap_.put()));
  factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
  make_targets();

  auto vf = compile("vs_full", "vs_5_0"), vp = compile("vs_panel", "vs_5_0");
  auto pw = compile("ps_world", "ps_5_0"), pp = compile("ps_panel", "ps_5_0");
  winrt::check_hresult(device_->CreateVertexShader(vf->GetBufferPointer(), vf->GetBufferSize(), nullptr, vs_full_.put()));
  winrt::check_hresult(device_->CreateVertexShader(vp->GetBufferPointer(), vp->GetBufferSize(), nullptr, vs_panel_.put()));
  winrt::check_hresult(device_->CreatePixelShader(pw->GetBufferPointer(), pw->GetBufferSize(), nullptr, ps_world_.put()));
  winrt::check_hresult(device_->CreatePixelShader(pp->GetBufferPointer(), pp->GetBufferSize(), nullptr, ps_panel_.put()));

  D3D11_BUFFER_DESC bd{};
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  bd.ByteWidth = sizeof(FrameCB);
  winrt::check_hresult(device_->CreateBuffer(&bd, nullptr, cb_frame_.put()));
  bd.ByteWidth = sizeof(PanelCB);
  winrt::check_hresult(device_->CreateBuffer(&bd, nullptr, cb_panel_.put()));

  D3D11_SAMPLER_DESC smd{};
  smd.Filter = D3D11_FILTER_ANISOTROPIC;
  smd.MaxAnisotropy = 8;
  smd.AddressU = smd.AddressV = smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  smd.MaxLOD = D3D11_FLOAT32_MAX;
  winrt::check_hresult(device_->CreateSamplerState(&smd, sampler_.put()));

  D3D11_BLEND_DESC bl{};
  bl.RenderTarget[0].BlendEnable = TRUE;
  bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;  // premultiplied
  bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  winrt::check_hresult(device_->CreateBlendState(&bl, blend_.put()));

  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  winrt::check_hresult(device_->CreateRasterizerState(&rd, raster_.put()));
  return true;
}

void Renderer::make_targets() {
  rtv_ = nullptr;
  winrt::com_ptr<ID3D11Texture2D> back;
  winrt::check_hresult(swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), back.put_void()));
  D3D11_RENDER_TARGET_VIEW_DESC rd{};
  rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;  // shaders work in linear light
  rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
  winrt::check_hresult(device_->CreateRenderTargetView(back.get(), &rd, rtv_.put()));
}

void Renderer::resize(UINT w, UINT h) {
  if (!swap_ || w == 0 || h == 0 || (w == w_ && h == h_)) return;
  w_ = w;
  h_ = h;
  ctx_->OMSetRenderTargets(0, nullptr, nullptr);
  rtv_ = nullptr;
  winrt::check_hresult(swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0));
  make_targets();
}

Ray Renderer::ray_through(const Camera& cam, float px, float py) const {
  Vec3 f, r, u;
  basis(cam, f, r, u);
  float ty = std::tan(cam.fov_y / 2), tx = ty * float(w_) / float(h_);
  float x = (px + 0.5f) / float(w_) * 2 - 1, y = 1 - (py + 0.5f) / float(h_) * 2;
  return {{0, 0, 0}, normalize(f + r * (x * tx) + u * (y * ty))};
}

void Renderer::draw(const Camera& cam, const std::vector<PanelDraw>& panels) {
  Vec3 f, r, u;
  basis(cam, f, r, u);
  float ty = std::tan(cam.fov_y / 2), tx = ty * float(w_) / float(h_);
  const float zn = 0.05f, zf = 100.0f;
  // view (rows r, u, -f; the eye at the origin), then a right-handed perspective with depth 0..1
  float view[16] = {r.x, r.y, r.z, 0, u.x, u.y, u.z, 0, -f.x, -f.y, -f.z, 0, 0, 0, 0, 1};
  float proj[16] = {1 / tx, 0, 0, 0, 0, 1 / ty, 0, 0, 0, 0, zf / (zn - zf), zn * zf / (zn - zf), 0, 0, -1, 0};
  FrameCB fc{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k) fc.view_proj[i * 4 + j] += proj[i * 4 + k] * view[k * 4 + j];
  fc.right[0] = r.x, fc.right[1] = r.y, fc.right[2] = r.z, fc.right[3] = tx;
  fc.up[0] = u.x, fc.up[1] = u.y, fc.up[2] = u.z, fc.up[3] = ty;
  fc.fwd[0] = f.x, fc.fwd[1] = f.y, fc.fwd[2] = f.z;
  D3D11_MAPPED_SUBRESOURCE m;
  ctx_->Map(cb_frame_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
  memcpy(m.pData, &fc, sizeof fc);
  ctx_->Unmap(cb_frame_.get(), 0);

  ID3D11RenderTargetView* rtv = rtv_.get();
  ctx_->OMSetRenderTargets(1, &rtv, nullptr);
  D3D11_VIEWPORT vp{0, 0, float(w_), float(h_), 0, 1};
  ctx_->RSSetViewports(1, &vp);
  ctx_->RSSetState(raster_.get());
  ctx_->IASetInputLayout(nullptr);
  ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D11Buffer* cbs[2] = {cb_frame_.get(), cb_panel_.get()};
  ctx_->VSSetConstantBuffers(0, 2, cbs);
  ctx_->PSSetConstantBuffers(0, 2, cbs);
  ID3D11SamplerState* smp = sampler_.get();
  ctx_->PSSetSamplers(0, 1, &smp);

  ctx_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
  ctx_->VSSetShader(vs_full_.get(), nullptr, 0);
  ctx_->PSSetShader(ps_world_.get(), nullptr, 0);
  ctx_->Draw(3, 0);

  // Panels far to near, so nearer ones cover farther ones without a depth buffer.
  std::vector<const PanelDraw*> order;
  for (auto& p : panels) order.push_back(&p);
  std::sort(order.begin(), order.end(), [](auto a, auto b) { return length(a->panel->centre()) > length(b->panel->centre()); });

  ctx_->OMSetBlendState(blend_.get(), nullptr, 0xffffffff);
  ctx_->VSSetShader(vs_panel_.get(), nullptr, 0);
  ctx_->PSSetShader(ps_panel_.get(), nullptr, 0);
  for (auto* d : order) {
    const Panel& p = *d->panel;
    Vec3 c = p.centre(), pr = p.right() * (p.width / 2);
    float hh = p.panel_height() / 2;
    PanelCB pc{{c.x, c.y, c.z, 0},
               {pr.x, pr.y, pr.z, 0},
               {0, hh, 0, 0},
               {d->hovered ? 1.f : 0.f, d->picture ? 1.f : 0.f, d->handle_hovered ? 1.f : 0.f, d->grabbed ? 1.f : 0.f},
               {p.handle_width() / 2, Panel::kHandleGap, Panel::kHandleHeight, 0}};
    ctx_->Map(cb_panel_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    memcpy(m.pData, &pc, sizeof pc);
    ctx_->Unmap(cb_panel_.get(), 0);
    ID3D11ShaderResourceView* srv = d->picture;
    ctx_->PSSetShaderResources(0, 1, &srv);
    ctx_->Draw(12, 0);
  }
  ID3D11ShaderResourceView* none = nullptr;
  ctx_->PSSetShaderResources(0, 1, &none);
}

void Renderer::present() { swap_->Present(1, 0); }

}  // namespace fi
