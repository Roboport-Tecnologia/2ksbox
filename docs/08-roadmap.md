# 8. Roadmap and milestones

The milestones, what each delivered and what is left. The riskiest
bets came first (in-process embedding, 3D into wgpu, TCG on Apple
Silicon), and each milestone ended in something runnable. Designs live
in their docs, working state in `docs/tracks/`, the order of work in
`docs/00-status.md` "Next steps", decisions in doc 10. History is in
the track docs and the commit log, not here.

Reference-rig sessions (doc 09) fed benchmarks into M1, CRT photos into
M2, ATAPI traces and disc dumps into M5, and real-GPU screenshots into
M3/M4.

| | Milestone | Status | Design · track |
|---|---|---|---|
| M0 | Foundation | done | doc 02 |
| M1 | Architecture validation | done | docs 02, 11 |
| M2 | Pixel accuracy and input | done, leftovers | doc 03 |
| M3 | 3D for Win98 (OpenGL) | done, leftovers | doc 12 |
| M4 | Paravirtual Direct3D device | done | doc 14 · `m4-d3d-device.md` |
| M5 | CD-ROM backend, folder discs | done | docs 05, 17 · `m5-cdrom-backend.md`, `m5-dirdisc.md` |
| M6 | Launcher and packaging | shipped, continues | doc 07 · `m6-launcher.md` |
| M7 | XP display driver | done, evolving | doc 15 · `m7-display-driver.md` |
| M8 | x87 / SSE fast paths | done | docs 13, 16 · `m8-tcg-fastpaths.md` |
| M9 | TCG on Apple Silicon | done | doc 22 · `m9-tcg-aarch64.md` |
| M10 | Win98 display driver | active (step 5) | doc 19 · `m10-win98-driver.md` |
| M11 | Windows host | done, leftovers | `build-windows.md` · `m11-windows-host.md` |
| M12 | Music | done | doc 20 · `m12-music.md` |
| M13 | Gamepads | done | `m13-gamepads.md` |
| M14 | Voodoo 2 device | active | doc 21 · `m14-voodoo2.md` |
| M15 | Direct3D executor on Wine | active (steps 5–6) | doc 14 · `m15-wine-executor.md` |
| M20 | Windows 11 (ADR-024) | active (step 1) | `m20-win11.md` |

## M0: Foundation

The repository per doc 02: QEMU v9.2.4 with the qemu-3dfx overlay and
our patch queue (`prepare-qemu.sh`, `configure-qemu.sh`), and a first
`player` (winit + wgpu, integer 4:3 viewport) on Linux and the M1 Air.
A libretro core was built on the way and dropped (ADR-005).

## M1: Architecture validation

QEMU in-process through `libqemu-embed-<target>` and the `qemu-embed`
crate (doc 11): display, input (keyboard, USB tablet, PS/2 grab),
audio (the `embed` audiodev into cpal), QMP over a socketpair
(`player-core/src/qmp.rs`) and the librashader CRT chain. FreeDOS, Win98 and
XP run in the player on Linux and the Air.

- **Latency** on the Air (Win98, crt-lottes, `PLAYER_LATENCY=1`):
  publish → present p50 6–10 ms, p95 15–17 ms, the vsync-phase floor at
  60 Hz and within doc 03's one-frame budget.
- **XP under TCG against the rig's P4 1.7** (`reference/benchmarks/`):
  integer 1.3–2x faster; x87 went from 21 % to 104 % with M8's patches
  05/06 (Super PI 1M 9:49 → 1:57, rig 2:02).

## M2: Pixel accuracy and input

Mode analysis (`player-core/src/mode.rs`, checked by `player
--mode-sweep`), whole-pixel geometry recomputed only on surface
changes, native screenshots (Ctrl+Alt+S). Design in doc 03.

**Left:** presets with no resolution override, the player's own overlay
controls (pause, snapshot, disc swap; doc 07).

## M3: 3D for Win98 (OpenGL)

qemu-3dfx's GL pass-through renders into the player's window-less
context (EGL, CGL, WGL), zero-copy on Linux and macOS (doc 12). The
Glide half of this milestone, our OpenGLide build behind qemu-3dfx's
Glide device, was removed on 2026-09-23 (ADR-020): Glide is the emulated
Voodoo 2's (M14).

**Left:** fence sync instead of `glFinish`, a GL game on a Windows host.

## M4: Paravirtual Direct3D device

ADR-006/007, doc 14. Guest `d3d9.dll` / `d3d8.dll` serialize the API to
the `d3dpt` device (patch 40); the host decoder runs it on DXVK. Phases
P0 to P4 (DXVK on KosmicKrisp and RADV, transport, fixed function,
shaders and queries, D3D8 over d3d9) are done, and D3DGAME9, D3DGAME8
and D3DFEAT9 are byte-identical to native DXVK. On XP the M7 driver
replaced the per-game DLLs; they remain the Win98 path and the
executor's harness.

