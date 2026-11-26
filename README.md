# FullImmersion

A 3D desktop that Milk and her agents share: real programs placed anywhere around you, plugins for everything,
live visualizations of what's going on, flat on the monitor or in VR.

The plan: [FullImmersion: architecture and MVP plan](https://claude.ai/artifact/1mgSyaGALos9mCNwsMe4Y3)

## Where it stands

**S0, the spikes.** Three small programs that prove the hard parts on Milk's PC (Windows 11, RTX 3060,
Bigscreen Beyond on SteamVR) before the real app is built:

| Spike | Proves | Run |
|---|---|---|
| `s1_capture` | A real window captured live into a GPU texture (Windows.Graphics.Capture) | `s1_capture --list`, `s1_capture notepad 5` |
| `s2_quad` | That window as an OpenXR quad layer in the headset, written only when it changes | `s2_quad notepad 30` (SteamVR running) |
| `s3_input` | A point on the panel becomes a click and typing in the real window, checked by capture | `s3_input notepad --click 0.5 0.5 --type "hello"` |

## Build (Windows)

Visual Studio 2022 with the C++ workload and a Windows 11 SDK. The first configure downloads the OpenXR SDK.

```
cmake --preset windows
cmake --build --preset release
build\Release\s1_capture.exe --list
```

Each spike saves its pictures under `out\` in the folder it runs from.
