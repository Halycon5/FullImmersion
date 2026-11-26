// S0 spike 3: pointing and typing into a real window, the way a hand on a panel will.
//   s3_input <title or exe> [--click u v] [--type "text"] [--wait ms] ...   (steps run in order)
// It captures the window before and after, and says how much of it changed: proof the input landed.
#include "common/capture.h"
#include "common/input.h"
#include <cstdio>

using namespace fi;

static double changed_fraction(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
  if (a.size() != b.size() || a.empty()) return 1.0;
  size_t n = 0;
  for (size_t i = 0; i < a.size(); ++i) n += (a[i] & 0xFFFFFF) != (b[i] & 0xFFFFFF);
  return double(n) / double(a.size());
}

int wmain(int argc, wchar_t** argv) {
  make_dpi_aware();
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  if (argc < 3) {
    fprintf(stderr, "usage: s3_input <title or exe> [--click u v] [--type text] [--wait ms] ...\n");
    return 2;
  }
  HWND hwnd = find_window(argv[1]);
  if (!hwnd) {
    fprintf(stderr, "No window matches \"%s\".\n", narrow(argv[1]).c_str());
    return 1;
  }
  auto info = describe(hwnd);
  printf("Window: %s (%s)\n", narrow(info.title).c_str(), narrow(info.exe).c_str());

  try {
    auto device = make_device();
    WindowCapture cap(device.get(), hwnd);
    if (!cap.wait_frame(2000)) {
      fprintf(stderr, "No frame from the window.\n");
      return 1;
    }
    auto before = read_pixels(device.get(), cap.texture(), cap.width(), cap.height());
    UINT bw = cap.width(), bh = cap.height();
    CreateDirectoryW(L"out", nullptr);
    save_bmp(before, bw, bh, L"out\\s3_before.bmp");

    int failures = 0;
    for (int i = 2; i < argc; ++i) {
      std::wstring_view a = argv[i];
      InputResult r{true, {}};
      double t0 = now_ms();
      if (a == L"--click" && i + 2 < argc) {
        float u = (float)_wtof(argv[i + 1]), v = (float)_wtof(argv[i + 2]);
        i += 2;
        r = click(hwnd, u, v);
        printf("click %.3f %.3f   %s (%.1f ms)\n", u, v, r.ok ? "ok" : r.why.c_str(), now_ms() - t0);
      } else if (a == L"--type" && i + 1 < argc) {
        std::wstring text = argv[++i];
        for (size_t p; (p = text.find(L"\\n")) != std::wstring::npos;) text.replace(p, 2, L"\n");
        r = type_text(hwnd, text);
        printf("type %zu chars     %s (%.1f ms)\n", text.size(), r.ok ? "ok" : r.why.c_str(), now_ms() - t0);
      } else if (a == L"--wait" && i + 1 < argc) {
        Sleep((DWORD)_wtoi(argv[++i]));
      } else {
        fprintf(stderr, "unknown step: %s\n", narrow(a).c_str());
        return 2;
      }
      if (!r.ok) ++failures;
    }

    // Let the app draw, then take the newest frame.
    double end = now_ms() + 600;
    while (now_ms() < end) {
      cap.poll();
      Sleep(5);
    }
    auto after = read_pixels(device.get(), cap.texture(), cap.width(), cap.height());
    save_bmp(after, cap.width(), cap.height(), L"out\\s3_after.bmp");
    bool same_size = cap.width() == bw && cap.height() == bh;
    printf("changed           %.2f%% of the window%s\n", 100.0 * changed_fraction(before, after),
           same_size ? "" : " (it was resized)");
    printf("saved             out\\s3_before.bmp, out\\s3_after.bmp\n");
    printf("focus now         %s\n", GetForegroundWindow() == hwnd ? "the window" : "elsewhere");
    return failures ? 1 : 0;
  } catch (winrt::hresult_error const& e) {
    fprintf(stderr, "Failed: 0x%08X %s\n", (unsigned)e.code().value, narrow(e.message()).c_str());
    return 1;
  }
}
