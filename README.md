# FullImmersion

A 3D desktop that Milk and her agents share: real programs placed anywhere around you, plugins for everything,
live visualizations of what's going on, flat on the monitor or in VR.

The plan: [FullImmersion: architecture and MVP plan](https://claude.ai/artifact/1mgSyaGALos9mCNwsMe4Y3)

## Where it stands

**M1, the flat room** (`FullImmersion.exe`): a 3D room over your monitor with your real windows pinned round you.

| Do | How |
|---|---|
| Pin a window | Right-click empty space, then **Pin a window** |
| Use a window | Just point at it: click, scroll, right-click and type as usual |
| Move a panel | Drag the bar under it |
| Resize a panel | Scroll on the bar (Shift+scroll: nearer or farther) |
| Panel menu | Right-click the bar (bigger, smaller, closer, farther, 1:1, unpin) |
| Look round / zoom | Drag empty space / scroll on empty space |
| Hide or show the room | Ctrl+Alt+F |
| Quit | Ctrl+Alt+Q |

The layout is saved to `%APPDATA%\FullImmersion\layout.json` and windows are found again by program and title
when it starts. A log is kept beside it.

How the mouse reaches a real window: the window is slid behind the room so the point you're on sits under the
cursor, and the room keeps a one-pixel hole at the cursor's tip (`src/app/passthrough.h`).

**M2, the room in the headset** (`FullImmersion.exe --vr`, SteamVR running, headset on when it starts): the
same room and layout around you, each window its own OpenXR quad layer so text stays sharp. A small window on
the monitor mirrors what you see; right-click it to pin a window or quit.

| Do | How (Index controllers) |
|---|---|
| Use a window | Point at it: trigger is its left button, A its right button, the stick scrolls it |
| Carry a panel | Grip on it (or trigger on its bar), move your hand, let go |
| Nearer or farther / bigger or smaller | While carrying: stick up and down / left and right |
| Pin a window, quit | Right-click the mirror window on the monitor; Ctrl+Alt+Q |

The cursor follows the hand that last clicked. A window partly off screen is slid on when you point into the
part that's off, so the click lands. The log notes frames per second and our own time per frame every 5 s.

**S0, the spikes** (done). Three small programs that proved the hard parts on Milk's PC (Windows 11, RTX 3060,
Bigscreen Beyond on SteamVR) before the real app is built:

| Spike | Proves | Run |
|---|---|---|
| `s1_capture` | A real window captured live into a GPU texture (Windows.Graphics.Capture) | `s1_capture --list`, `s1_capture notepad 5` |
| `s2_quad` | That window as an OpenXR quad layer in the headset, written only when it changes | `s2_quad notepad 30` (SteamVR running) |
| `s3_input` | A point on the panel becomes a click and typing in the real window, checked by capture | `s3_input notepad --click 0.5 0.5 --type "hello"` |

## Build (Windows)

Visual Studio 2026 with the C++ workload and a Windows 11 SDK. The first configure downloads the OpenXR SDK.

```
cmake --preset windows
cmake --build --preset release
build\Release\FullImmersion.exe
build\Release\fi_tests.exe
```

On other systems only the core and its tests build (`cmake -S . -B build && cmake --build build && ctest --test-dir build`).

Each spike saves its pictures under `out\` in the folder it runs from.
