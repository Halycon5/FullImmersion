// Your real mouse, straight into a pinned window.
//
// The room is a topmost window over the monitor. While the pointer is on a panel, the real window is slid
// (behind the room) so that the point you're on sits exactly under the cursor, and the room keeps a one-pixel
// hole at the cursor's tip. Clicks, scrolls and hovers then land on the real window natively: Windows does
// the hit-testing, focus, capture and context menus, and your keyboard goes to it after a click. Events that
// reach the room instead (the cursor moved off the hole between frames) are sent on with SendInput, once.
#pragma once
#include "common/desktop.h"

namespace fi {

class Passthrough {
 public:
  explicit Passthrough(HWND room) : room_(room) {}
  // Line `target` up under the cursor at (u, v) of its picture, and open the hole. False if it can't.
  bool line_up(HWND target, float u, float v);
  void close();  // the room takes the mouse again
  HWND target() const { return target_; }

  void send_move();                          // the cursor as it is, so the window sees it
  void send_button(UINT msg);                // WM_LBUTTONDOWN ... WM_XBUTTONUP, as the room received it
  void send_wheel(int delta, bool horizontal);

 private:
  HWND room_;
  HWND target_ = nullptr;
  POINT hole_{-1, -1};
};

}  // namespace fi
