#include "app/cards.h"
#include <wincodec.h>
#include <algorithm>

namespace fi {

ID3D11ShaderResourceView* Cards::get(const std::wstring& title, const std::wstring& body, uint32_t accent,
                                     float& aspect) {
  std::wstring key = title + L'\x1f' + body + L'\x1f' + std::to_wstring(accent);
  if (auto it = cards_.find(key); it != cards_.end()) {
    it->second.used = true;
    aspect = it->second.aspect;
    return it->second.srv.get();
  }

  const int W = 640, pad = 28, stripe = 10;
  HDC dc = CreateCompatibleDC(nullptr);
  HFONT bold = CreateFontW(-40, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
  HFONT text = CreateFontW(-32, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
  RECT tr{pad + stripe, pad, W - pad, pad}, br{pad + stripe, 0, W - pad, 0};
  SelectObject(dc, bold);
  DrawTextW(dc, title.c_str(), -1, &tr, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
  tr.right = W - pad;
  br.top = br.bottom = tr.bottom + (body.empty() ? 0 : 8);
  if (!body.empty()) {
    SelectObject(dc, text);
    DrawTextW(dc, body.c_str(), -1, &br, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    br.right = W - pad;
  }
  int H = std::clamp<int>(int((body.empty() ? tr.bottom : br.bottom) + pad), 80, 1100);

  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof bi.bmiHeader;
  bi.bmiHeader.biWidth = W;
  bi.bmiHeader.biHeight = -H;  // top row first
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ old = SelectObject(dc, bmp);
  RECT all{0, 0, W, H}, bar{0, 0, stripe, H};
  HBRUSH bg = CreateSolidBrush(RGB(30, 24, 42));
  HBRUSH ac = CreateSolidBrush(RGB((accent >> 16) & 255, (accent >> 8) & 255, accent & 255));
  FillRect(dc, &all, bg);
  FillRect(dc, &bar, ac);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(240, 232, 255));
  SelectObject(dc, bold);
  DrawTextW(dc, title.c_str(), -1, &tr, DT_WORDBREAK | DT_NOPREFIX);
  if (!body.empty()) {
    SetTextColor(dc, RGB(205, 195, 225));
    SelectObject(dc, text);
    DrawTextW(dc, body.c_str(), -1, &br, DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
  }
  GdiFlush();
  auto* px = static_cast<uint32_t*>(bits);
  for (int i = 0; i < W * H; ++i) px[i] |= 0xff000000u;  // GDI leaves alpha at 0

  Card c;
  D3D11_TEXTURE2D_DESC td{};
  td.Width = W;
  td.Height = H;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_IMMUTABLE;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA sd{bits, UINT(W * 4), 0};
  bool made = SUCCEEDED(device_->CreateTexture2D(&td, &sd, c.tex.put()));
  if (made) {
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    vd.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;  // GDI's colours are sRGB
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipLevels = 1;
    made = SUCCEEDED(device_->CreateShaderResourceView(c.tex.get(), &vd, c.srv.put()));
  }
  SelectObject(dc, old);
  DeleteObject(bmp);
  DeleteObject(bg);
  DeleteObject(ac);
  DeleteObject(bold);
  DeleteObject(text);
  DeleteDC(dc);
  if (!made) return nullptr;
  c.aspect = float(H) / float(W);
  aspect = c.aspect;
  auto* srv = c.srv.get();
  cards_[key] = std::move(c);
  return srv;
}

void Cards::sweep() {
  for (auto it = cards_.begin(); it != cards_.end();) {
    if (!it->second.used) it = cards_.erase(it);
    else {
      it->second.used = false;
      ++it;
    }
  }
}

std::string encode_png(const std::vector<uint32_t>& bgra, UINT w, UINT h) {
  winrt::com_ptr<IWICImagingFactory> wic;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()))))
    return {};
  winrt::com_ptr<IStream> stream;
  if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()))) return {};
  winrt::com_ptr<IWICBitmapEncoder> enc;
  wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.put());
  if (!enc || FAILED(enc->Initialize(stream.get(), WICBitmapEncoderNoCache))) return {};
  winrt::com_ptr<IWICBitmapFrameEncode> frame;
  if (FAILED(enc->CreateNewFrame(frame.put(), nullptr)) || FAILED(frame->Initialize(nullptr))) return {};
  frame->SetSize(w, h);
  // The encoder may answer with another format than asked for: write whatever it settled on.
  WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
  frame->SetPixelFormat(&fmt);
  std::vector<BYTE> rows;
  UINT stride = 0;
  if (fmt == GUID_WICPixelFormat24bppBGR) {
    stride = (w * 3 + 3) & ~3u;
    rows.assign(size_t(stride) * h, 0);
    for (UINT y = 0; y < h; ++y)
      for (UINT x = 0; x < w; ++x) {
        uint32_t p = bgra[size_t(y) * w + x];
        BYTE* d = &rows[size_t(y) * stride + x * 3];
        d[0] = BYTE(p), d[1] = BYTE(p >> 8), d[2] = BYTE(p >> 16);
      }
  } else if (fmt == GUID_WICPixelFormat32bppBGRA || fmt == GUID_WICPixelFormat32bppBGR) {
    stride = w * 4;
    rows.resize(size_t(stride) * h);
    for (size_t i = 0; i < bgra.size(); ++i) {
      uint32_t p = bgra[i] | 0xff000000u;  // opaque: the room has no holes
      memcpy(&rows[i * 4], &p, 4);
    }
  } else {
    return {};
  }
  if (FAILED(frame->WritePixels(h, stride, UINT(rows.size()), rows.data())) || FAILED(frame->Commit()) ||
      FAILED(enc->Commit()))
    return {};
  STATSTG st{};
  stream->Stat(&st, STATFLAG_NONAME);
  std::string out(size_t(st.cbSize.QuadPart), '\0');
  LARGE_INTEGER zero{};
  stream->Seek(zero, STREAM_SEEK_SET, nullptr);
  ULONG got = 0;
  stream->Read(out.data(), ULONG(out.size()), &got);
  out.resize(got);
  return out;
}

}  // namespace fi
