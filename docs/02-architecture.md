# 2. Architecture: in-process QEMU, player + launcher, process model

How the pieces fit: QEMU linked into the player, the launcher beside
it, the threads, the languages and the repository. The embed API itself
is doc 11, the display pipeline doc 03, the launcher and packaging
doc 07, and the rationale for every decision doc 10.

Three locked decisions shape it: QEMU runs in-process with the
display (ADR-002), a standalone Rust player plus a launcher with no
RetroArch/libretro (ADR-005), and Rust wherever possible (ADR-004).

## Why in-process

Framebuffer, input and audio must not cross a process boundary. Linking
QEMU as a library gives zero-copy framebuffer access, direct input
injection and one clock for pacing. The costs: one VM per process
(QEMU's global state and incomplete cleanup), a QEMU fork to maintain
(needed anyway for qemu-3dfx, the CD backend and our devices), and
GPL-2.0 for everything that links it.

## Component map

```
┌──────────────────────── player process (Rust) ───────────────────────┐
│  ┌────── QEMU fork (C, libqemu-embed-<i386|x86_64|aarch64>) ──────┐  │
│  │  TCG + our fast paths / KVM / WHPX / HVF                       │  │
│  │  qemu-3dfx's port: OpenGL pass-through (hw/mesa)               │  │
│  │  d3dpt-vga + Direct3D executor (DXVK / system d3d9 / Wine)     │  │
│  │  voodoo2 (86Box's chip), opl3 + mpu401 → libsynth              │  │
│  │  ATAPI raw-CD device ──► libdisc (Rust staticlib, C API)       │  │
│  │  TPM 2.0 on libtpms (tpm/), qemu-vdagent clipboard             │  │
│  │  display listener ─┐   input inject ◄─┐   embed audiodev ─┐    │  │
│  └────────────────────┼──────────────────┼───────────────────┼────┘  │
│        libqemu_embed.h (hand-written `qemu-embed` bindings)  │       │
│  ┌──────────────── player-core ──────────┼───────────────────┼────┐  │
│  │  frame handoff: 2D surface copy / 3D dma-buf or IOSurface ring │  │
│  │  mode analysis → geometry/shader params (event-driven)         │  │
│  │  shader-chain: librashader on wgpu (CRT presets)               │  │
│  │  geometry (aspect, integer scale) → wgpu present               │  │
│  │  input, gamepads ─────────────────────┘   cpal audio ◄─ ring   │  │
│  │  QMP over a socketpair (events, PLAYER_QMP_EXEC)               │  │
│  │  shared folder (libsmb), host clipboard                        │  │
│  └────────────────────────────────────────────────────────────────┘  │
│  window: player/ (winit) or player-mitsuami/ (ADR-025)               │
└──────────────────────────────────────────────────────────────────────┘
               ▲ spawns, + a -qmp unix: socket for live control
┌───────────────────────── launcher process ───────────────────────────┐
│  launcher-core (Rust): bundles, library, disc shelf, snapshots,      │
│  shader profiles, preview, every window's state machine              │
│  front ends: launcher-mitsuami (AppKit/WinUI 3/GTK 4, as `2ksbox`),  │
│  launcher-capi (C ABI), launcherx (toolkit-free debug verbs)         │
└──────────────────────────────────────────────────────────────────────┘
```

- **A machine bundle** is a directory with `machine.toml` and usually
  its disk; discs sit on a shelf shared by every machine. The launcher
  turns a bundle into the player's command line; the player runs a QEMU
  command line and knows nothing of bundles (doc 07).
- One VM per player process, so a crashed guest never takes the library
  down and several machines run at once. For live control (snapshots,
  disc swaps) the launcher speaks QMP to a second monitor socket it adds
  at spawn; neither binary has an IPC channel of its own (doc 07, "How
  the launcher reaches a running machine").
- **The player** is `player-core`, everything but the window, under
  two front ends: `player/` on winit and `player-mitsuami/` on the
  mitsuami toolkit (ADR-025, track M22). The Windows packages ship
  `player-mitsuami` as `2ksbox-player.exe`. A guest of another
  architecture is another player binary (`--features qemu-x86_64` /
  `qemu-aarch64`), never a library opened at run time (doc 22 §5.0).
- **Modern guests** (ADR-024, track M20) get a TPM 2.0 kept in QEMU's
  process (`tpm/qemu/`, libtpms). The host clipboard goes through
  QEMU's `qemu-vdagent` and our `guest-agent/`, and a host folder is an
  SMB share served by `libsmb` in the player (track M23, doc 24).

## The embed boundary: `libqemu_embed.h`

A small C API on the QEMU fork (`embed/`, **v11**; the version history
and every call are doc 11): lifecycle from a plain `qemu-system`
command line, VM control, a 2D display listener and a 3D frame copy or
zero-copy ring (dma-buf on Linux, IOSurface on macOS), keyboard,
pointer and gamepad input, a caller-owned audio ring, and
`qemu_embed_socket_to_fd()` for the QMP socket on Windows and
`qemu_embed_setenv()` for the environment QEMU reads (doc 11, "The C
runtime boundary"), and a clipboard peer on `qemu-vdagent`. Media,
snapshots and status go over QMP. The Rust bindings in the
`qemu-embed` crate are hand-written (no libclang), and
`qemu_embed_api_version()` guards drift. The player never reaches past
this header.

## Language policy (ADR-004)

- **Rust:** `player-core` and both players, `qemu-embed`,
  `launcher-core`, `launcher-capi` and `launcher-mitsuami` (native
  widgets through the mitsuami toolkit), `shader-chain`, `libdisc` (CD
  model and parsers, a staticlib with a C API for QEMU's ATAPI device),
  `libsynth` (the music engines, doc 20), `libsmb` (the SMB server),
  `gamepad`, `guest-agent` (the Windows 11 guest agent), and host-side
  tools.
- **C/C++:** our QEMU patches and devices (embed, `d3dpt/`, `voodoo/`,
  `tpm/`, `hw/audio` opl3/mpu401), the Direct3D executor, and the test
  harnesses in `tools/`.
- **Era C and assembly** for the guest side: the XP and Win9x display
  drivers, Windows 7's WDDM driver
  (`guest-tools/src/d3dptvid/wddm/`), the per-game guest DLLs
  (`OPENGL32.DLL`, `DINPUT.DLL`), and the guest-tools programs.
- Python (uv-managed) for test drivers and helpers.

## Graphics stack

wgpu (Metal on macOS, Vulkan/D3D12 elsewhere) with **librashader's wgpu
runtime** for slang shaders, wrapped in the `shader-chain` crate that
the player and the launcher's preview share. librashader pins a wgpu
major (0.12 → wgpu 30) and the workspace follows that pin, not the
newest wgpu. Geometry updates are event-driven (`Gpu::guest_surface_changed`,
`Gpu::resize`).

## Threading model

- **QEMU main loop + vCPU threads**, QEMU-managed (MTTCG where TCG
  runs).
- **Render thread:** wgpu, librashader, present. The display listener
  (QEMU's main loop) only publishes a dirty rect into a triple-buffered
  handoff, or a ring slot index for 3D. QEMU never blocks on vsync; a
  slow host frame repeats the last guest frame.
- **Audio thread:** real time; cpal drains the ring QEMU's mixer fills.
- **Event thread:** the window's (winit or mitsuami); forwards input to
  QEMU without waiting.

Rule: no QEMU-owned thread waits on the GPU; no render or audio thread
takes a QEMU lock.

## Acceleration per platform

| Host | x86 guests | Notes |
|---|---|---|
| Linux x86_64 | KVM (TCG fallback) | the performance reference |
| Windows x86_64 | WHPX (TCG fallback) | a bundle's `accel = "kvm"` is spelled `whpx` there; era machines on `qemu-system-i386` since patch 84 |
| macOS Apple Silicon | TCG (MTTCG) | Win98 easy; XP at 104 % of a 1.7 GHz P4 with the TCG fast paths. Windows 11 on Arm runs under HVF (M20) |
| macOS Intel | TCG | community build only: permitted, untested (ADR-019) |

Win98 machines default to TCG even where KVM exists (doc 07). macOS's
JIT needs the `com.apple.security.cs.allow-jit` entitlement on the app
(`packaging/macos/2ksbox.entitlements`).

## Repo layout

```
player-core/     the player minus its window: present, audio, input, QMP, sharing
player/          the player's winit front end
player-mitsuami/ the player's mitsuami front end (ADR-025), its own workspace
qemu-embed/      hand-written Rust bindings to libqemu_embed.h
embed/           the embed library's C sources, overlaid into qemu/embed/
launcher-core/   everything the launcher decides; src/bin/launcherx.rs
launcher-mitsuami/ the shipped launcher (mitsuami: AppKit, WinUI 3, GTK 4), its own workspace
launcher-capi/   launcher-core as a C ABI (non-default workspace member)
shader-chain/    librashader-on-wgpu chain shared by player and launcher
libdisc/         CD-ROM model, image formats, C API, discx
libsynth/        OPL3 / General MIDI / MT-32 engines, synthx
libsmb/          the SMB 2/3 server behind a machine's shared folder (doc 24)
guest-agent/     the Windows 11 guest agent: clipboard, share mapping (doc 24)
tpm/             the libtpms TPM backend, overlaid into QEMU (M20)
gamepad/         the abstract gamepad (Rust) and the guest pad devices (qemu/)
d3dpt/           Direct3D protocol, the d3dpt-vga device, the executor
voodoo/          86Box's Voodoo 2 (verbatim), its shim, the QEMU device
cdshelf/         the in-guest disc shelf protocol
guest-tools/     the guest-tools ISO: drivers (XP, 9x, WDDM), guest DLLs, SETUP.EXE, tests
patches/         our patch queues: qemu, qemu-3dfx, dxvk, seabios, edk2, deps, winetest
qemu/            submodule: QEMU v11.1.2, prepared by scripts/prepare-qemu.sh
third_party/     qemu-3dfx, dxvk, slang-shaders; khronos headers, uide
firmware/        our VGA BIOS builds (VBE 4F09h)
soundfonts/      the shipped General MIDI bank
shaders/         our own CRT presets (the rig's monitor)
packaging/       Linux, Flatpak, macOS, Windows assets and the icon
scripts/         build, prepare, package and test entry points
tools/           test harnesses and diagnostics (docs/testing.md)
reference/       rig captures: D3D goldens, benchmarks
docs/            design documents and tracks
```

## Open questions

- **Host 3D texture sharing on Windows:** Linux and macOS are
  zero-copy (doc 12 §4); Windows still takes the CPU-copied frame, and
  a DXGI shared handle is open (M11).
- **QEMU cadence:** pinned to v11.1.2 plus a patch queue (track M21
  moved it from v9.2.4), built with only what is used (the
  `no-optionals` check). No further rebase is scheduled.

In-process QMP is settled: a `socketpair` (a loopback pair on Windows),
standard QMP JSON, no filesystem path (doc 11).
