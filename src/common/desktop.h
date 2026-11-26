// The Windows side shared by every spike: the windows on the desktop, a D3D11 device, pixels read back, the clock.
#pragma once
#include <windows.h>
#include <unknwn.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <winrt/base.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fi {

struct WindowInfo {
  HWND hwnd = nullptr;
  std::wstring title;
  std::wstring exe;  // file name only, e.g. notepad.exe
  RECT frame{};      // DWM's extended frame bounds, in physical pixels: what WGC captures
};

void make_dpi_aware();  // per-monitor v2, so every coordinate here is a physical pixel

std::vector<WindowInfo> list_windows();  // visible, uncloaked, titled top-level app windows
HWND find_window(std::wstring_view needle);  // first whose title or exe contains needle (any case)
RECT frame_bounds(HWND hwnd);
WindowInfo describe(HWND hwnd);

std::string narrow(std::wstring_view s);
std::wstring widen(std::string_view s);

winrt::com_ptr<ID3D11Device> make_device(IDXGIAdapter* adapter = nullptr);

// Reads a B8G8R8A8 texture (any of its typeless family) back to the CPU, top row first.
std::vector<uint32_t> read_pixels(ID3D11Device* device, ID3D11Texture2D* tex, UINT w, UINT h);
bool save_bmp(const std::vector<uint32_t>& px, UINT w, UINT h, const std::wstring& path);

double now_ms();  // QPC, milliseconds
int64_t now_100ns();  // QPC in the units of WGC's SystemRelativeTime

}  // namespace fi
