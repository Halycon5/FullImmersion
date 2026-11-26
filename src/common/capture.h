// One real window, captured live through Windows.Graphics.Capture into a GPU texture.
// Polled from the frame loop (the pool is free-threaded, so no callbacks and no threads of our own).
#pragma once
#include "common/desktop.h"
#include <memory>

namespace fi {

struct CaptureStats {
  uint64_t frames = 0;
  double last_latency_ms = 0, sum_latency_ms = 0, max_latency_ms = 0;  // window composed -> copied by us
  double avg_latency_ms() const { return frames ? sum_latency_ms / double(frames) : 0; }
};

class WindowCapture {
 public:
  WindowCapture(ID3D11Device* device, HWND hwnd);
  ~WindowCapture();
  WindowCapture(const WindowCapture&) = delete;
  WindowCapture& operator=(const WindowCapture&) = delete;

  bool poll();  // true when a new frame was copied into texture(). WGC only sends a frame when the window changes.
  bool wait_frame(double timeout_ms);  // polls until a frame arrives or the time runs out

  ID3D11Texture2D* texture() const;               // B8G8R8A8_TYPELESS, width() x height()
  ID3D11ShaderResourceView* srv(bool srgb) const;  // srgb: sampled values come out linear
  UINT width() const;
  UINT height() const;
  HWND hwnd() const;
  const CaptureStats& stats() const;
  bool border_hidden() const;  // Windows 11 let us drop the yellow capture border

 private:
  struct Impl;
  std::unique_ptr<Impl> m;
};

}  // namespace fi
