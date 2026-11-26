# FullImmersion: notes for agents

- The plan lives in the Claude Doc linked from README.md. Read it before changing the architecture.
- Windows only for now (WGC, D3D11, SendInput). Build: `cmake --preset windows` then `cmake --build --preset release`.
- `src/common` is shared by every program and has no OpenXR in it. Dependencies point down only.
- Input into real windows must always check it lands on the window meant (`fi::click` refuses otherwise).
  Agents never send input into Milk's windows without a grant she gave.
- Milk is she/her; call her Milk (or Fox).
