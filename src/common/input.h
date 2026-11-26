// Pointing and typing into real windows: a point on a panel (u, v in 0..1) becomes a click on the real window.
// Every action checks first that it would land on the window meant, and refuses rather than hit another one.
#pragma once
#include "common/desktop.h"
#include <string>

namespace fi {

struct InputResult {
  bool ok = false;
  std::string why;  // when not ok: what stopped it
};

POINT uv_to_screen(HWND hwnd, float u, float v);  // over DWM's frame bounds, as WGC captures it
InputResult raise_and_focus(HWND hwnd);  // restore, bring to front, take the keyboard
InputResult click(HWND hwnd, float u, float v, bool restore_cursor = true);
InputResult type_text(HWND hwnd, std::wstring_view text);  // \n is Enter; anything else is sent as Unicode

}  // namespace fi
