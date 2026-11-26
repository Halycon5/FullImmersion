#include "common/desktop.h"
#include <dwmapi.h>
#include <cwctype>
#include <cstdio>

namespace fi {

void make_dpi_aware() { SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); }

RECT frame_bounds(HWND hwnd) {
  RECT r{};
  if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r))) GetWindowRect(hwnd, &r);
  return r;
}

static std::wstring exe_of(HWND hwnd) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p) return L"?";
  wchar_t buf[MAX_PATH];
  DWORD n = MAX_PATH;
  std::wstring out = L"?";
  if (QueryFullProcessImageNameW(p, 0, buf, &n)) {
    out.assign(buf, n);
    auto slash = out.find_last_of(L"\\/");
    if (slash != std::wstring::npos) out = out.substr(slash + 1);
  }
  CloseHandle(p);
  return out;
}

WindowInfo describe(HWND hwnd) {
  WindowInfo w;
  w.hwnd = hwnd;
  wchar_t title[512];
  int n = GetWindowTextW(hwnd, title, 512);
  w.title.assign(title, n > 0 ? n : 0);
  w.exe = exe_of(hwnd);
  w.frame = frame_bounds(hwnd);
  return w;
}

static bool is_app_window(HWND hwnd) {
  if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return false;
  if (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return false;
  DWORD cloaked = 0;
  DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
  if (cloaked) return false;
  return GetWindowTextLengthW(hwnd) > 0;
}

std::vector<WindowInfo> list_windows() {
  std::vector<WindowInfo> out;
  EnumWindows(
      [](HWND hwnd, LPARAM lp) -> BOOL {
        if (is_app_window(hwnd)) reinterpret_cast<std::vector<WindowInfo>*>(lp)->push_back(describe(hwnd));
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&out));
  return out;
}

static std::wstring lower(std::wstring_view s) {
  std::wstring o(s);
  for (auto& c : o) c = static_cast<wchar_t>(std::towlower(c));
  return o;
}

HWND find_window(std::wstring_view needle) {
  auto n = lower(needle);
  for (auto& w : list_windows())
    if (lower(w.title).find(n) != std::wstring::npos || lower(w.exe).find(n) != std::wstring::npos) return w.hwnd;
  return nullptr;
}

std::string narrow(std::wstring_view s) {
  if (s.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
  std::string o(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), o.data(), n, nullptr, nullptr);
  return o;
}

std::wstring widen(std::string_view s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring o(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), o.data(), n);
  return o;
}

winrt::com_ptr<ID3D11Device> make_device(IDXGIAdapter* adapter) {
  D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  winrt::com_ptr<ID3D11Device> dev;
  winrt::check_hresult(D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                         D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, dev.put(),
                                         nullptr, nullptr));
  if (auto mt = dev.try_as<ID3D10Multithread>()) mt->SetMultithreadProtected(TRUE);
  return dev;
}

std::vector<uint32_t> read_pixels(ID3D11Device* device, ID3D11Texture2D* tex, UINT w, UINT h) {
  D3D11_TEXTURE2D_DESC d{};
  tex->GetDesc(&d);
  d.Width = w;
  d.Height = h;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  d.MiscFlags = 0;
  winrt::com_ptr<ID3D11Texture2D> staging;
  winrt::check_hresult(device->CreateTexture2D(&d, nullptr, staging.put()));
  winrt::com_ptr<ID3D11DeviceContext> ctx;
  device->GetImmediateContext(ctx.put());
  D3D11_BOX box{0, 0, 0, w, h, 1};
  ctx->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, tex, 0, &box);
  D3D11_MAPPED_SUBRESOURCE m{};
  winrt::check_hresult(ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m));
  std::vector<uint32_t> px(size_t(w) * h);
  for (UINT y = 0; y < h; ++y)
    memcpy(&px[size_t(y) * w], static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, size_t(w) * 4);
  ctx->Unmap(staging.get(), 0);
  return px;
}

bool save_bmp(const std::vector<uint32_t>& px, UINT w, UINT h, const std::wstring& path) {
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
  BITMAPFILEHEADER fh{};
  BITMAPINFOHEADER ih{};
  ih.biSize = sizeof ih;
  ih.biWidth = (LONG)w;
  ih.biHeight = -(LONG)h;  // top row first
  ih.biPlanes = 1;
  ih.biBitCount = 32;
  ih.biCompression = BI_RGB;
  ih.biSizeImage = w * h * 4;
  fh.bfType = 0x4D42;
  fh.bfOffBits = sizeof fh + sizeof ih;
  fh.bfSize = fh.bfOffBits + ih.biSizeImage;
  fwrite(&fh, sizeof fh, 1, f);
  fwrite(&ih, sizeof ih, 1, f);
  fwrite(px.data(), 4, px.size(), f);
  fclose(f);
  return true;
}

static double qpc_freq() {
  static double f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return double(q.QuadPart); }();
  return f;
}

double now_ms() {
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return double(c.QuadPart) * 1000.0 / qpc_freq();
}

int64_t now_100ns() {
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return int64_t(double(c.QuadPart) * 1e7 / qpc_freq());
}

}  // namespace fi
