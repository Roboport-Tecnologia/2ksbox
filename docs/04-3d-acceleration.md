# 4. 3D acceleration: strategy and paths

How a guest's 3D reaches the host GPU: which path serves which API on
which Windows, what backs each one with and without Vulkan 1.3, and what
counts as done. The paths are designed elsewhere: the GL context
provider in doc 12, the paravirtual Direct3D device and its executor in
doc 14, the XP and 9x display drivers in docs 15 and 19, the Voodoo 2 in
doc 21. The guest-tools ISO's layout is `guest-tools/README.md`.

## Strategy

We don't write GPU emulators for the era's APIs. Guest 3D leaves through
API pass-through and paravirtual devices:

- **OpenGL** goes through qemu-3dfx's wrapper and device. The host draws
  it with its own OpenGL (doc 12).
- **Direct3D 3–9 and DirectDraw** go through our display adapter,
  `d3dpt-vga`. Its driver writes the DDIs into a command window that a
  native host executor runs on a real Direct3D 9 (docs 14, 15, 19).
- **One chip is emulated**: a Voodoo 2 (`-device voodoo2`, 86Box's code,
  ADR-016, doc 21), the one Glide on a machine (ADR-020). It runs 3dfx's
  own driver and the game's own Glide 2 or 3 at a software rasteriser's
  speed. OpenGL has no chip equivalent.

That gives Win98 and XP host acceleration on every platform we ship,
Apple Silicon included.

## The paths

| API | Win98 | Windows XP |
|---|---|---|
| Glide 2 / 3 | the Voodoo 2 device with 3dfx's driver and the game's own Glide (a DOS game brings its own `GLIDE2X.OVL`) | the same (few titles care) |
| OpenGL | qemu-3dfx GL pass-through (`OPENGL32.DLL`) | the same |
| DirectDraw, Direct3D 3–7 | `d3dpt-vga`'s driver (doc 19); below the Vulkan floor, see "Fallbacks" | `d3dpt-vga`'s driver (doc 15); the same |
| Direct3D 8 / 9 | the driver's DirectX 9 DDI under Windows' own `d3d9.dll` / `d3d8.dll` (ADR-021; the per-game DLLs were retired in M16 step 7); the same fallbacks | the same |
| 2D desktop | `d3dpt-vga`'s driver (the default adapter); `-vga cirrus` with Windows' in-box driver one pick away | the same |

Which host Direct3D 9 the executor calls (design in doc 14, decisions in
ADRs 007, 013 and 018):

| Host | Backend | How it is picked |
|---|---|---|
| Vulkan 1.3 (Linux, Windows, macOS 26+ over KosmicKrisp) | DXVK, the default everywhere and the only one goldens are taken with | automatic |
| Windows below the floor | the system's own `d3d9.dll` | `D3DPT_D3D9=auto\|dxvk\|system`, `-device d3dpt-vga,d3d9=…` |
| Linux / macOS below the floor, with a Wine | the executor's Windows build in a Wine process on the host, on Wine's `d3d9` (M15) | `-global d3dpt-vga.exec=wine` |
| Linux / macOS below the floor, no Wine | none: the adapter reports no executor | `-global d3dpt-vga.no-exec=on` models it |

A software Vulkan (lavapipe) counts as available: the launcher warns it
will be slow and lets the box decide. The launcher's probe
(`launcher-core/src/host_gpu.rs`, `launcherx --host-check`) answers in
the same terms, and the machine form's Direct3D row offers only what the
host can run.

## Constraints we design around

- **Version coupling.** qemu-3dfx's guest wrappers and the patched QEMU
  must come from the same commit (the host checks a signature stamp), so
  the guest-tools ISO is built with each build of our QEMU, never
  downloaded. qemu-3dfx publishes its patch only up to QEMU 9.2, so we
  carry our own port for the pinned v11.1.2 (ADR-001's amendment,
  track M21).
- **Host GL on macOS** is Apple's OpenGL framework (4.1 core / 2.1
  compat), a long-term risk. If Apple removes it, the way out is ANGLE
  (GL ES on Metal) or a Zink-style layer.
- **Windowing.** Era games use exclusive full screen and mode changes;
  the player follows them with event-driven geometry changes and CRT
  shader resets (doc 03).
- **Output interop.** Guest 3D reaches wgpu without a CPU readback: a
  dma-buf ring on Linux, an IOSurface ring on macOS (doc 12 §4). Windows
  still reads frames back; a DXGI shared handle is open
  (`docs/00-status.md`).

## Fallbacks and alternatives

- **A host with no executor** (no Vulkan 1.3, and on Linux / macOS no
  Wine) gives the guest no Direct3D: the driver keeps its DirectDraw
  half, a game gets the runtime's software device, OpenGL still passes
  through and the Voodoo 2 still works. `-global d3dpt-vga.no-exec=on` (doc 15) models
  such a host on any box. WineD3D in the guest (wine9x, Wine 1.7.55 over
  the GL pass-through from the ISO) was the fallback here until ADR-018
  retired it and M15 step 6 removed it (2026-09-23).
- **The Cirrus adapter** with Windows' in-box driver: plain 2D, no
  acceleration, the control for any display-driver question.
- **SoftGPU** (software 3D in the guest, from the wine9x author) would
  work with no host GPU, slowly under TCG. Planned for the ISO, not
  shipped.

## Acceptance matrix (M3 / M4 exit criteria)

A small fixed set per API. Pass means it renders correctly at a playable
rate with acceleration verifiably active (renderer string not software;
`TESTS\GLPROBE.EXE` and `DRIVER\DDTEST.EXE` / `D3D7TEST.EXE` check that
inside the guest).

| Guest | Planned | Where it stands |
|---|---|---|
| Win98 GL | Quake 2 | GLQuake on the pass-through (Linux, in the player); Quake II's MiniGL on the Voodoo 2 (by hand) |
| Win98 Glide | Unreal | UT on the Voodoo 2 (by hand) |
| Win98 D3D | Forsaken or Incoming (D3D6) | neither run; 3DMark 99 / 2001 SE, Crimson Skies, Moto Racer, Max Payne 2 and Vice City on the 9x driver (doc 19, track M16) |
| XP GL | Quake 3 | not run |
| XP D3D8 | Max Payne | plays on the driver's DDI with no DLL in the game folder (doc 15) |
| XP D3D9 (stretch) | Half-Life 2 or GTA:VC | Vice City (a DirectX 8 title) plays; no D3D9 title yet |
| Benchmarks | 3DMark 99 / 2001 SE (98), 2001 SE / 03 (XP) | 99 and 2001 SE run on Win98 headless (`tools/w98-3dmark*.sh`); XP not run |

Most of this evidence is from the Linux host; "on all three host
platforms" is not met yet. Per-host state and open title work are in
`docs/00-status.md` and `docs/tracks/m10-win98-driver.md`.
