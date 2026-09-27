# Track M7: the XP display driver (doc 15, ADR-008)

This track gave XP a real display driver for the `d3dpt-vga` adapter:
the QEMU device, the miniport and display DLL, their DirectDraw and
Direct3D DDIs, the executor's half that runs the driver's records, and
the tests. Doc 15 is the design, doc 14 the protocol and executor; the
Win98 driver on the same core is doc 19 and the M10 track. Read
`docs/00-status.md` first for the track rules.

## State

M7 is **done** (roadmap `docs/08`). The driver still changes; each new
DirectX 8 feature is a protocol bump made here.

- **M7a**, framebuffer: the host's mode table, the desktop from VRAM,
  unattended install (`DRIVER\DRVINST.EXE`), 8 bpp palettes, hardware
  cursor, gamma ramps. Register set **v5** (`D3DPT_FB_VERSION`); drivers
  built since 2026-09-12 accept any adapter at or above their version.
- **M7b**, DirectDraw: VRAM surfaces, flips paced by a vertical blank,
  colour keys.
- **M7c**, Direct3D from DX3 execute buffers through the DX8 DDI:
  hardware T&L, vs/ps 1.x, palettized and compressed textures, VRAM
  vertex/index buffers, sixteen streams, cube and volume textures,
  anisotropic filtering, multisampling. Protocol **v13**
  (`D3DPT_PROTO_VERSION`).
- Titles: FIFA 2000, Max Payne, Diablo, Moto Racer 1997, GTA 2, GTA Vice
  City (played by hand by the user). The user's daily XP runs on it with
  no custom DLL. The FIFA 2000 keyboard shim `D3DPT\DINPUT.DLL` is
  installed per game by decision (doc 15 "FIFA 2000 on the HAL").
- Below the Vulkan floor the executor runs under Wine (M15,
  `EXEC=wine`) or on Windows' d3d9 (`d3d9=system`); with `no-exec=on`
  the driver keeps DirectDraw and offers no Direct3D (doc 15).

## Scope and files

Owned by this track:

- QEMU device: `d3dpt/hw/d3dpt_vga.c`, register set `d3dpt/d3dpt_fb.h`
  (versioning rules in doc 15 "Newer register sets are accepted"), and
  the executor loader `d3dpt/hw/d3dpt_exec_load.[ch]`.