**Left:** a D3D8/9 game by hand on the DLL path, its stubs, a decoder
thread (the track doc).

## M5: CD-ROM backend and folder discs

Docs 05 and 17. `libdisc` models cue/bin, CCD, MDS and ISO with EDC/ECC
and Q-subchannel synthesis behind the `cdimage` block driver (patch 50)
and raw ATAPI (patch 51), with CD-DA through the drive's `audiodev` and
the in-guest disc shelf (patch 52, `CDSHELF`). SafeDisc, mixed-mode and
ProtectCD dumps pass; `discx repair` is the negative control. `isodir:`
serves a host folder as a generated ISO 9660 + Joliet disc (M5g, doc
17 §8).

**Left:** FIFA 2002's no-match, a second SafeDisc 2 title, SecuROM,
multisession, CHD (the track doc).

## M6: Launcher and packaging

Doc 07. `launcher-core` owns every rule (ADR-014); `launcher-mitsuami`
is the shipped front end since 2026-10-02 (ADR-023, track M19: AppKit,
WinUI 3, GTK 4), after the Qt one (ADR-015, deleted that day) and egui
(deleted by ADR-017); `launcher-capi` and `launcherx` are its other
callers. Done: the library and form (four families and their pickers),
the disc shelf, snapshots, shader profiles with a live preview, clone.
Packages: a Linux tarball (the host's GTK 4), a Flatpak
(`org.gnome.Platform` 49, offline), a macOS app in two builds (ADR-019)
and a Windows zip.

**Left:** an AppImage, a Windows installer, grid thumbnails, bundle
import/export, Flathub screenshots (the track doc's "Open"); the Linux,
macOS and Flatpak packagers' first runs with the mitsuami launcher
(M19).

## M7: XP display driver

ADR-008, doc 15. The `d3dpt-vga` adapter with the miniport + display
DLL pair (`guest-tools/src/d3dptvid/`), register set v5.

- **M7a** framebuffer: the host's mode table, 8 bpp palettes, hardware
  cursor (v4), gamma ramps (v5).
- **M7b** DirectDraw: VRAM surfaces, vblank-paced flips, colour keys.
- **M7c** Direct3D and DX3 execute buffers through a DirectX 8 DDI:
  hardware T&L, vs/ps 1.x (protocol v7), palettized textures (v8), VRAM
  buffers (v9), sixteen streams (v10), cube and volume textures
  (v11, v12), full-screen multisampling (v13), untracked GDI writes
  caught by a target shadow.

FIFA 2000, Max Payne, Vice City, Moto Racer 1997, Diablo and GTA 2 play
on it.

**Left:** a title for each probe-only DX8 feature, more 8 bpp titles, a
driver stage in `scripts/test.sh`.

## M8: CPU fast paths in TCG

x87 on the host FPU with the stack kept as host values inside TCG
(patches 05/06 and later, doc 13), SSE inline (patch 11) and MMX/integer
SIMD inline (patch 12, doc 16).

**Left:** a Direct3D title with and without `*-fast=off`.

## M9: TCG on Apple Silicon

Profile-first work on aarch64 (`-perfmap`, patch 13): JIT write-protect
per thread, TB and TLB fixes, a REP fast path, SMC filtering, inline
jump-cache lookup (every patch in `patches/qemu/README.md`). Doc 22
measures the queue at 2.34x geomean over pristine 9.2.4 on the Air. The
user closed the optimization work on 2026-09-12. Patch 21
(`pinned-regs`, doc 18) crashes XP and is not offered in the form.

**Left:** the Air's game tests uncapped, binary32 at PC=24 on aarch64,
the measurements doc 22 still owes.

## M10: Native Win98 display driver (active)

ADR-012, doc 19. The XP driver's OS-independent core (§19) plus a 9x
layer: `d3dpt9x.drv`, the mini-VDD `d3dpt9v.vxd` and the ring-3 HAL
`d3dpt9hl.dll`, built with Open Watcom. Steps 0–4 are done: PnP install
from the INF alone, the M7c matrix on 98 (§24–§25), visible blue
screens and power-down, and `d3dpt-vga` as the Win98 default. Step 5,
real titles (§26 on), is under way: Crimson Skies, 3DMark 99 / 2001 SE,
Carmageddon in Mode X, Blood in a DOS box.

**Left:** the track doc's next steps.

## M11: Windows host

`docs/build-windows.md`. A native build on the user's PC, in MSYS2
(QEMU with clang, patch 68), into a portable zip and a Store MSIX:
WHPX, the WGL backend, DXVK as `dxvk_d3d9.dll`, Windows' own Direct3D 9
below the floor (ADR-007). The cross build from Linux it began as was
retired on 2026-10-03 (ADR-026).
The launcher is WinUI 3, built with MSVC on the PC only (ADR-023, since
2026-10-02; the Qt launcher before), so the zip is rolled there.

**Left:** Moto Racer's speed on the PC, live control over AF_UNIX, the
Windows-built ISO's `SETUP.EXE` and Win98 half in a guest, an installer, DXGI zero-copy, a check that
boots a guest (the track doc).

## M12: Music

Doc 20. An OPL3 at 0x388 and the Sound Blaster's base and an MPU-401 at
0x330 (patch 60) on three pure-Rust engines in `libsynth/` (Nuked OPL3,
SoundFont General MIDI, a CM-32L on the user's ROMs), mixed in QEMU on
the guest's clock. The pickers, the bank in every package, the
`midi-guest` check and the SB16 interrupt and mixer fixes (patches 25,
61) landed; QEMU's Gravis Ultrasound is offered as a card.

**Left:** Win98's own MIDI through our port loses instruments (capture
it, doc 20 §7.2); optionally a host MIDI port (doc 20 §8).

## M13: Gamepads

QEMU had no gameport, gamepad HID or joystick input class, so each
path is new: the host end (`gilrs`, the `gamepad/` crate,
`PLAYER_PAD_SCRIPT` for headless runs), a key mapping for every guest
(path C), a `usb-gamepad` HID device for Windows (path A, patch 26) and
an ISA gameport for DOS (path B, patch 27). The 9x analog stack was
dropped: the USB pad already reaches DirectInput and winmm on
Win98. `pad-guest`, `pad-guest-xp` and `pad-guest-98` guard it.

**Left:** a real controller on the key mapping; the USB pad on Win98
FE / Me.

## M14: The Voodoo 2 device (active)

ADR-016, doc 21. 86Box's Voodoo 2, vendored verbatim under `voodoo/`,
as `-device voodoo2`, the machine's only Glide since ADR-020 removed the pass-through. 3dfx's own driver runs Quake II, Unreal Tournament, NFS
Porsche, FIFA 2000 and Carmageddon. The command FIFO lives in guest RAM
(`ramfifo=on`, Quake II 41 → 147.5 fps). The 8 MB board is the default.

**Left:** a second Glide game after one quits sometimes starts
glitched; a client resuming on a dead ring; the Air and Windows builds;
patches 64 and 71 upstream (the track doc).

## M15: The Direct3D executor on Wine, on the host (active)

ADR-018, doc 14 §"The executor on Wine, in another process". A Linux or
macOS host below DXVK's Vulkan 1.3 floor runs the same executor on
Wine's d3d9 in a child process. Steps 1–4 are done: the spike, the
transport, the guest on XP and Win98, the launcher's third verdict and
the packages (the macOS community build carries the Wine pair, and no
package ships a Wine).

**Done 2026-09-23:** step 5, the community app on a real macOS 15
(user-confirmed), and step 6, WineD3D-in-guest removed in one commit.
**Left:** the Flatpak's Wine (the user's, on Linux).

## M20: Windows 11 (active)

ADR-024 made 2ksbox a general-purpose VM manager. The first modern box
is Windows 11: EDK2 firmware, a TPM 2.0 from libtpms inside QEMU, x86_64
under KVM or WHPX, and Windows 11 on Arm under Hypervisor.framework on
Apple Silicon. Steps in the track doc.

## Post-v1 candidates

Recording and streaming, CRT bezel packs, VRR pacing, suspend/resume,
a host MIDI port for a real module (doc 20 §8), QEMU in its own process
(ADR-010), upstreaming (libdisc, the embed API).

## Standing risks

| Risk | State |
|---|---|
| GL → wgpu interop | Resolved: zero-copy rings on Linux and macOS (embed API v5/v6). |
| Host-side Glide | Resolved: the Voodoo 2 device (doc 21); the OpenGLide wrapper was built, then removed (ADR-020). |
| Apple's GL deprecation | Watched. ANGLE or Zink is the escape hatch. |
| XP on TCG on Apple Silicon | Addressed: integer beyond the P4 1.7, x87 at its speed (M1, M8, M9). |
| Hosts below DXVK's Vulkan floor | Addressed: Windows' own d3d9 (ADR-007), Wine on the host (ADR-018). |
| Drift from upstream QEMU | Pinned at v9.2.4, with a patch queue re-applied by `prepare-qemu.sh`. |
| QEMU dependency bloat | Addressed: unused backends disabled, 175 → 93 shared libraries (the `no-optionals` check). |
| Protection checks needing dump features | `discx` inspects, verifies and makes negative controls. |
