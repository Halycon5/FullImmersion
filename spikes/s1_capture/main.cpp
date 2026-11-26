// S0 spike 1: a real window, captured live into a GPU texture.
//   s1_capture --list                  the windows FullImmersion could capture
//   s1_capture <title or exe> [secs]   capture it (default 5 s), save the first and last frames, print the numbers
#include "common/capture.h"
#include <cstdio>

using namespace fi;

int wmain(int argc, wchar_t** argv) {
  make_dpi_aware();
  winrt::init_apartment(winrt::apartment_type::multi_threaded);

  if (argc < 2 || std::wstring_view(argv[1]) == L"--list") {
    for (auto& w : list_windows())
      printf("%p  %5ldx%-5ld  %-22s %s\n", (void*)w.hwnd, w.frame.right - w.frame.left, w.frame.bottom - w.frame.top,
             narrow(w.exe).c_str(), narrow(w.title).c_str());
    return 0;
  }

  HWND hwnd = find_window(argv[1]);
  if (!hwnd) {
    fprintf(stderr, "No window matches \"%s\". Try --list.\n", narrow(argv[1]).c_str());
    return 1;
  }
  double secs = argc > 2 ? _wtof(argv[2]) : 5.0;
  auto info = describe(hwnd);
  printf("Capturing: %s (%s)\n", narrow(info.title).c_str(), narrow(info.exe).c_str());

  try {
    auto device = make_device();
    double t_start = now_ms();
    WindowCapture cap(device.get(), hwnd);
    double t_made = now_ms();
    if (!cap.wait_frame(2000)) {
      fprintf(stderr, "No frame in 2 s (minimised, or a window WGC can't see).\n");
      return 1;
    }
    double t_first = now_ms();
    CreateDirectoryW(L"out", nullptr);
    save_bmp(read_pixels(device.get(), cap.texture(), cap.width(), cap.height()), cap.width(), cap.height(),
             L"out\\s1_first.bmp");

    double end = t_first + secs * 1000.0;
    while (now_ms() < end) {
      cap.poll();
      Sleep(1);
    }
    save_bmp(read_pixels(device.get(), cap.texture(), cap.width(), cap.height()), cap.width(), cap.height(),
             L"out\\s1_last.bmp");

    auto& s = cap.stats();
    printf("size              %u x %u\n", cap.width(), cap.height());
    printf("border hidden     %s\n", cap.border_hidden() ? "yes" : "no (the yellow frame shows)");
    printf("session start     %.1f ms\n", t_made - t_start);
    printf("first frame       %.1f ms after start\n", t_first - t_made);
    printf("frames            %llu in %.1f s (%.1f fps; WGC only sends when the window changes)\n", s.frames, secs,
           double(s.frames) / secs);
    printf("latency           avg %.2f ms, max %.2f ms (the frame's own timestamp -> in our texture; below 0 means it reached us before that time)\n", s.avg_latency_ms(),
           s.max_latency_ms);
    printf("saved             out\\s1_first.bmp, out\\s1_last.bmp\n");
  } catch (winrt::hresult_error const& e) {
    fprintf(stderr, "Capture failed: 0x%08X %s\n", (unsigned)e.code().value, narrow(e.message()).c_str());
    return 1;
  }
  return 0;
}