- Guest driver, `guest-tools/src/d3dptvid/`: the NT layer `nt/`
  (miniport `d3dptvid.c`, display DLL `d3dptdisp.c`, INF, `.def`) over
  the OS-independent `core/` (DP2 walker, surface table, caps, flip
  chain; shared with M10, doc 19 §19), `kcrt.c`, `drvinst.c`,
  `setmode.c`, the vendored `ddk/`, and the test programs (`ddtest`,
  `d3d7test`, `ditest`, `dxttest`, `shtest`, `cktest`, `ebtest`,
  `gammatest`, `zfilltest`, and the DX8 probes over `d3d8probe.h`).
  `guest-tools/build-driver.sh` builds them; `build-wrappers.sh` runs it
  to stage `DRIVER\` on the ISO.
- Executor, the driver's half: `d3dpt/exec/d3dpt_exec_ddi.cpp` (VRAM
  surfaces, contexts, the DP2 interpreter, readback),
  `d3dpt_exec_int.h`, `tools/d3dpt-dp2-test.cpp`.
- Tests: `tools/xp-driver-test.sh`, `tools/xp-fifa-match.sh` +
  `xp-fifa2000.bat`, `xp-diablo.sh`, `xp-motoracer.sh` +
  `motoracer-state.py`, `xp-maxpayne.bat`, `xp-vicecity.sh`.

Shared (rebase first, edit minimally, name the other track in the
commit): `d3dpt/d3dpt_proto.h`, `d3dpt/exec/d3dpt_exec.cpp` /
`d3dpt_exec.h` (M4), `core/` (M10), the
remote executor files (M15), `scripts/test.sh`, `player/`.

## Where the design lives

| Topic | Doc 15 section |
|---|---|
| adapter, miniport, mode table | "Shape (M7a)", "Building kernel-mode PE files with GCC" |
| DirectDraw HAL, dxg's caps rules | "The DirectDraw DDI (M7b)", "Blit caps and the HEL" |
| flip pacing; a flip swaps roles, not memory | "The flip chain's vertical blank", "A DirectX 6 title's flip chain" |
| 8 bpp, palettes, colour keys | "8 bpp palettized modes", "Palettized textures and colour keying" |
| GDI writes into a Direct3D target | "Untracked writes" |
| cursor, gamma | "The hardware cursor", "Gamma ramps" |
| DX3 execute buffers | "Execute buffers (the DirectX 3 path)" |
| DX7 HAL, DX8 DDI, d3d8.dll's findings | "The Direct3D DDI (M7c)", "The DirectX 8 DDI (M7c)" |
| shaders, VRAM buffers, streams, cubes, volumes, formats, MSAA | their subsections under the DX8 DDI |
| one probe per DX8 feature | "The DX8 feature probes" |

## Build and test loop

```sh
scripts/build.sh                         # QEMU, the executor, both driver ISOs
guest-tools/build-driver.sh              # just the driver: guest-tools/out/d3dpt-driver.iso
scripts/build-d3dpt-exec.sh              # after touching d3dpt/exec/
build/d3dpt-dp2-test x.bmp               # the records through the executor, no guest (the d3dpt-dp2 check)
```

After a protocol bump, `scripts/build.sh` rebuilds QEMU, the executor
and the ISO together; one side alone gives `protocol mismatch` and no
Direct3D. XP loads the driver from `system32`, not the ISO, so
**reinstall after every driver build**:

```sh
tools/xp-driver-test.sh <image> install     # DRVINST from the ISO, restart, desktop on the driver
tools/xp-driver-test.sh <image> ddtest      # DirectDraw at 8/16/32 bpp + windowed
tools/xp-driver-test.sh <image> d3d7        # the DX7 HAL; frame must equal d3dpt-dp2-test's
tools/xp-driver-test.sh <image> d3dgame8    # M4's scene through XP's d3d8.dll; diffed against the native oracle
tools/xp-driver-test.sh <image> probes      # every DX8 feature probe in one boot
OUT=build/xd/sh tools/xp-driver-test.sh <image> shtest   # also cktest, ebtest, gamma, modes
tools/xp-driver-test.sh <image> cmd 'D:\DRIVER\DDTEST.EXE 800 600 32 300'
```

Keep `OUT=` short (QMP socket path limit). The knobs (`DDFLAGS=`,
`EXEC=wine`, `NO_EXEC=1`, `VGA=cirrus`, `DRIVER_ISO=`, `KEEP=1`, ...)
and the game loops (`xp-fifa-match.sh kvm|tcg`, `xp-diablo.sh
install|play`, `xp-motoracer.sh install|play`, `xp-vicecity.sh play`,
`GAME_ISO=… tools/xp-driver-test.sh <image> bat tools/xp-maxpayne.bat`
or `xp-fifa2000.bat`) are in `docs/testing.md`.

**Images.** Use an overlay or copy of an XP image with the driver
installed, never the user's own `~/vms/winxp-m7.qcow2`. Scratch copies
exist (`winxp-m7g` has Diablo and the game installs); otherwise make one
with `install`. `~/vms/winxp.qcow2` is M4's cirrus image.

## Diagnostics

The driver writes through the DEBUG register into the QEMU log
(`d3dpt-vga: guest: …`); no WinDbg, no serial KD. Lines to read:

- `d3dpt-vga: ddi: …` are the executor's (unsupported states and
  tokens, once each); `batch N: error` is a refused record.
- `d3dptdisp: dp2 0x…` is a DrawPrimitives2 the host failed.
- `dx8 draws skipped … why …` is the driver's. Bits: 1 shader, 2 no
  FVF, 4 no stream, 8 stride < FVF, 16 vertex range, 32 index range,
  64 primitive.
- `N page flips in 5.0 s` is a title's frame rate; no line means it
  blits to the primary.
- `N untracked guest pixels` counts GDI writes caught by the target
  shadow.

`D3DPT_DP2_TRACE=<flag file>` (one frame per `touch`),
`D3DPT_DDI_REREAD=1`, `D3DPT_DDI_NOFOG=1` and disassembling an image's
`ddraw.dll` / `dxg.sys` are in doc 15 "Debugging the driver".
`-device d3dpt-vga,ddflags=N` withdraws one feature for an A/B without a
reinstall (doc 15 "The ddflags bits"); most used: `0x20` no Direct3D,
`0x1000` no T&L, `0x2000` a DX7 driver to d3d8.dll, `0x8000` no vertical
blank.

## Traps

The rules are in doc 15; these are the ones a session meets first.

- Kernel mode with mingw-w64: `ntdef.h` + `ddk/miniport.h`, never
  `ntddk.h`; GCC emits `memcpy`/`memset` even freestanding (`kcrt.c`);
  `build-driver.sh` checks the import lists (doc 15 "Building
  kernel-mode PE files with GCC").
- dxg silently drops the whole HAL to `DDCAPS_NOHARDWARE` for
  `DDCAPS_GDI`, palette caps, colour-key caps without a Blt callback, or
  a mode without Direct3D. A `DRAWPRIMITIVES2EX` device must answer
  `GUID_Miscellaneous2Callbacks` with `GetDriverState`.
- Never re-register the flip chain in `DdFlip`: on NT a flip exchanges
  the surfaces' roles, not their memory.
- Never let DXVK throw or assert: its exceptions abort QEMU and its
  shader compiler asserts on an unknown opcode. Validate every index,
  count and shader (`sm1_valid`) first.
- Never claim `D3DPMISCCAPS_CLIPTLVERTS`; the runtime's clipped fans are
  stream-0 draws into its own clip buffer.
- XP SP3's driver-signing Logo dialog ignores every policy; DRVINST
  presses it, once per unsigned file, for the whole install.
- Keys typed while a full-screen DirectDraw window is up are lost, and
  the Run dialog silently truncates long lines. Use one short `cmd` line
  or stage `E:\RUN.BAT` (`bat`).
- On macOS a standalone `qemu-system-i386` with no Vulkan loader on
  `DYLD_LIBRARY_PATH` segfaults in DXVK at the first Direct3D request,
  which reads as "the guest never reached its shell".
  `xp-driver-test.sh` sets the environment, as `scripts/test.sh` does.

## Next steps

1. ~~An XP whose driver is refused shows black~~: it was patch 44's
   retired TLB table coming back across the VGA window's topology flush
   (fixed 2026-09-24; `xp-driver-test.sh vesa`). The inbox driver's VESA
   desktop renders on the adapter; a failed install is no longer blind.
2. **A `driver` stage in `scripts/test.sh`**: boot on `d3dpt-vga`, run
   `d3d7` + `shtest` + `probes` from a snapshot, as the XP D3D stage
   does. Until then `tools/xp-driver-test.sh` is the check.
3. **Titles for the features only probes have seen** (table below).
4. Present the host frame through the player's 3D path instead of the
   per-frame readback into VRAM.
5. Small items, each when a title asks: `DrvDeriveSurface` (GDI on
   DirectDraw surfaces; an optimisation given the target shadow), a real
   blitter behind `DDCAPS_BLT`, a vblank from the player's present, the
   mode table fed from the player (M2), RT/N patches (PATCHTST is the
   only probe still "not offered"), a RAM-backed palette page for fast
   palette animation, more 8 bpp titles (StarCraft, Age of Empires,
   Caesar 3).
6. A bytecode validator for SM2/3 on M4's d3d9 half. Not a v1 blocker
   (user decision: hostile guest input is an accepted risk).

## Games to test, by feature

Reinstall the driver from this build's ISO and run QEMU and the executor
from the same build. Read the QEMU log first (`ddi:` lines, `dx8 draws
skipped`). Run each feature's probe (doc 15 "The DX8 feature probes")
before its title; a title that fails where its probe passes is the
title's own problem.

| Feature (probe) | Title | Look for |
|---|---|---|
| streams, v10 (STRMTEST) | Unreal Tournament 2003/2004 | meshes, terrain, characters whole; `ddflags=0x200000` the A/B |
| vertex shaders 1.1 (SHTEST) | 3DMark2001 SE, Morrowind | no `vertex shader … refused` / `not valid vs 1.x` |
| pixel shaders 1.1–1.4 (SHTEST) | 3DMark2001 SE PS tests, Morrowind water | water shaded, not milk |
| point sprites (SPRTEST) | 3DMark2001 SE Point Sprites | sized particles |
| DOT3, EMBM (BUMPTEST) | 3DMark2001 SE, C&C Renegade water, Dungeon Keeper 2 | relief, not flat |
| cube maps, v11 (CUBETEST) | 3DMark2001 SE Nature | sky and trees in the water |
| anisotropic (ANISTEST) | UT2003/2004 `LevelOfAnisotropy` | sharp floors at a grazing angle |
| DX8 by hand | Max Payne, first levels | alley walls and ground whole |
| regression | Vice City, D3DGAME8, Moto Racer, FIFA 2000 | same frames and rates |

3DMark2001 SE's whole benchmark has run through the shared core on
Win98 (doc 19 §36–§39), never on XP.
