# 15. A real XP display driver (ADR-008, M7)

This is the XP guest side of doc 14. Instead of DLL copies in each game
folder, a Windows display driver pair owns the adapter and speaks the
DirectDraw and Direct3D DDIs into the doc 14 transport and executor.
ADR-008 (doc 10) has the why; this doc has the how, for whoever works on
the driver, the adapter or the executor's DDI path. The Win98 driver
shares the OS-independent `core/` described here (doc 19 §19 has the 9x
specifics). The track's loop and open work are in
`docs/tracks/m7-display-driver.md`, and every test tool named here is in
`docs/testing.md`.

The driver came in three stages, which still name the parts: **M7a** the
framebuffer driver, **M7b** the DirectDraw DDI, **M7c** the Direct3D DDI
(a DirectX 7 HAL, grown into a DirectX 8 DDI with hardware T&L, and
since M16 a DirectX 9 DDI with shader model 3.0). The
register set is **v7** (`D3DPT_FB_VERSION`; v6 and v7 added only the
WDDM driver's interrupt, fence and DMA append, which this driver never
touches) and the protocol **v22**
(`D3DPT_PROTO_VERSION`). FIFA 2000, Max Payne, Diablo, Moto Racer 1997,
GTA 2 and GTA Vice City run on it with no DLL in their folders.

## Shape (M7a)

```
guest (XP)                                          host (QEMU process)
 win32k.sys (GDI)                                    hw/d3dpt/d3dpt_vga.c  "d3dpt-vga"
   └ d3dptdisp.dll  display driver (winddi.h)          ├ PCI 1234:3d00, class VGA, stdvga ROM
       GDI draws into mapped VRAM                      ├ BAR 0 VRAM 128 MiB ─── DisplaySurface points here
       DirectDraw / Direct3D DDIs                      │   (top 64 MiB: the M7c command window)
 videoprt.sys                                          ├ BAR 1 registers (d3dpt/d3dpt_fb.h)
   └ d3dptvid.sys   video miniport (video.h)           │   mode table, WIDTH/HEIGHT/BPP/PITCH/OFFSET,
       finds the PCI device, maps the BARs,            │   ENABLE, FRAMES, DEBUG (char → QEMU log)
       reads the host's mode table, sets modes         └ VGA core (hw/display/vga.c) while ENABLE = 0:
 vga.sys / VGA BIOS until the driver is installed         BIOS text, vga.sys before install, BSODs
                                                     embed listener: same surface pointer → player
```

The sources are in `guest-tools/src/d3dptvid/`. The NT layer `nt/`
(miniport `d3dptvid.c`, display DLL `d3dptdisp.c`, INF) sits over `core/`
(DP2 walker, surface table, caps, flip chain), which Win98 shares.

- **The adapter is a real VGA**: QEMU's standard VGA core with the Bochs
  VBE ports. SeaBIOS' `vgabios-stdvga.bin` boots it (the ROM takes BAR 0
  as the LFB: it patches the ROM's PCI ids to ours), and XP's inbox
  `vga.sys` runs the desktop in the ROM's VESA modes before our driver is
  installed (a fresh install at 640×480×32; `xp-driver-test.sh vesa`
  changes it to 800×600×32). Blue screens and
  shutdown text work because `HwResetHw` returns FALSE and videoprt's
  int10 puts the core back into mode 3. The id is 1234:3d00 (the
  QEMU/Bochs pseudo vendor, our device id), matched as
  `PCI\VEN_1234&DEV_3D00`.
- **The register BAR is the paravirtual part** (`d3dpt/d3dpt_fb.h`, one
  header for the device and both drivers). The host owns the mode table:
  the miniport reads MODE_COUNT and walks MODE_SEL → MODE_W/H/BPP/HZ, so
  the host decides what Display Properties offers. The table is a static
  list of 7 sizes (640×480 to 1600×1200) × {60, 75, 85} Hz × {8, 16, 32}
  bpp = 63 modes. A mode switch writes WIDTH/HEIGHT/BPP/PITCH/OFFSET and
  ENABLE = 1; ENABLE = 0 hands the console back to the VGA core. DEBUG
  takes one character per write, and the device prints whole lines to
  the QEMU log (`d3dpt-vga: guest: …`). It is the kernel code's only
  debugger, and enough.
- **No copy inside QEMU.** While ENABLE is set, the device creates its
  `DisplaySurface` over the VRAM bytes at the guest's offset and pitch
  (`qemu_create_displaysurface_from`). Dirty lines come from the memory
  dirty log (`DIRTY_MEMORY_VGA`, as in `bochs-display`), and the embed
  listener hands that pointer plus the dirty rectangles to the player.
  The listener takes one format, so 8 and 16 bpp modes (and a 32 bpp one
  under a gamma ramp) are converted per dirty span into an x8r8g8b8
  shadow. The player's upload of the dirty rectangle is the one copy
  left; zero-copy from guest RAM to the GPU is impossible with a
  host-visible-only import (M3's dma-buf ring is for host-rendered 3D).
  `-device d3dpt-vga,full-frames=on` converts the whole frame every
  refresh, the A/B for a picture in stale bands (an open thread in
  `docs/00-status.md`).
- **Mode switches do not flash.** A switch is RESET then SET_MODE a few ms
  apart. After ENABLE goes 0 the device holds the last linear frame for
  250 ms of wall-clock time before it shows the VGA core
  (`D3DPT_FB_VGA_GRACE_MS`). Counted in refreshes, the hold lasted 45 s
  headless, where an idle console refreshes every 3 s. The miniport
  zeroes the new mode's frame buffer unless `VIDEO_MODE_NO_ZERO_MEMORY`
  is set. A 16 bpp desktop's held frame is the shadow copy, so VGA writes
  to VRAM offset 0 before ENABLE goes 0 land in it (doc 19 §35). While
  the linear mode is off, the device logs the VGA core's mode registers
  once per change (`d3dpt-vga: vga core cr1=… sr4=…`), which is how a
  headless run tells Mode X, mode 13h and a text-mode blue screen apart.
- **The display driver has the DDK "framebuf" shape.** `DrvEnablePDEV`
  picks the miniport mode that matches the DEVMODE and fills
  GDIINFO/DEVINFO. `DrvEnableSurface` sets the mode, maps VRAM and gives
  GDI a surface over it with no drawing hooks, so GDI draws every pixel
  in software straight into guest VRAM. `DrvAssertMode(FALSE)` resets to
  VGA for full-screen consoles and the logon desktop switch. The register
  page reaches the display driver through
  `IOCTL_VIDEO_QUERY_PUBLIC_ACCESS_RANGES`.
- **Install** is `DRIVER\DRVINST.EXE [-reboot]` on the guest-tools ISO
  (`SETUP` runs it). It sets the driver-signing policy to ignore, then
  calls newdev's `UpdateDriverForPlugAndPlayDevices` with
  `INSTALLFLAG_FORCE` for the hardware id (what `devcon update` does).
  XP SP3 shows the "has not passed Windows Logo testing" dialog anyway,
  because the stored policy is hash-protected and only the Control Panel
  changes it. It comes once per unsigned file, minutes apart under TCG,
  and a watcher thread in DRVINST presses each one's first button
  (`alt+c` by hand). `SETMODE.EXE [w h bpp [hz]]` lists or switches modes
  for scripts. After the restart XP starts at 640×480×32@60 and remembers
  what is set.
- XP runs far faster under **KVM** (`-accel kvm -cpu pentium3`) than
  under TCG; Linux hosts should use it. TCG is the Apple Silicon path.

### The register set and its versions

| Version | Adds |
|---|---|
| 1 | mode table, linear mode, ENABLE, FRAMES, DEBUG, DDFLAGS |
| 2 | the Direct3D command window: CMD_OFFSET, DOORBELL, D3D_STATUS |
| 3 | 8 bpp: the 256-entry PALETTE block (0x400), `CAP_BPP8` |
| 4 | the hardware cursor: CURSOR_* |
| 5 | gamma: the GAMMA block (0x800), GAMMA_ENABLE (0xb4), `CAP_GAMMA` |

The BAR has `valid.min_access_size = 4`, so every access is 32-bit,
which matters to the 16-bit 9x driver (doc 19).

### Newer register sets are accepted

The miniport, the 9x display driver and the mini-VDD accept any version
at or above their own and compare `D3DPT_FB_MAGIC` exactly. Each version
has only added registers and a CAP bit, so a newer adapter is the old one
plus registers the driver never touches. `d3dpt_fb.h` states the rule: a
bump only adds, and a change that must reinterpret a register gets a new
MAGIC. This lets an installed guest survive a QEMU update.

Drivers built before 2026-09-12 want the exact version. On XP such a
driver refuses the adapter and the desktop comes up on plain VGA; a
Windows 98 image whose `SYSTEM.INI` says `*DisplayFallback=0` dies at
boot with "Windows protection error". Boot such a machine on the Cirrus
once and run the ISO's `SETUP /ALL`. `-device d3dpt-vga,fb-version=N`
makes the adapter report another version, which is how the acceptance
is checked.

### Device properties

| Property | Effect |
|---|---|
| `ddflags=N` | the DDFLAGS register: feature A/B bits for the driver, no reinstall ("The ddflags bits" below) |
| `no-exec=on` | D3D_STATUS answers `NO_EXEC`: a host with no executor at all |
| `d3d9=auto\|dxvk\|system` | the Direct3D 9 the executor runs on; `system` is Windows' own (ADR-007's second amendment) |
| `exec=wine` | the executor in a Wine process on the host (ADR-018, M15, doc 14) |
| `fb-version=N` | the register set version reported |
| `full-frames=on` | whole-frame conversion every refresh |
| `irq=on` | an interrupt pin (INTA) for Windows 7's WDDM driver (M18), which dxgkrnl will not start without one, and the IRQ registers behind it (register set v6, `CAP_IRQ`): the vertical blank at the refresh in `HZ`, on the guest's clock, while the driver enables it; off (the default) keeps the PCI config XP, 9x and snapshots know |

The executor properties model hosts, not devices. The adapter only hands
them to the loader (`d3dpt/hw/d3dpt_exec_load.c`).

- **`no-exec=on`** models a Linux or macOS host below the Vulkan 1.3
  floor with no Wine (`-global d3dpt-vga.no-exec=on` in the machine
  form's Extra QEMU arguments, or `NO_EXEC=1 tools/xp-driver-test.sh`).
  The driver keeps its whole DirectDraw half (modes, flip chain, cursor,
  gamma, palette) and offers no Direct3D, so a game falls back to the
  runtime's software device (doc 04). The loader refuses before it opens the executor
  library, so it tries no backend. The QEMU log says `d3dpt: no-exec=on:
  no Vulkan 1.3 device on this host`, the driver `d3dptdisp: no
  Direct3D executor on the host`. **Do not use `ddflags=0x20`**
  (`DDF_NO_D3D`) for this: that bit makes the driver decide without
  reading D3D_STATUS, so it proves nothing about a real below-floor
  host. The 9x counterpart is doc 19 §40.
- **`exec=wine`** models a below-floor host that has Wine; the guest
  sees READY. On a host with Vulkan it is the A/B (`EXEC=wine
  tools/xp-driver-test.sh`).
- **`d3d9=`** reaches the executor as `D3DPT_D3D9`; an explicit
  `D3DPT_D3D9` in the environment wins, which is how the two backends
  are compared on one Windows host (doc 14 "The executor and its
  Direct3D 9"). The log names the library opened and the adapter that
  answered.

## Building kernel-mode PE files with GCC

`guest-tools/build-driver.sh` builds the drivers; `build-wrappers.sh`
runs it and stages the result as `DRIVER\` on the ISO. mingw-w64 ships
the DDK headers and import libraries under its permissive licence, and
nothing from Microsoft's DDK is used.

- Flags are `-nostdlib -shared -ffreestanding -fno-stack-protector
  -mno-stack-arg-probe -fno-asynchronous-unwind-tables`, subsystem native,
  image base 0x10000, OS/subsystem version 5.1. The entry is
  `_DriverEntry@8` for the miniport and `_DrvEnableDriver@12` for the
  display DLL, exported undecorated through a .def with `--kill-at`.
  `-lgcc` supplies helpers.
- The miniport links `libvideoprt.a` plus the few ntoskrnl imports of the
  cached VRAM mappings (below). The display DLL links only `libwin32k.a`.
  The build script fails on any other import.
- GCC emits `memcpy`/`memset` calls for struct copies even when
  freestanding, and neither port driver exports them. `kcrt.c` carries
  byte loops, compiled with `-fno-tree-loop-distribute-patterns` so GCC
  does not turn them back into calls to themselves.
- Header sets do not mix. The miniport takes `ntdef.h`, `dderror.h`,
  `devioctl.h`, `ddk/miniport.h`, `ntddvdeo.h`, `ddk/video.h`, and
  **not** `ntddk.h`, which conflicts with `miniport.h`. Leave
  `_WIN32_WINNT` at the default: with `0x0501` mingw's `wdm.h` does not
  compile. The display DLL takes `windef.h`, `wingdi.h`, `winddi.h`,
  `devioctl.h`, `ntddvdeo.h`.
- mingw's `winddi.h` includes `ddrawint.h` and `d3dnthal.h`, which mingw
  does not ship. `guest-tools/src/d3dptvid/ddk/` vendors ReactOS'
  public-domain `ddrawint.h` (+ `dvp.h`) and a self-contained
  `d3dnthal.h`: the DDK's pulls in the user-mode `windows.h` through
  `d3dtypes.h`/`d3dcaps.h`, so ours spells out the few Direct3D types the
  DDI structures use, with the DDK's layouts. Hand-transcribed constants
  are where this goes wrong silently. The `DDBD_*` bit-depth flags count
  down (`DDBD_16` 0x400, `DDBD_24` 0x200, `DDBD_32` 0x100); the other way
  round, `CreateDevice` failed with `DDERR_INVALIDPIXELFORMAT` at 32 bpp
  only, because the runtime checks the target against
  `dwDeviceRenderBitDepth`. Only the host interprets the DP2 token
  layouts (`d3dpt/exec/d3dpt_exec_ddi.cpp`).
- The drivers keep the wrappers' `-march=pentium3` floor and ISA/UCRT
  checks.

## The DirectDraw DDI (M7b)

This is what `ddraw.dll` → `dxg.sys` sees behind the display driver.

- **The primary is a device surface GDI still draws on.** The driver
  calls `EngCreateDeviceSurface` + `EngModifySurface(hsurf, hdev,
  HOOK_SYNCHRONIZE, MS_NOTSYSTEMMEMORY, dhsurf, pvScan0 = VRAM, pitch)`
  with a no-op `DrvSynchronizeSurface`. Without `HOOK_SYNCHRONIZE`
  win32k refuses the `EngModifySurface`. The driver tries the variants in
  order and logs the one that took; an engine bitmap is the desktop-only
  fallback.
- **VRAM behind the primary is one linear heap.** `DrvGetDirectDrawInfo`
  reports it as `VIDMEM_ISLINEAR`, from the primary size rounded to 4 KiB
  up to 16 KiB below the command window, where the cursor image lives.
  dxg's heap manager places every DirectDraw surface; the driver never
  allocates.
- **The callbacks.**
  - `DdMapMemory` maps VRAM into the game's process through the
    miniport's `IOCTL_VIDEO_SHARE_VIDEO_MEMORY`.
  - `DdCanCreateSurface` accepts the formats the host mirrors.
  - `DdFlip` writes the target's VRAM offset into OFFSET once: a real
    page flip that copies nothing.
  - `DdWaitForVerticalBlank`, `DdGetFlipStatus` and
    `GetVerticalBlankStatus` run on FRAMES (next section).
  - `DdGetBltStatus` always answers done.
  - `DdLock` / `DdUnlock` handle the Direct3D readback and dirty
    tracking.
  - `DdCreateSurface` sizes compressed textures, volumes and buffers
    (M7c).
  - `DdBlt` is registered and never called.
  - `GetDriverInfo` answers `GUID_NTCallbacks` (SetExclusiveMode,
    FlipToGDISurface) and the Direct3D GUIDs, and refuses the rest.
- **Mappings are cached, not write-combined.** videoprt's
  `VideoPortMapMemory` maps frame buffers uncached or write-combined,
  which suits a PCI aperture, but this VRAM is RAM, and GDI scrolls,
  the HEL's copies and every `Lock` read from such a mapping at tens of
  MB/s. The miniport therefore maps VRAM itself:
  `MmMapIoSpace(MmCached)` for the kernel view GDI draws through, and a
  cached view of `\Device\PhysicalMemory` (`ZwMapViewOfSection`, what
  videoprt does minus `PAGE_NOCACHE`) for DirectDraw's user-mode `Lock`.
  The register BAR stays an uncached videoprt mapping. QEMU reads guest
  RAM coherently, so a cached mapping is correct under KVM and TCG. DDTEST's
  windowed offscreen → primary `Blt` went from 29.9 to 305 fps, and the
  16 bpp flip chain from 31 fps to thousands.
- **Test.** `DRIVER\DDTEST.EXE [w h bpp] [frames] [-windowed]` logs the
  caps, then runs an exclusive flip chain (Lock/Unlock pattern + `Blt`
  colour fill + `Flip`) or a windowed offscreen surface blitted to the
  primary, and writes fps to `ddtest.log` plus a `.bmp`. `tools/
  xp-driver-test.sh <image> ddtest` runs 8, 16, 32 bpp and windowed.
  Every surface reports `VIDEOMEMORY | LOCALVIDMEM`, and the QEMU log
  shows `scanout offset 0 -> 614400 -> 0 …` per flip.

### dxg's caps rules

dxg validates the HAL after enabling it. On any of the following it
drops the whole HAL with no message anywhere: `GetCaps` answers
`DDCAPS_NOHARDWARE`, surfaces go to system memory, and there is no
Direct3D. The rules came from `ddflags` bisection and from disassembling
the image's `dxg.sys` / `ddraw.dll` ("Debugging the driver").

- `DDCAPS_GDI` in `dwCaps` (`ddflags=0x10` is the repro), and likewise
  `DDCAPS_BANKSWITCHED` and `DDSCAPS_MODEX`.
- `ddCaps.dwPalCaps != 0`, or palette callbacks ("8 bpp palettized
  modes").
- Colour-key caps without a `Blt` callback ("Palettized textures and
  colour keying"; `ddflags=0x20000`).
- Direct3D appearing or disappearing across a mode switch, which makes
  the object rebuild after `SetDisplayMode` fail. The driver therefore
  offers Direct3D in every mode, 8 bpp included; the runtime refuses
  `CreateDevice` on a palettized primary by itself.
- A device claiming `DRAWPRIMITIVES2EX` or `HWTRANSFORMANDLIGHT` without
  a `GetDriverState` callback ("The Direct3D DDI (M7c)").

`dwCaps` also claims no blit caps (`DDCAPS_BLT` and friends): the HEL
does every blit in user mode on the cached VRAM mapping. "Blit caps and
the HEL" explains why that is not a choice.

### The flip chain's vertical blank

Titles of the era pace themselves by the flip chain: on a real card the
second `Flip` of a double-buffered chain waits until the first has been
scanned out, and that wait, not a timer in the game, holds a 1997 racer
at 60 frames a second. Without it Moto Racer ran several times too
fast.
**A game that runs far too fast is missing a frame limiter, not a clock
bug.**

- **The device (`d3dpt_vga.c`, `FRAMES`)** counts periods of the mode's
  `HZ` since `ENABLE`, on the host clock (60 Hz if `HZ` is unset). It
  must not follow whoever pulls frames, because a headless run has no
  display client and never calls `graphic_hw_update`.
- **The driver (`core/core_flip.c`).** `DdFlip` remembers `FRAMES` at the
  flip, and the flip is in the air until that value moves. Meanwhile a
  second `DdFlip` waits (`DDFLIP_WAIT`) or returns
  `DDERR_WASSTILLDRAWING`, and `DdGetFlipStatus` answers
  `DDERR_WASSTILLDRAWING` for both `DDGFS_CANFLIP` and
  `DDGFS_ISFLIPDONE`. Every wait is bounded at 50 ms, so a device that
  stops counting cannot stop the guest, and pauses between polls,
  because the register read and XP's performance counter are both exits.
- **`GetVerticalBlankStatus`** (`WaitForVerticalBlank(DDWAITVB_I_TESTVB)`)
  must sometimes say yes, or a title's `while (!in_vb)
  GetVerticalBlankStatus(&in_vb);` never ends. The adapter has no beam
  position, so `vb_test` says yes once a frame, to the first question
  after `FRAMES` moved: `while (!in_vb)` ends at the next frame and
  `while (in_vb)` at the next question. DDTEST and `ddprobe` count the
  answers in a 500 ms loop (about 30 at 60 Hz).

With the vertical blank, DDTEST's 8/16/32 bpp chains and `D3D7TEST` run
at 60 fps, and D3D7TEST's frame is still byte-identical to the host
oracle. A windowed blit to the primary is not a flip and is not
throttled, as on real hardware. `ddflags=0x8000` (`DDF_NO_VSYNC`) makes
flips complete instantly: the A/B for a suspect title and how throughput
is measured (DDTEST 640×480 under KVM: 3846 fps at 16 bpp, 4762 at 32,
1132 at 8 with a palette every frame).

While flips happen, the device prints the guest's frame rate every 5 s
(`d3dpt-vga: 299 page flips in 5.0 s (59.6/s)`). The flips drive the
line, so headless and player runs agree. **If no such line appears while
a game runs, the game blits to the primary instead of flipping**; the
vertical blank cannot pace it, and if it runs too fast the cause is the
guest CPU, not the display path. `FRAMES` is a clock, not the player's
present: a game gets the refresh it asked for, but not in phase with the
host's swapchain (open item).

### A DirectX 6 title's flip chain

GTA 2 (1999, `IDirectDraw4` / `IDirect3D3`, 640×480×16) exposed these
rules; all of them apply to every title.

- **A flip swaps roles, not memory.** `DdFlip` gets `lpSurfCurr` (front)
  and `lpSurfTarg` (back), and the driver scans out `Targ`'s memory.
  Afterwards dxg does not exchange the two objects' `fpVidMem`: the
  handles keep their VRAM, `DDSCAPS_PRIMARYSURFACE` moves to the object
  now displayed, and the application's `back` pointer means the other
  object. dxg tells the driver with a `CreateSurfaceEx` pair (same
  offsets, swapped caps), and the runtime's `SETRENDERTARGET` alternates
  the handle (2, 1, 2, 1 …) to match. **Never re-register the chain in
  `DdFlip`.** Swapping the offsets there made the host render into the
  displayed buffer on alternate frames, and a `Lock` of the back buffer
  found black or the previous frame; a golden compare of identical frames
  cannot see this. `DdFlip` logs its first eight calls (`d3dptdisp: flip
  curr H at OFF targ H at OFF`). Should a runtime ever swap memory,
  `DdFlip` and a `DdLock` of a target re-register a surface the host
  knows at another offset (`d3d_register_moved`, silent), and the host
  drops a moved surface's shadow ("Untracked writes") so the first
  readback is whole.
- **The back buffer may never be announced.** A DirectX 7 interface's
  chain gets one `CreateSurfaceEx` per member; GTA 2's DX6 chain arrived
  as its root alone, so the host did not know `SETRENDERTARGET 2` and
  every other frame showed the buffer nobody drew. `DdCreateSurfaceEx`
  walks the surface's attach list (`d3d_register_chain`: the ring of a
  flip chain and an attached Z buffer, while mip levels stay with their
  texture) and registers each member the host does not know, or knows at
  another offset. `D3dContextCreate` and `D3dSetRenderTarget` do the same
  for their targets, as the DDK samples do.
- **Contexts outlive their PDEV.** A game's exclusive mode switch gives
  GDI a new PDEV, and its switch back another, before dxg's
  `ContextDestroyAll` arrives. With the context table in the PDEV no
  `CTX_DESTROY` reached the host, and `CreateDevice` failed on the game's
  second start (`CTX_CREATE` of handle 1 → `BAD_HANDLE`). The table and
  its count are globals, like the surface table, and the host replaces a
  context re-created under an open handle (`ddi: context N still open,
  replaced`).
- **The white menu text: the legacy blend.** GTA 2 picks its blend once
  with the DirectX 5 `TEXTUREMAPBLEND` render state (`MODULATEALPHA`),
  before it binds any texture. The DX6 runtime passes the state to a DX7
  driver unchanged instead of translating it into stage states. Mapped
  once, it meant "no texture, the diffuse alone", and the glyphs drew as
  white boxes. The executor keeps the legacy blend in effect as two
  flags, `legacy_cop` / `legacy_aop`: `TEXTUREMAPBLEND` / `TEXTUREHANDLE`
  set both, the app's own `COLOROP` ends the first and its `ALPHAOP` the
  second, and its ARGs end neither (Crimson Skies sets `COLORARG2 =
  DIFFUSE` after the blend, doc 19 §28). Every bind of stage 0's texture
  re-evaluates the halves still in effect. `d3dpt-dp2-test` covers both
  orders.
- Not ours: Enter during GTA 2's Bink intro kills the game (`c0000095`
  in `binkw32!BinkGetSummary`) on the Cirrus too. Skip it at the logos.

### 8 bpp palettized modes

The late-90s 2D titles (Diablo, StarCraft, Age of Empires, Caesar 3) set
640×480×8 through DirectDraw and animate the palette. Every size is
offered at 8 bpp. Four layers take part.

- **Device** (register set v3). `BPP = 8` and the 256-entry `PALETTE`
  block (x8r8g8b8, 0x400..0x7fc). The scanout is a pixman `c8` view of
  VRAM through a `pixman_indexed_t` built from the registers, converted
  into the shadow per dirty span. A palette write repaints the whole
  frame at the next refresh. Each entry write is one MMIO exit, so a full
  256-entry animation costs about 0.5 ms under KVM, fine for 20–30
  palette updates a second; a RAM-backed palette page is the fix if a
  title needs more.
- **Miniport.** 8 bpp entries carry `VIDEO_MODE_PALETTE_DRIVEN |
  VIDEO_MODE_MANAGED_PALETTE`, 8 bits per gun.
  `IOCTL_VIDEO_SET_COLOR_REGISTERS` writes the block.
- **Display driver, GDI.** It sets `GCAPS_PALMANAGED |
  GCAPS_COLOR_DITHER`, a `PAL_INDEXED` default palette (the 20 system
  colours at 0–9 and 246–255, a 6×6×6 cube, 20 greys), `BMF_8BPP`,
  `ulNumColors = 20`, `ulNumPalReg = 256`, `HT_FORMAT_8BPP`, and a
  `DrvSetPalette` that writes the registers directly. XP's desktop at
  800×600×8 comes up in the classic theme, with the wallpaper dithered
  through the system palette.
- **Display driver, DirectDraw.** `ddpfDisplay = DDPF_RGB |
  DDPF_PALETTEINDEXED8`. **`dwPalCaps` stays 0 and there are no palette
  callbacks.** On NT the primary's palette is GDI's, so
  `IDirectDrawPalette::SetEntries` on it lands in `DrvSetPalette`. The
  DDK header's `DdCreatePalette` / `DdSetEntries` / `DdSetPalette` are
  dead on XP, and claiming them drops the HAL. `DDPCAPS_8BIT |
  DDPCAPS_ALLOW256` palettes work without any of them.
- **Test.** `DDTEST 640 480 8 300` rotates a four-ramp palette by one
  entry per frame and writes `ddtest.bmp` through the palette.
  `tools/xp-diablo.sh install|play` takes the retail Diablo from its
  installer to Tristram at 640×480×8 (dungeon, sound and TCG timing not
  run).

A 320×200 game on Win98 wants DirectDraw's own Mode X, not a driver
mode, so no 320-wide mode is listed (doc 19 §30).

### The hardware cursor

Without pointer hooks, GDI paints a software pointer into VRAM, erasing
and redrawing it around every drawing operation, always into the GDI
primary. Under a flip chain that is one buffer of the two, so the
pointer blinks every other frame in a full-screen title. Register set v4
adds the sprite every card of the era had.

- **Driver.** `DrvSetPointerShape` takes GDI's pointer (a 1 bpp AND/XOR
  mask, or a colour surface with a translation object; `SPS_ALPHA` marks
  32 bpp with alpha), writes it as a8r8g8b8 into the 16 KiB above the
  DirectDraw heap, then writes CURSOR_ADDR / W / H / HOT_X / HOT_Y and
  CURSOR_DEFINE. For monochrome pointers, AND 1 + XOR 0 is transparent,
  AND 0 is black or white by XOR, and AND 1 + XOR 1 ("invert") becomes
  black, since a sprite cannot invert. Colour pointers in another format
  go through a 32 bpp engine bitmap and `EngCopyBits`. The driver
  declines (`SPS_DECLINE`) anything over `D3DPT_FB_CURSOR_MAX` (64), and
  GDI keeps those. `DrvMovePointer` writes CURSOR_X / Y and
  CURSOR_ENABLE (x = −1 hides). The driver sets `GCAPS_ASYNCMOVE`, and
  `DrvAssertMode(FALSE)` hides the sprite before the VGA text.
- **Device.** CURSOR_DEFINE reads the image into a `QEMUCursor` for
  `dpy_cursor_define`; X / Y / ENABLE writes become `dpy_mouse_set`.
  Nothing is composited, so a QMP screendump shows no cursor, as with a
  real sprite. The log has the first defines and moves. The device
  reports the sprite hidden whenever ENABLE is 0 (every ENABLE write
  re-runs `fb_cursor_move`), so the player draws no Windows pointer over
  full-screen DOS games, XP's console or blue screens (doc 19 §29), and
  a reset drops the shape. **Never call
  `dpy_cursor_define(con, NULL)`**: QEMU's console takes a reference on
  the cursor unconditionally (`cursor_ref`), so NULL segfaults the
  player. "No cursor" is a hidden 1×1 transparent one
  (`fb_cursor_clear`).
- **Player.** With the USB tablet, the host window's cursor takes the
  guest's shape over the image and hides when the guest hides it. With a
  relative mouse (PS/2, the player's grab) the player composites the
  sprite into the frame, and a move alone republishes it; the headless
  dump takes that path, so a `PLAYER_DUMP_OUT` frame shows the cursor. A
  guest without a hardware cursor (Cirrus, vga.sys) keeps the software
  pointer in the frame.
- **Hidden behind a flip chain.** Windows keeps its pointer enabled
  behind an exclusive-mode game that never hides it, and its arrow was
  drawn over Moto Racer's own. The adapter hides the sprite from a flip
  chain's first page flip, whatever CURSOR_ENABLE says
  (`cur_flip_hidden`); a page-flipping game draws its own pointer, and
  the first flip would wipe GDI's anyway. The sprite comes back at the
  next mode set, or after 2 s of guest time with no flip while the
  desktop's page is on screen, since a game at the desktop's own mode
  sets no mode on the way out (3DMark 99 left the pointer hidden for
  good). A game idle on its other page stays hidden; a loading screen on
  the desktop's page shows the pointer until its next flip. While a Voodoo 2 has the monitor the
  cursor is hidden too (patch 66). The adapter does all this, so both
  driver families get it; the `voodoo-guest-d3dpt` check guards it.

### Gamma ramps

Register set v5 adds a ramp where a RAMDAC would have one.

- **The adapter** has a 256-entry x8r8g8b8 `GAMMA` block (0x800),
  `GAMMA_ENABLE` (0xb4) and `D3DPT_FB_CAP_GAMMA`. It builds the tables
  at the `GAMMA_ENABLE` write, not at the next refresh, which a headless
  run never makes. The ramp is applied per dirty span to the shadow the
  8/16 bpp modes convert into. A 32 bpp mode moves onto a shadow only
  while the ramp changes anything, so the identity ramp GDI loads at
  every mode set costs no copy. VRAM never holds ramped pixels:
  `GetFrontBuffer` and every readback see the pixels as drawn, a
  screendump and the player see the ramp. The log says `gamma ramp on` /
  `off`, and a reset turns it off.
- **The XP driver** implements `DrvIcmSetDeviceGammaRamp` with
  `GCAPS2_CHANGEGAMMARAMP`, where GDI's `SetDeviceGammaRamp`,
  DirectDraw's gamma control and Direct3D 8's `SetGammaRamp` all arrive.
  It writes the high byte of each of the three 256-word ramps, then
  `GAMMA_ENABLE`, and claims `DDCAPS2_PRIMARYGAMMA`. The core claims
  `D3DCAPS2_FULLSCREENGAMMA` only when its layer loads ramps
  (`d3dpt_core.gamma`); Windows 98's layer has no ramp path yet.
  `ddflags=0x10000000` (`DDF_NO_GAMMA`) takes it out; the GDI cap then
  stays and its entry refuses.
- **Test.** `GAMMATEST` holds a mild ramp over a mid-grey frame (blue at
  3/4, because GDI range-checks ramps). `tools/xp-driver-test.sh <image>
  gamma` waits for `gamma ramp on`, takes a screendump, waits for `off`
  and takes another. The centre pixel reads 80 80 60, then 80 80 80.

### Untracked writes by GDI on a DirectDraw surface

`VRAM_DIRTY` announces the guest's writes to a render target's VRAM
(from `DdUnlock` and the driver's own copies). The host uploads a dirty
target before drawing and reads the host frame back into VRAM at
EndScene, Lock and Flip. Two kinds of guest write never pass through
`DdLock` / `DdUnlock`:

- **`GetDC` on a DirectDraw surface** is `NtGdiDdGetDC` → dxg, which
  builds a GDI surface over the DirectDraw surface's memory. dxg asks a
  driver that exports `DrvDeriveSurface` for that surface; ours does not,
  so GDI's `TextOut` / `BitBlt` write straight into VRAM with no driver
  callback on either side.
- **The engine's software cursor**, painted into the primary (576 pixels
  per move at 24×24). In a flip chain that is the next frame's back
  buffer.

Moto Racer draws its menu panels and HUD text the first way, and the
readback overwrote them every frame. `DrvDeriveSurface` would not
fix it: GDI does not say when it is done with a derived surface, so a
readback could overtake a `TextOut` in progress. Instead the executor
keeps a shadow of each render target's VRAM as of the last moment host
and VRAM agreed (after every upload and readback), and uses it twice a
frame:

1. Before the frame's first draw (`bind_ctx`), it compares a target that
   is not dirty with its shadow. A difference is an unannounced guest
   write, and the target is uploaded as if announced.
2. At the readback, pixels that differ from the shadow are the guest's
   writes since the draws began. They are kept over the host frame, and
   the target is marked dirty so the next frame starts from them.

A full-target `Clear` skips the check as it skips the upload. The cost
is one `memcmp` of the target per frame (per row first, per pixel only
on rows that differ) and, while a title writes after its scene, one
upload per frame; Moto Racer stays at 120 frames/s under KVM. The device
log counts these writes (`ddi: … N untracked guest pixels in 5.0 s`,
plus the first eight events). `DrvDeriveSurface` stays on the list as an
optimisation, not a correctness item.

### Blit caps and the HEL

**On XP a declined `DdBlt` does not fall back to the HEL.** With blit
caps claimed (`DDCAPS_BLT | BLTSTRETCH | BLTCOLORFILL …` in any of the
caps sets), dxg routes every blit to the driver, and a
`DDHAL_DRIVER_NOTHANDLED` reaches the application as `E_NOTIMPL`
(DDTEST's windowed `Blt(DDBLT_COLORFILL)` failed with 0x80004001).
Whatever the 9x DDK says, claiming blit caps on NT means writing the
blitter (copy, colour fill, stretch, source colour key, ROP) in the
display DLL or on the host. The HEL's user-mode copies on cached VRAM
are fast enough for the 2D titles tried so far, so the driver claims
none. The `Blt` callback is still registered, because colour-key caps
need one; without `DDCAPS_BLT` nothing reaches it, and it logs any call
with its rectangles. `DDF_CKEY_NOBLTCB` leaves it out. Windowed
multisampling is the one feature that waits on a blitter
("Multisampling").

**The one blit cap claimed: `DDCAPS_BLT` in `dwSVBCaps`** (M16, with
Direct3D on). It is how d3d8.dll decides that the driver does
`UpdateTexture` from system memory: with it the runtime sends a DP2
`TEXBLT` / `VOLUMEBLT`, without it the runtime copies level by level
itself, and that copy faults when the source has fewer levels than the
target (track M16 finding 2). With no system-to-video ROPs, colour-key
or FX caps, DirectDraw's own system-to-video blits stay in the HEL:
DDTEST's `sysmem blt` line (Blt, BltFast, a stretched Blt, a keyed
BltFast, each read back) passes at every depth and no call reaches
`DdBlt`.


## The Direct3D DDI (M7c)

This is the DX7 HAL behind the display driver, on the doc 14 protocol
and executor. The adapter's top 64 MiB of VRAM is a command window in
the layout doc 14's retired SysBus device used (`d3dpt_proto.h`: header
page, records, return area), so the guest encoder `d3dpt_enc.h` and `d3dpt_exec_submit`
work unchanged. DOORBELL submits the window, and the host runs the batch
synchronously inside the write. D3D_STATUS says whether the host has an
executor (reading it loads the library); CMD_OFFSET says where the
window is. The executor library is loaded once per process, and each
device has its own instance.

```
guest (XP)                                      host
 ddraw.dll / d3dim.dll / d3d8.dll                d3dpt-vga: VRAM [heap | 64 MiB window]
   └ dxg.sys ── DDI ──> d3dptdisp.dll              DOORBELL ──> libd3dpt_exec (d3dpt_exec_ddi.cpp)
        CreateSurfaceEx → VRAM_SURFACE record        surface handle → DXVK texture / render target
        ContextCreate  → CTX_CREATE                  d3d9 device, SetRenderTarget + depth
        DrawPrimitives2 → DP2 record (tokens+verts)  token interpreter → Draw*PrimitiveUP etc.
        SceneCapture END / Lock / Flip → READBACK    GetRenderTargetData → memcpy into VRAM
        Unlock → VRAM_DIRTY                          texels re-read from VRAM before the next use
```

- **Surfaces stay in guest VRAM.** dxg's heap allocates every surface
  below the window. `DdCreateSurfaceEx` (`GUID_Miscellaneous2Callbacks`)
  registers each by `dwSurfaceHandle` with VRAM offset, size, pitch,
  D3DFORMAT and caps (mip chains send every level's offset). The host
  creates the DXVK object lazily: a MANAGED texture filled from the VRAM
  pointer, a render target, or a depth surface (D16/D24X8/D24S8, with
  fallbacks). `DdUnlock` of a texture or target sends `VRAM_DIRTY`.
  `DdDestroySurface` returns `NOTHANDLED` so dxg frees the block, and
  sends `VRAM_RELEASE`. The driver logs every surface it registers
  (`d3dptdisp: surface <handle> caps … w h fmt at …`), so an unknown
  handle in the executor's log can be looked up.
- **Rendering goes to a host render target, and READBACK brings it
  back.** A context (`D3dContextCreate`) is a render-target + Z pair, and
  the d3d9 device is created with the first one. The executor copies the
  frame into the target's VRAM at `SceneCapture END`, at `DdLock` of a
  target, and in `DdFlip` before the OFFSET write, so flips, HEL blits,
  GDI and screenshots see it. It skips the copy when nothing was drawn
  since the last one. A full `Clear` of the target skips the upload of
  stale VRAM; a partial one, or a draw onto a HEL-blitted background,
  uploads it first. Per frame that is one `GetRenderTargetData` + memcpy
  (1.2 MB at 640×480×32).
- **The GPU starts on a frame before its readback** (track M17). The
  executor never presents, and DXVK submits recorded work on its own only
  once three command chunks have piled up (`EndScene`'s hint, at the end
  of every batch). The rest of a frame waited for the readback, which then
  waited for the GPU to draw it. After every DP2 record that brings the
  count to `flush_draws` draws since the last hint (16), the executor
  issues an EVENT query and polls it with `D3DGETDATA_FLUSH`, which
  submits whenever the GPU is short of work. In Max Payne 2's corridor
  the readback's wait fell from 283 to 116 ms in every 5 s and the frame
  rate rose 4.7 % (tools/w98-mp2.sh); 4 draws instead of 16 changed
  nothing. `D3DPT_DDI_FLUSH_DRAWS=n` sets it (0 = off). The rate line
  reports the readbacks' time and the part spent waiting for the frame.
- **DrawPrimitives2** copies the runtime's command buffer and vertices
  into one `DP2` record and rings the doorbell.
  `D3DHALDP2_USERMEMVERTICES` is a user pointer, read in the caller's
  context. The driver mirrors RENDERSTATE tokens into the runtime's
  `lpdwRStates`. The host interprets the tokens on `IDirect3DDevice9`:
  - render and stage states (DX7→d3d9 filter renumbering, address /
    filter / LOD states → sampler states);
  - VIEWPORTINFO + ZRANGE, re-applied after every target change because
    d3d9 resets them;
  - SETRENDERTARGET and CLEAR;
  - the draw tokens → `DrawPrimitiveUP` / `DrawIndexedPrimitiveUP` over
    the touched range;
  - SETMATERIAL / SETLIGHT / SETTRANSFORM (WORLD renumbered), STATESET,
    TEXBLT and palettes.

  A malformed stream answers `D3DERR_COMMAND_UNPARSED` with
  `dwErrorOffset`; an out-of-range vertex reference skips the primitive.
  The DX7-only states without a d3d9 twin (4, 10, 30, 33, 40:
  TEXTUREPERSPECTIVE, LINEPATTERN, ZVISIBLE, STIPPLEDALPHA,
  EDGEANTIALIAS…) are logged once and dropped. ZBIAS (47) is mapped:
  each of its 0..16 steps is DEPTHBIAS −1/65535, the scale DXVK's own d3d8
  layer uses (`d8caps::ZBIAS_SCALE`).
- **Caps.**
  - `lpD3DGlobalDriverData`: FLOATTLVERTEX, DRAWPRIMITIVES2 + 2EX,
    HWRASTERIZATION, TEXTUREVIDEOMEMORY; Z, all blends and compares,
    Gouraud + specular, fog, point/linear/mip filters, every address
    mode; the texture formats the host mirrors.
  - `lpD3DHALCallbacks`: ContextCreate/Destroy/DestroyAll, SceneCapture.
  - `GUID_D3DCallbacks3`: Clear2, ValidateTextureStageState,
    DrawPrimitives2.
  - `GUID_D3DCallbacks2`: SetRenderTarget.
  - `GUID_D3DExtendedCaps`: 4096² textures, 8 stages, all texture ops,
    stencil, `dwMaxTextureAspectRatio` 4096.
  - `GUID_ZPixelFormats`: D16, D24X8, D24S8.
  - `DDCAPS_3D` + `DDSCAPS_3DDEVICE|TEXTURE|ZBUFFER|MIPMAP`.

  `DdCanCreateSurface` accepts exactly the formats the host mirrors.
- **`GetDriverState` is mandatory.** After validating the
  `GUID_Miscellaneous2Callbacks` table, `ddraw.dll` checks
  `hwCaps.dwDevCaps & (D3DDEVCAPS_DRAWPRIMITIVES2EX |
  D3DDEVCAPS_HWTRANSFORMANDLIGHT)`. If either bit is set it requires a
  non-NULL `GetDriverState`, and otherwise builds the HEL-only object, so
  the whole HAL is gone (the DDK does not say so). `DdGetDriverState` answers DD_OK with the buffer untouched. Three more
  rules from the same disassembly: user-mode ddraw probes a mangled
  `GUID_DDStereoMode` that the driver must refuse; `dxg.sys` drops
  Callbacks3 when the driver refuses the ParseUnknownCommand query; and
  no answer may exceed `dwExpectedSize`, because the runtime checks guard
  words after the buffer.
- **Texture sizes.** `dwTextureCaps` claims `D3DPTEXTURECAPS_POW2 |
  NONPOW2CONDITIONAL` (and D3DCAPS8 with it), a GeForce's answer.
  Crimson Skies branches on `POW2`, and without it the game never made
  its menu's string textures (doc 19 §34). `ddflags=0x2`
  (`DDF_TEX_ANYSIZE`) is the A/B. **The DX9 caps claim any size** for
  2D, cube and volume textures (M16): a GeForce 6, the rig's card, claims
  none of the POW2 flags there, and a conditional claim promises a clamp
  DXVK does not do (Wine's `conditional_np2_repeat_test`). **Not on
  Win98**: 9x DirectDraw itself refuses a mipmapped surface with a width
  or height that is not a power of two, a cube map that is not a
  power-of-two square and a mip count above log2(max(w, h)) + 1 (depth
  not counted), before any driver call. `d3d9.dll` returns that as
  `D3DERR_DRIVERINTERNALERROR` (`0x88760827`) whatever the caps said. So the 9x layer sets
  `core.pow2_mips` and the DX9 caps keep the DX8 face's `POW2 |
  NONPOW2CONDITIONAL` with `CUBEMAP_POW2` / `VOLUMEMAP_POW2` (M16 finding
  33). **The host keeps the conditional promise** (v20): the driver
  flags each context whose runtime was shown the conditional claim
  (`D3DPT_CTX_NP2_CONDITIONAL` in `CTX_CREATE`), and on such a context
  the executor samples a texture of other sizes with every address mode
  clamped, whatever the app set, as those cards do (M16 finding 36).
- **A Z buffer written through a Lock.** Some titles reset depth by
  writing the Z buffer, and the HEL performs an application's depth fill
  through `DdLock` too, since the driver claims no blits. Both write VRAM
  that the host's depth buffer never reads. `DdUnlock` hands a Z buffer
  locked for writing to the core (`d3d_z_written`), which samples a
  seventh of its rows and turns a buffer of one value into a host Z
  clear (`ZFILLTEST.EXE`, doc 19 §34).
- **Tests.** `tools/d3dpt-dp2-test.cpp` (the `d3dpt-dp2` host check)
  registers a target, a Z buffer and a texture in malloc'ed VRAM, sends
  the D3D7TEST scene as the DP2 tokens the runtime would emit, checks
  pixels, and feeds hostile records the executor must refuse without
  dying; every executor fix has a case there.
  `DRIVER\D3D7TEST.EXE` draws the same scene through `IDirect3DDevice7`,
  and `tools/xp-driver-test.sh <image> d3d7` diffs its BMP against the
  host test's: 0 of 307200 pixels differ. The runtime batches a whole
  frame into one DP2 call.

### When a title falls back to its software renderer

A 1997 title asks for what a Voodoo of the day had, 8-bit palettized
textures (what fits in 4 MB) and colour keying. With either missing it
takes its software rasterizer without a word. `DdCanCreateSurface`
prints the first eight formats it refuses:

    d3dpt-vga: guest: d3dptdisp: refused pixel format, flags 0x00000020 fourcc 0x00000000 bits 0x00000008 …

`flags` bit 5 with 8 bits is `DDPF_PALETTEINDEXED8`. Read it with the
context line: `d3dptdisp: d3d context 1 …` means the game took the HAL,
and no context line means it never got that far.

### Palettized textures and colour keying

Protocol v8 gives a 1997 title the following.

- **The caps.** The DX7 texture list carries `DDPF_RGB |
  DDPF_PALETTEINDEXED8` at 8 bits and the DX8 list `D3DFMT_P8`.
  `dwTextureCaps` carries `D3DPTEXTURECAPS_TRANSPARENCY` and
  `ALPHAPALETTE`, masked out of `D3DCAPS8.TextureCaps`, where bit 3 means
  nothing. DirectDraw carries `DDCAPS_COLORKEY` with `dwCKeyCaps =
  DDCKEYCAPS_SRCBLT`, a `SetColorKey` callback and a `Blt` callback.
  CKTEST settled that shape in four runs:
  1. caps + `SetColorKey`, no `Blt`: dxg drops the whole HAL
     (`ddflags=0x20000` is the repro);
  2. no caps: `SetColorKey(DDCKEY_SRCBLT)` succeeds, but user-mode ddraw
     keeps the key to itself and never calls `DdSetColorKey`;
  3. caps + `DDCAPS_BLT` + a declining `DdBlt`: works, but claims blits;
  4. caps + the `DdBlt` callback **without** `DDCAPS_BLT`: works, and
     nothing can ever be routed to `DdBlt`. This is the driver's shape.

  `ddflags=0x10000` (`DDF_NO_CKEY`) withdraws the D3D caps, the
  DirectDraw caps, the P8 format and the key check together.
- **The key arrives two ways, and the driver keeps both.**
  `DdSetColorKey` sends `D3DPT_OP_VRAM_COLORKEY` (handle, low, high,
  on/off) when the app sets it. The DP2 walk also checks every
  `TEXTURESTAGESTATE` that binds a texture: it reads the surface's
  `DDRAWISURF_HASCKEYSRCBLT` / `ddckCKSrcBlt` off the `DD_SURFACE_LOCAL`
  the surface table remembers, and sends the record ahead of the DP2
  record when it differs from what the host was told. That covers a key
  set before the surface was mirrored. Key values are the surface's own
  pixel values (0xf81f for magenta in R5G6B5, an index for P8) and form
  an inclusive range.
- **Palettes never touch the driver.** A texture's palette reaches the
  host inside the DP2 stream as `SETPALETTE` (palette handle, flags with
  `DDRAWIPAL_ALPHA` 0x2000 when `peFlags` are alpha, surface handle) and
  `UPDATEPALETTE` (handle, start, count, entries), in DX7 and DX8 alike.
- **The host expands both to A8R8G8B8.** DXVK has no P8 and a key needs
  alpha, so a P8 or keyed texture's host object is A8R8G8B8.
  `upload_texture` converts texel by texel: the palette colour, alpha
  from `peFlags` for an alpha palette, a grey ramp when no palette is
  set, and alpha 0 inside the key range. A palette update dirties every
  texture that uses it; a key change releases the host object when its
  format changes. A bound texture can change under the runtime (a
  palette edit, a `Lock`), and the runtime re-sends `TEXTUREMAP` only on
  a `SetTexture`, so every draw first re-uploads dirty bound stages
  (`pre_draw`).
- **Keying is alpha 0 plus the alpha test, with one override.** While
  `COLORKEYENABLE` (41) is on and stage 0's texture has a key, the
  executor forces the alpha test on (`GREATEREQUAL 1`) unless the app
  runs its own, and makes stage 0's alpha op `SELECTARG1 TEXTURE` when
  the app's alpha pipeline does not read the texture. The override is
  needed because the DX7 runtime's `TEXTUREMAPBLEND` emulation sets
  `ALPHAOP = SELECTARG2 DIFFUSE` for any format without alpha (every
  keyed R5G6B5 / P8 texture), and an alpha test cannot see a key the
  pipeline threw away. The app's states come back the moment the key
  stops applying. A title that keys and fades by diffuse alpha in one
  draw loses the fade (rare under the DX7 SDK's semantics).
- **Tests.** `d3dpt-dp2-test` covers a P8 texture through a palette, an
  entry changed under a bound texture, a keyed checker with 41 on and
  off, the app's own alpha test winning, and hostile palettes.
  `DRIVER\CKTEST.EXE` runs through the DX7 API (`xp-driver-test.sh
  <image> cktest`).

### Execute buffers (the DirectX 3 path)

Moto Racer (1997) took the HAL with v8's caps and drew nothing through
it. It ships with DirectX 3 and draws through `IDirect3DDevice::Execute`:
execute buffers of `D3DOP_*` instructions (`STATERENDER`,
`PROCESSVERTICES`, `TRIANGLE`, `EXIT`), textures bound by
`D3DRENDERSTATE_TEXTUREHANDLE`, and the viewport cleared through a
background material. XP's `d3dim.dll` (the DX3–6 runtime; `d3dim700.dll`
is only `IDirect3D7`) runs that on a DrawPrimitives2 driver.
`DRIVER\EBTEST.EXE` does what such a title does and logs every HRESULT;
`-rgb` runs it on the runtime's RGB software device as the control. The
path needs the following, all from `d3dim.dll`'s disassembly (XP SP3).

- **`dwMaxVertexCount` must be 4096.** The Execute core sizes its TL
  vertex buffer as `max(dwVertexCount clamped to 4096, dwMaxVertexCount)
  × 32` bytes + 0x400, and the vertex buffer constructor refuses more
  than 0xffff vertices, reporting it as `E_OUTOFMEMORY`. A cap of 65535
  is one page over, so every `Execute` failed with 0x8007000E before a
  token reached the driver. 4096 is the runtime's own clamp.
  `dwMaxBufferSize` stays 0 (unlimited). `ddflags=0x40000`
  (`DDF_EB_MAXVERT_65535`) is the repro. The DX5+ interfaces never
  consult the field.
- **The path is a pass-through with a bounce, not a translation.** In
  the UNCLIPPED mode the core hands every run of driver instructions to
  `DrawPrimitives2` as they are: `dwFlags` is `D3DHALDP2_EXECUTEBUFFER`
  (0x2), `lpDDCommands` the app's execute buffer, `dwCommandOffset` the
  current instruction, and `lpDDVertex` the runtime's TL buffer. The
  `D3DOP_*` opcodes share the DP2 numbering where the payloads match:
  `POINT` / `LINE` / `TRIANGLE` / `STATERENDER` (1 / 2 / 3 / 8) are
  `POINTS` / `INDEXEDLINELIST` / the legacy 8-byte `INDEXEDTRIANGLELIST`
  / `RENDERSTATE`, which is why the DP2 enumeration skips 4–7 and 9–14.
  The driver consumes those, `SPAN` (13, skipped) and `EXIT` (11). Every
  other opcode belongs to the runtime: `PROCESSVERTICES` (9) above all,
  the matrix / light opcodes 4–7, `TEXTURELOAD`, `BRANCHFORWARD` and
  `SETSTATUS`. The driver ends the call before such an opcode with
  `D3DERR_COMMAND_UNPARSED` (0x88760BB8) and its offset in
  `dwErrorOffset`. The runtime catches up its state mirror, executes the
  instruction itself (a `PROCESSVERTICES` fills the TL buffer) and calls
  again from the next one. **Skipping** opcode 9 instead makes `Execute`
  succeed with an all-zero TL buffer. `walk` bounces and logs the first
  eight (`d3dptdisp: execute buffer: opcode 9 x1 bounced to the runtime
  at 0x34`); a call that starts on a runtime instruction bounces with no
  host round trip. The runtime's `D3DParseUnknownCommand` answers
  `COMMAND_UNPARSED` for these too, but it is not the mechanism
  (`ddflags=0x80000` never calls it). No caps steer any of this, and a
  CLIPPED `Execute` never reaches the pass-through: the runtime
  transforms it and emits ordinary DP2 draws.
- **The DirectX 5 texture render states** arrive as the app wrote them,
  where the DX6+ runtimes turn them into stage states. The executor maps
  them (`legacy_render_state`):
  - `TEXTUREHANDLE` (1, the surface handle, which is dxg's and so ours)
    binds stage 0 and applies the blend;
  - `TEXTUREMAPBLEND` (21) sets stage 0's ops as the old fixed function
    did. With no texture it gives the diffuse, `DECAL` / `COPY` the
    texels, `MODULATE` texels × diffuse with alpha from the texture when
    its format has one (the colour-key expansion counts) and from the
    diffuse otherwise, plus `DECALALPHA`, `MODULATEALPHA` and `ADD`;
  - `TEXTUREADDRESS` (3), `TEXTUREMAG` / `MIN` (17 / 18) and `WRAPU` /
    `WRAPV` (5 / 6) go to stage 0's sampler and `WRAP0`.

  The legacy-blend flags of "A DirectX 6 title's flip chain" apply.

EBTEST passes its five cases (Clear, flat, textured, keyed, CLIPPED),
and Moto Racer plays, colour-keyed palms included, at 120 frames/s under
KVM (four DP2 calls and about 175 draws a frame); TCG is fast too,
because a DX3 title's guest side only builds execute buffers. Its
one-triangle draws cannot be batched (it sorts back to front and
switches texture per polygon) and cost DXVK nothing measurable. Each
`Execute` costs one doorbell round trip per run of driver instructions
plus one per bounce; batching waits for a title that shows the cost.
`tools/xp-motoracer.sh` drives the game, which insists on a 16 bpp
desktop.

### FIFA 2000 on the HAL

With no DLL in the folder, FIFA 2000's own DirectX renderer
(`THRASH\dx6z.dll`) runs on the HAL unmodified: the EA intro, the title
and the attract-mode match at 800×600×16. The match does not page-flip;
it blits its back buffer to the primary, so every frame is a READBACK
plus a HEL blit. `tools/xp-fifa2000.bat` and `tools/xp-fifa-match.sh
kvm|tcg <image>` drive it.

**The match ignores the keyboard under TCG.** FIFA's keyboard is a
`DISCL_NONEXCLUSIVE | DISCL_FOREGROUND` DirectInput device polled with
`GetDeviceState`. On XP a low-level hook feeds such a device, and it runs
only while the creating thread services its message queue, which the
match loop does rarely. Under TCG the hook falls behind: the device
reports no key while `GetAsyncKeyState` sees every one. Ruled out: the
emulator's input path (`DRIVER\DITEST.EXE` sees every key, and the
`qemu-embed: input:` statistics are clean), `LowLevelHooksTimeout`, and
the frame rate.

The fix is `DINPUT\DINPUT.DLL` next to the EXE (`SETUP /GAME 2`), a
forwarding shim that sets every key `GetAsyncKeyState` reports pressed
in the returned keyboard state; a KVM host needs nothing. The shim is
silent by default; `D3DPT_DINPUT_LOG=1` adds `dinput_log.txt` (devices,
cooperative level, poll rate, every key) through a sampler thread that
costs real time under TCG. `tools/xp-fifa2000.bat` turns it on when
`E:\DILOG` is staged.

**The shim goes next to the game, never system-wide** (user decision).
Replacing `system32\dinput.dll` fights Windows File Protection, a
forwarding shim cannot share its target's name in one directory, and
`AppInit_DLLs` would load it into every GUI process. The merge is also a
lie: `GetAsyncKeyState` is system-wide, so a foreground device that has
correctly gone quiet would report keys again. That lie is worth telling
FIFA's match loop, not every process.

FIFA's own quirks: its front-end menus need a mouse button held about
1 s, Esc skips the intro, and the kickoff starts by itself after about a
minute. In the match F1–F4 are cameras, Esc pauses and F12 exits.

### Max Payne on the HAL: XP's own d3d8.dll on a DX7 driver

XP's d3d8.dll treats a driver without a `D3DCAPS8` answer as a "DirectX
7 driver": it does the vertex processing itself and feeds the DX7 token
set through DrawPrimitives2. Max Payne runs that way with no wrapper DLL
(`ddflags=0x2000` gives d3d8.dll that face; `tools/xp-maxpayne.bat`,
about 290 frames/s at 800×600×16 under KVM `-cpu pentium3`). It exposed
two rules.

- **`…_IMM` tokens are DWORD-aligned, before and after.** After the
  `D3DHAL_DP2COMMAND` of `TRIANGLEFAN_IMM` / `LINELIST_IMM`, round the
  offset up to 4, then read the token's header and the vertices; round
  up again for the next command, as the DDK's perm3 sample does. The DX8
  runtime's legacy path starts such tokens at 2 mod 4 every frame (after
  an `INDEXEDTRIANGLELIST2` with an even count). Aligning only the end
  read vertices two bytes early: one garbage fan per frame, clipped to
  the screen as black bands across the alley. `d3dpt-dp2-test` sends
  such a fan with the runtime's 0xcc padding. The executor logs the
  token history with every first failure of a kind (`ddi: dp2: tokens
  before it (offset:op x count): …`).
- **DXVK's exceptions abort QEMU, so validate before calling.** A garbage
  `LightEnable` index made DXVK grow its light array to about 2³² and
  throw `std::bad_alloc`, which cannot be caught: DXVK carries its own
  statically linked unwinder, and the system `__gxx_personality_v0`
  `abort()`s when handed its context. The interpreter drops light
  indices ≥ 1024 and transform ids outside VIEW / PROJECTION /
  TEXTURE0–7 / WORLD0–3. `d3dpt_exec_submit` still wraps each record in
  a `try` for the executor's own allocations.

## The DirectX 8 DDI (M7c)

To d3d8.dll the driver is a DirectX 8 driver: `D3DCAPS8` with hardware
T&L, the DX8 token stream, render-to-texture and state sets. D3DGAME8
(doc 14's DX8 reference scene) runs through XP's own d3d8.dll with
hardware vertex processing and no wrapper DLL
(`tools/xp-driver-test.sh <image> d3dgame8`, diffed against the native
oracle).

- **`GetDriverInfo2`.** With `DDHALINFO_GETDRIVERINFO2` in the HAL info,
  the runtime sends `GUID_DDStereoMode` queries whose data starts with a
  `DD_GETDRIVERINFO2DATA` header (`dwMagic` = `D3DGDI2_MAGIC`). The
  driver answers `DXVERSION` (0x802), `GETD3DCAPS8` (212 bytes),
  `GETFORMATCOUNT` / `GETFORMAT` (`DDPF_D3DFORMAT` entries, with the
  D3DFORMAT in `dwFourCC` and the `D3DFORMAT_OP_*` in the `dwRBitMask`
  slot), and refuses the rest, including a real stereo query, which
  lacks the magic. Two traps from `d3d8.dll`'s disassembly: without the
  HAL-info flag the runtime never asks and stays on the DX7 path; and
  the runtime checks `dwActualSize` against the size inside the GDI2
  header while leaving the outer `dwExpectedSize` at the previous
  query's 24 bytes, so an answer clamped to the outer size makes it drop
  the driver (`CreateDevice` answers `D3DERR_NOTAVAILABLE`).
  `ddflags=0x2000` (`DDF_NO_DX8`) keeps the DX7 face.
- **The caps.** `D3DCAPS8` is the DX7 caps in DX8 form plus
  `HWTRANSFORMANDLIGHT`, `PUREDEVICE`, 16-bit indices, 4096² textures, 8
  stages, and the features of the sections below. `HWTRANSFORMANDLIGHT`
  is also in the DX7 `D3DDEVICEDESC`, with the transform and lighting
  caps, lights, clip planes and blend matrices. The executor maps
  SETTRANSFORM / MULTIPLYTRANSFORM / SETLIGHT / SETMATERIAL onto DXVK's
  fixed function; `ddflags=0x1000` withdraws it. **Never claim
  `D3DPMISCCAPS_CLIPTLVERTS`.** With it the runtime stops clipping
  pre-transformed vertices and hands the driver polygons that cross the
  camera plane, which the host rasterizes as garbage: Max Payne
  transforms on the CPU even on a T&L device, and its alley walls came
  out as flat panels at wrong depths. Since the runtime has clipped such
  geometry, the executor turns `D3DRS_CLIPPING` off for every
  pre-transformed draw (`Dp2::apply_clip`) and DXVK then draws it
  unclipped in depth, depth clamped, as the cards without the cap do
  (`patches/dxvk/12`, M16 finding 24).
- **What Wine's conformance tests fixed (M16).** A8R8G8B8 carries
  `D3DFORMAT_OP_SAME_FORMAT_UP_TO_ALPHA_RENDERTARGET` (0x100, from
  `ddk/ddrawint.h`), so a windowed device can have an A8R8G8B8 back
  buffer on the X8R8G8B8 desktop; with a wrong value (0x20) d3d8.dll
  dropped the HAL altogether. Cube and volume textures claim
  `CUBEMAP_POW2` / `VOLUMEMAP_POW2` wherever 2D textures claim POW2, so
  the runtime refuses the sizes real cards refuse.
- **The tokens.** The DX8 draws name vertex and index buffers by surface
  handle, possibly in guest system memory the host cannot see. The
  driver keeps a table of every surface dxg reports (VRAM and system
  memory, each level's address and pitch, buffers with their size) and
  walks the stream twice in `D3dDrawPrimitives2` (`core/core_dp2.c`).
  - SETVERTEXSHADER (an FVF, or a shader handle with bit 0 set),
    SETSTREAMSOURCE(UM) and SETINDICES become context state.
  - Each DRAWPRIMITIVE(2) / DRAWINDEXEDPRIMITIVE(2) / CLIPPEDTRIANGLEFAN
    becomes a self-contained `D3DPT_DP2_DRAW8` (protocol v6) carrying the
    primitive, the vertex format, the vertex range and the indices
    (relative to MinIndex).
  - The driver does TEXBLT itself (system-memory texture → VRAM, then
    VRAM_DIRTY).
  - Patches and dirty rects are dropped by size.
  - The DX7 tokens pass through, with inline-vertex ones re-padded for
    their new offset.

  Pass 1 measures and blits, pass 2 writes. **The state persists between
  calls**, because the runtime sends bindings only on change. The driver
  keeps it as handles and resolves them at every call, because a `Lock`
  with DISCARD gives a buffer new memory and dxg reports that with
  another `CreateSurfaceEx`. A user-memory stream holds `dwVertexLength`
  vertices of the token's stride, not of `dwVertexSize`. The driver's
  `dx8 draws skipped … why` line names skipped draws (bits: 1 shader, 2
  no FVF, 4 no stream, 8 stride < FVF, 16 vertex range, 32 index range,
  64 primitive).
- **State sets** (`STATESET`) record into d3d9 state blocks.
  **Render-to-texture:** a texture with 3DDEVICE caps is a default-pool
  render-target texture whose level 0 is the target. The d3d8-only
  render states 153, 164, 172, 173 are dropped; the rest share d3d9's
  numbering.
- **Compressed textures need a `DdCreateSurface` that sizes them.** dxg
  sizes a video-memory surface from its bit count, which a FOURCC format
  lacks, so a DXT texture asked for zero bytes: a `D3DPOOL_DEFAULT` one
  failed with `D3DERR_OUTOFVIDEOMEMORY`, and a MANAGED one's video copy
  failed silently at the first draw and left the previous texture bound.
  For a `DDPF_FOURCC` DXT surface the callback sets `dwBlockSizeX` to the
  linear size, `dwBlockSizeY` to 1, `fpVidMem =
  DDHAL_PLEASEALLOC_BLOCKSIZE` and `dwLinearSize` (hence dxg's "pitch" of
  a DXT surface is its linear size), and returns `NOTHANDLED` for
  everything else. DirectDraw creates a FOURCC surface only when the
  code is in the driver's FOURCC list (`DrvGetDirectDrawInfo`'s
  `pdwFourCC`, read by the kernel, not d3d8.dll). A FOURCC-style entry
  in the GDI2 format list makes `CreateTexture` fail, because d3d8.dll
  matches `dwFourCC` under `DDPF_D3DFORMAT` only. `DRIVER\DXTTEST.EXE`
  tries every format × pool.
- **The runtime's clipped fans are stream-0 draws, and the DP2 vertex
  buffer is a dummy under d3d8.dll.** With `CLIPTLVERTS` withdrawn, the
  runtime clips pre-transformed triangles and emits `CLIPPEDTRIANGLEFAN`
  (58: FirstVertexOffset, dwEdgeFlags, PrimitiveCount). d3d8.dll keeps
  its own clip stream (a `D3DPOOL_DEFAULT`, `D3DUSAGE_DYNAMIC` vertex
  buffer), copies the fan in at the current FVF's stride, and first
  emits `SETSTREAMSOURCE` for stream 0 with that buffer, so
  `FirstVertexOffset` is a byte offset into stream 0 as bound. The DP2
  call's own vertex pointer is a 10 × 32-byte dummy on the DX8 path
  (only `DrawPrimitiveUP` swaps a user pointer in). Reading fans from
  `lpVertices`, the DX7 layout, drew Max Payne's nearest walls black.
- **Filter numbering.** d3d8.dll hands a DirectX 8 driver its own
  `D3DTEXF_*` values for `MAGFILTER` / `MIPFILTER` (NONE 0, POINT 1,
  LINEAR 2, ANISOTROPIC 3, the cubics 4 and 5). The DX7 runtime sends
  `D3DTFG_*` (ANISOTROPIC 5) and `D3DTFP_*` (NONE 1, POINT 2, LINEAR 3);
  `MINFILTER` agrees. The executor reads the DX7 numbering, so the driver
  rewrites a d3d8.dll context's two states as it copies them
  (`tss_dx8_filter`). It tells runtimes apart by `ContextCreate`'s
  `dwhContext` on input, which is the interface version: **4** d3d8.dll,
  **3** DirectX 7, **0** the execute-buffer path (`iface` on the `d3d
  context` line). Without the rewrite every DX8 trilinear filter drew
  point-mipped (D3DGAME8: 11163 pixels off the oracle, 618 with it).

Each protocol feature below has a `ddflags` A/B bit, a `d3dpt-dp2-test`
section (including hostile records) and a guest probe.

### Vertex and pixel shaders 1.x on the DX8 DDI

Protocol v7. The caps say `D3DVS_VERSION(1,1)` / `D3DPS_VERSION(1,4)`
(96 vertex constants, `MaxPixelShaderValue` 8; `ddflags=0x4000`
withdraws both). d3d8.dll validates each shader against the caps and
emits `CREATEVERTEXSHADER` (handle, declaration bytes, function bytes),
`SETVERTEXSHADER`, `SETVERTEXSHADERCONST`, `DELETEVERTEXSHADER` and the
four pixel-shader equivalents.

- **The driver stays out of it.** The shader tokens pass through the
  walk. A DRAW8 under a shader carries the handle in its `fvf` field
  with the stream's stride; only the host knows what a vertex is.
- **The executor** keeps shaders per context by handle.
  `CREATEVERTEXSHADER` converts the `D3DVSD_*` declaration to
  `D3DVERTEXELEMENT9`s: `REG`'s register names the usage by the DX8
  convention (0 position, 3 normal, 5 diffuse, 7.. texcoords), `SKIP`
  and `CONST` runs are remembered, and tessellator and `EXT` tokens are
  skipped. It records the bytes it reads of each stream, and puts one
  `dcl_usage vN` per input register in front of the function, since d3d9
  wants them and DX8 shaders have none. A declaration without a function
  is the fixed function on that layout. `SETVERTEXSHADER` applies the
  declaration, the function and the `D3DVSD_CONST` runs (DX8 loads them
  when the shader is set). Constants go to `Set*ShaderConstantF` with the
  register range checked. A DRAW8 under an unknown shader, or whose
  declaration reads a stream or bytes the draw did not carry, is skipped
  with one log line.
- **The bytecode is validated before DXVK sees it.** On an unknown
  opcode DXVK's compiler logs `No layout known for opcode` and then
  asserts, which aborts QEMU. `sm1_valid` walks the tokens against a
  table of the vs 1.x / ps 1.x opcodes with their operand counts (`DEF`
  carries four raw floats, `COMMENT` its length, `PHASE` exists only in
  ps 1.4) and checks every register against the stage's file sizes. It
  refuses anything else, and the handle stays unknown. The DX8 runtime
  validates shaders itself, so a real guest never gets there.
- **Test.** `DRIVER\SHTEST.EXE` runs through XP's d3d8.dll with
  hand-assembled shaders (mingw has no D3DX) and reads back every draw
  (`xp-driver-test.sh <image> shtest`). A declaration-only shader must
  list its registers in FVF order, because d3d8.dll refuses a
  colour-before-position layout.

### Vertex and index buffers in video memory

Protocol v9. With `D3DDEVCAPS_HWVERTEXBUFFER` / `HWINDEXBUFFER` (DDI-only
bits of `D3DCAPS8.DevCaps`), the runtime asks for its `D3DPOOL_DEFAULT`
buffers in video memory and keeps a MANAGED buffer's video copy in step
with `BUFFERBLT` tokens. A draw can then name the buffer instead of
copying it through the window. `ddflags=0x100000` (`DDF_NO_HWVB`) keeps
every buffer in system memory, the A/B. On Win98 the same since M16
finding 34, with two 9x differences (doc 19 §45): the HALINFO's
`ddsCaps` must claim `DDSCAPS_EXECUTEBUFFER`, and the size comes in
`dwWidth`.

- **Allocation.** The buffer callbacks see `DDSCAPS_EXECUTEBUFFER |
  DDSCAPS_VIDEOMEMORY` with `DDSCAPS2_VERTEXBUFFER` / `INDEXBUFFER`. As
  for a DXT texture, the driver asks dxg for one block of the size, and
  the `CreateSurfaceEx` that follows registers a `D3DPT_VS_BUFFER`
  surface (no host object; the host reads it from VRAM). Command buffers
  stay in system memory. `DDSCAPS_EXECUTEBUFFER` is 0x00800000 (the
  DDKs' `DDSCAPS_RESERVED2`); a transcription as 0x800 once broke a case.
- **Filling.** `D3dUnlockD3DBuffer` reports the locked range with
  `VRAM_DIRTY_RANGE`. The driver does `BUFFERBLT` itself in pass 1 and
  reports the range the same way. **The `BUFFERBLT` token is 24 bytes**,
  not the 20 its field list suggests: a sixth dword follows the
  `D3DRANGE`, and sized at 20 the stream desynchronised after the first
  blit. `DISCARD` / `NOOVERWRITE` need nothing, since every earlier draw
  ran inside its doorbell write. The host keeps no copy, so the ranges
  are only counted (`N buffer writes of M KiB` in the 5 s line).
- **Drawing.** `walk_draw` sets `D3DPT_DRAW8_VRAM_VB` / `VRAM_IB` and
  writes a `{handle, byte offset}` pair in place of the bytes. The host
  checks the range against the buffer and draws from the VRAM pointer.
  Inline and VRAM sources mix in one draw. The clip stream is a
  default-pool buffer, so clipped fans come from VRAM too.
- **The numbers** (GTA Vice City, `tools/xp-vicecity.sh play`, about 480
  draws a frame at 800×600×32, the game's limiter and the vertical blank
  off): **360–375 frames/s** under KVM against **265–285** with every
  draw's vertices copied, and **62–75** against **51–55** under TCG. The
  removed cost is the guest's memcpys. RenderWare rewrites 330–430 MiB
  of buffers per 5 s, so the host reads them at each draw rather than
  mirroring them.

### More than one vertex stream

Protocol v10. `MaxStreams` is 16, what the era's T&L parts claim.
`ddflags=0x200000` (`DDF_ONE_STREAM`) sets it back to 1, the A/B.

- **Driver.** The context keeps all sixteen bindings. A draw under a
  shader handle carries, after stream 0 and the indices, every other
  bound stream whose range is there, because the driver does not parse
  declarations. A stale binding costs an 8-byte reference when it is a
  VRAM buffer and a copy when it is not. One vertex number indexes every
  stream, so stream *n*'s range starts at stream 0's first vertex at
  stream *n*'s own stride. A stream that runs past its buffer is left
  out. An FVF draw carries stream 0 alone.
- **Record.** `D3DPT_DRAW8_STREAMS`. After the indices come a `{count,
  0}` pair and `count` `d3dpt_dp2_draw8_stream` entries (number 1..15
  increasing, stride, its own VRAM flag), each followed by bytes or
  `{handle, offset}`.
- **Executor.** It parses every entry before anything can skip the draw.
  A declaration that reads more than stream 0 is drawn interleaved: the
  streams it reads are copied into one vertex, and a copy of the
  declaration with every element moved into stream 0 at its stream's
  base is bound (cached per shader per stride set, eight at most). So
  the host holds no vertex buffers and every DRAW8 takes the same `…UP`
  path. `apply_vs`'s early return stays, because it keeps a
  `D3DVSD_CONST` run from clobbering constants set after the shader.
- **Tests.** SHTEST's two-stream cases and STRMTEST (three streams from
  a StartVertex, BaseVertexIndex + MinIndex, a system-memory stream, a
  gap, stale streams under an FVF draw), with every buffer's first
  vertices a decoy.

### Cube textures

Protocol v11. The caps are `D3DPTEXTURECAPS_CUBEMAP | MIPCUBEMAP`, cube
filter caps equal to the 2D ones, and `D3DFORMAT_OP_CUBETEXTURE` on
every RGB and DXT format (not P8). A render-target format is a
render-target cube too. `ddflags=0x400000` (`DDF_NO_CUBE`). **This is
the DX8 face only**: user-mode DirectDraw creates a DirectX 7 cube
against `GUID_DDMoreSurfaceCaps`, which this driver does not answer.

- **What the runtime builds.** Six surfaces. The root is +X's level 0
  (`DDSCAPS2_CUBEMAP | CUBEMAP_POSITIVEX`), and the other faces hang off
  its attach list, each with its own mip chain, so the flip-chain
  attach walk does not fit it.
- **Driver** (`core/core_surf.c`). `d3dpt_os_attached_all` (both layers)
  lists every attachment. `cube_faces` finds the six from the root, and
  `d3d_register_cube` puts every face in the table under its own handle.
  It sends one `VRAM_SURFACE` with `D3DPT_VS_CUBE` (face 0's level 0,
  then `6 × levels − 1` `{offset, pitch}` pairs face-major), then a
  `VRAM_CUBE_FACE` per face. A TEXBLT into a cube root copies every face
  and level (`blt_levels`).
- **Executor.** A plain cube is a managed `IDirect3DCubeTexture9`. A
  render-target cube is a one-level default-pool one whose faces are
  each entry's `rt`, so targeting a face, clearing, the untracked-write
  shadow and READBACK into the face's VRAM are the 2D path unchanged.
  `VRAM_DIRTY` of a face means the cube. A render-target cube uploads
  only its dirty faces (`faces_dirty`).
- **How the runtime fills a cube.** It writes the default-pool cube's
  faces through Lock / Unlock under each face's handle. No cube `TEXBLT`
  has been seen, so that path is kept but unexercised. System-memory
  copies reach `CreateSurfaceEx` with odd shapes and are logged `not
  mirrored … (system memory)`.
- **Test.** CUBETEST (managed, Lock, `UpdateTexture` into a cube the
  host already uploaded, DXT1, a render-target cube,
  `TCI_CAMERASPACENORMAL`, a vs 1.1 writing `oT0`).

### Volume textures

Protocol v12. The caps are `D3DPTEXTURECAPS_VOLUMEMAP | MIPVOLUMEMAP`,
volume filter and address caps equal to the 2D ones, `MaxVolumeExtent`
256, and `D3DFORMAT_OP_VOLUMETEXTURE` on the six RGB formats and
DXT1/3/5, not P8. `ddflags=0x1000000` (`DDF_NO_VOLUME`). The DX8 face
only.

- **What the runtime builds.** One DirectDraw surface per level,
  `DDSCAPS2_VOLUME` with the depth in `dwCaps4`'s low word, each through
  its own `DdCreateSurface`. It fills a video-memory volume by locking
  the whole level and writing slice n at n × the slice pitch (upload,
  `LockBox` and `UpdateTexture` alike). No `VOLUMEBLT` has been seen,
  though the driver handles one like a TEXBLT.
- **The slice pitch trap.** `DD_SURFACE_GLOBAL.dwBlockSizeY` is a union
  with `lSlicePitch`. Asked for as one block of `size × 1` (the DXT
  recipe), the slice pitch came out 1 and the runtime wrote every slice
  a byte apart; setting it later in `CreateSurfaceEx` reaches only the
  kernel's copy. The driver asks for the block as **`depth` × `pitch ×
  rows`**, whose height is the slice pitch the runtime then uses.
  `DdCreateSurface` sizes rows by format (`fmt_row_bytes`, `surf_rows`:
  block rows for DXT), on 9x as on NT.
- **Driver / executor.** `VRAM_SURFACE` with `D3DPT_VS_VOLUME` carries
  level 0's first slice, the levels' pairs, then `{depth, slice pitch}`.
  The host makes a managed `IDirect3DVolumeTexture9` uploaded slice by
  slice. It refuses depth 0 or over 256, a short slice pitch, anything
  past VRAM, and a volume that is also a cube, target, primary or Z
  buffer.
- **Test.** VOLTEST (managed and minified, `LockBox`, `UpdateTexture`, a
  DXT1 volume). DXT3/5 share the arithmetic and are unexercised.

### Anisotropic filtering

Caps only, since the executor already mapped the filters and passed
`MAXANISOTROPY`. Both faces claim `MINFANISOTROPIC | MAGFANISOTROPIC`
(cube and volume caps copy them), `D3DPRASTERCAPS_ANISOTROPY` and a
maximum of 16. `ddflags=0x2000000` (`DDF_NO_ANISO`). ANISTEST draws
receding stripes; far-row contrast is 0 under trilinear and 252 under
×16.

### The rest of DX8's texture formats

L8, A8L8, A4L4, A8, X4R4G4B4, R3G3B2, A8R3G3B2, DXT2 and DXT4 are 2D
textures in the DX8 list (32 slots). `ddflags=0x4000000`
(`DDF_NO_MORE_FMTS`). The DX7 list is unchanged.

- **Driver.** `pf_format` knows d3d8.dll's pixel formats for them
  (`DDPF_LUMINANCE` by mask, `DDPF_ALPHA` alone at 8 bits, the 3-3-2
  masks), read through the RGB names of the union slots because the 9x
  DDK has no luminance names. DXT2/DXT4 are in both families' FOURCC
  lists, since DirectDraw checks that list first. `fmt_row_bytes` sizes
  all of them, and V8U8 too, whose managed `TEXBLT` used to copy rows of
  zero bytes.
- **Host.** At device creation the executor asks `CheckDeviceFormat`
  about each format once and expands any refused one to A8R8G8B8 at
  upload, as it does P8 (`host_lacks`, `texel_argb`). The log says which
  (`ddi: no host texture format 52 27 29: expanded to A8R8G8B8 at
  upload` under KosmicKrisp). **X4R4G4B4 is always expanded**, DX7's
  X4R4G4B4 surfaces included: DXVK creates it as
  `VK_FORMAT_A4R4G4B4_UNORM_PACK16` with no swizzle, so the X nibble
  samples as alpha, and a texture written with it 0 draws transparent.
- **Bump maps.** V8U8 is in both lists (the DX8 one as `TEXTURE |
  BUMPMAP`, the DX7 one as a `DDPF_BUMPDUDV` format for DX6/7 EMBM
  titles). L6V5U5 and X8L8V8U8 are in the DX8 one, mapped from
  `DDPF_BUMPDUDV | DDPF_BUMPLUMINANCE` by mask (the luminance mask sits
  in the `dwBBitMask` slot). `ddflags=0x800000` (`DDF_NO_BUMP`) takes
  them all out. The host needs only sizes: DXVK does the op, and the
  bump matrix is an ordinary stage state. DXVK's fixed function never
  applied the luminance (a shadowed variable in `sampleTexture`, and the
  luminance read from the wrong texel); `patches/dxvk/07` fixes it, with
  a `d3dpt-dp2-test` case that fails on unpatched DXVK.
- **Q8W8V8U8 is a FOURCC surface.** It has no DDPIXELFORMAT, so d3d8.dll
  creates it as a FOURCC surface whose code is the D3DFORMAT (63).
  DirectDraw refuses a FOURCC missing from the driver's list before any
  callback, and the texture silently kept only its system-memory copy.
  Both layers list 63 and size it. 3DMark2001 SE refuses Nature without
  it (doc 19 §38).
- **Point sprites** with a per-vertex size: the driver claims
  `D3DFVFCAPS_PSIZE` (its `fvf_stride` and the host already carried it).

### Multisampling

Protocol v13, full-screen only. The format list gives the render-target
formats (X8R8G8B8, A8R8G8B8, R5G6B5, X1R5G5B5) and D16 / D24X8 / D24S8
2 and 4 samples in `MultiSampleCaps` (the green-mask slot, with
`wFlipMSTypes` in the low word, `wBltMSTypes` in the high word, and bit
n − 1 for n samples). `ddflags=0x8000000` (`DDF_NO_MSAA`).

- **The driver** reads the count off `ddsCapsEx.dwCaps3`
  (`DDSCAPS3_MULTISAMPLE_MASK`) and puts it in the `VRAM_SURFACE` caps
  (`D3DPT_VS_SAMPLES`, bits 8..12) of a target, depth buffer or
  flip-chain member. VRAM holds only the resolved image.
- **The host** creates the target and its depth buffer multisampled when
  DXVK has the count (`CheckDeviceMultiSampleType`, else plainly, logged
  once) and resolves (`StretchRect`, `resolved()`) before every readback.
  Nothing is uploaded into one, since Direct3D 8 locks no multisampled
  surface.
- **Full screen only.** With blt types claimed, d3d8.dll made a windowed
  multisampled device whose `Present` drew nothing: a windowed `Present`
  of a multisampled back buffer is a driver blt, and there is no blitter
  ("Blit caps and the HEL"). A flip needs nothing new.
- **Test.** MSAATEST, a full-screen 640×480×16 4-sample device, reads
  the edge from the front buffer after the flip.

### The DX8 feature probes

`DRIVER\` holds one program per Direct3D 8 feature, each running through
XP's own d3d8.dll with every draw read back in the guest, built over
`guest-tools/src/d3dptvid/d3d8probe.h`. A probe first logs the caps and
`CheckDeviceFormat` answers for its feature. **When the caps say the
driver has no such feature, it ends `(not offered: <why>)` and runs
nothing**, so the same program becomes the feature's check the day the
caps claim it. `tools/xp-driver-test.sh <image> probe <NAME>` runs one,
and `probes` runs all ten in one boot. The verdict is PASS, NOT OFFERED
or FAIL.

| Probe | Feature | On XP |
|---|---|---|
| `CUBETEST` | cube textures (v11) | PASS, 9 cases |
| `STRMTEST` | several vertex streams (v10) | PASS, 6 |
| `VOLTEST` | volume textures (v12), DXT1 included | PASS, 5 |
| `FMTTEST` | the nine formats above, colour and alpha each | PASS, 9 |
| `BUMPTEST` | EMBM on each bump format, luminance variants, DOT3 | PASS, 8 (Q8W8V8U8's cases pass on Win98; XP not re-run) |
| `SPRTEST` | point sprites, per-vertex size | PASS, 4 |
| `ANISTEST` | anisotropic filtering | PASS, 1 |
| `PATCHTST` | RT- and N-patches | NOT OFFERED |
| `MSAATEST` | full-screen multisampling on and off | PASS, 2 |
| `MGDTEST` | `UpdateTexture`, a managed texture and a managed vertex buffer changed between two draws | PASS, 3 (doc 19 §32) |

## The DirectX 9 DDI (M16)

Track M16 (`docs/tracks/m16-dx9-ddi.md`, ADR-021). To XP's `d3d9.dll`
the driver is a DirectX 9 driver with vs / ps 3.0; `d3d8.dll` still sees
the DX8 driver above (it asks for `GETD3DCAPS8`, never the DX9 queries).
Both OS layers route `GetDriverInfo2` to `core_gdi2_answer`
(`core/core_caps.c`); the NT layer turns the DX9 face on (`core.dx9`),
the 9x layer not yet.

- **The queries.** `d3d9.dll` asks, in this order: `DXVERSION` (0x902),
  `GETD3DCAPS9` (304 bytes), `GETDDIVERSION` (its `dwDXVersion` is **9**,
  not 0x900; the answer is `DX9_DDI_VERSION`, 4), `GETFORMATCOUNT` /
  `GETFORMAT`, `GETEXTENDEDMODECOUNT` (0), `GETADAPTERGROUP`,
  `GETD3DQUERYCOUNT` (0 for now). Each answer needs `DD_OK`, the exact
  `dwActualSize` and its field set, or the runtime falls back.
- **The runtime's caps check.** An answered `D3DCAPS9` still goes through
  `d3d9.dll`'s validation (XP SP3's at 0x4fcf6d20, read in the
  disassembly and followed under the QEMU gdbstub, `-gdb tcp::N` and a
  hardware breakpoint, since `d3d9.dll` loads at its preferred base in
  every process). A driver that fails it gets the runtime's DX7-level
  caps: no T&L, no streams, no shaders, `MaxStreamStride` 255, with
  `GetDeviceCaps` succeeding, so the symptom looks like a caps bug and
  not a refusal. What it demands, beyond the DX8 caps:
  - every DX9 driver: `D3DCAPS2_DYNAMICTEXTURES`,
    `D3DPRASTERCAPS_SCISSORTEST`, `NumSimultaneousRTs` 1..4, and every
    format with multisample types must also claim NONMASKABLE (bit 0 of
    the flip / blt words);
  - vs or ps 2.0: `D3DPMISCCAPS_FOGINFVF`, no `LINEPATTERNREP`,
    `MaxStreams` of 8 or more, NONPOW2CONDITIONAL beside POW2, the
    `VS20Caps` / `PS20Caps` fields inside d3d9caps.h's ranges;
  - vs 3.0: 512..32768 instruction slots, predication, a guard band of
    at least 8192 each way, point-sampled vertex textures in
    `VertexTextureFilterCaps`, `D3DFVFCAPS_PSIZE`,
    `D3DDEVCAPS2_VERTEXELEMENTSCANSHARESTREAMOFFSET`, the declaration
    types 0x30f;
  - ps 3.0: 4096² textures, repeat 8192, anisotropy 16, depth bias and
    slope-scaled depth bias, the blend factor on both blends, two-sided
    stencil, `TEXREPEATNOTSCALEDBYSIZE`, full compare and filter caps.
  The executor already honours the render states these claim
  (`rs_passthrough`).
- **The tokens.** Declarations and vertex shader code arrive apart.
  `SETVERTEXSHADERDECL` shares DX8's handle space (bit 0 set: a
  declaration, else an FVF), so the walker consumes it as it does DX8's
  `SETVERTEXSHADER` and a `DRAW8` carries it; the host turns a
  `CREATEVERTEXSHADERDECL` into a vertex declaration and applies the
  context's current `SETVERTEXSHADERFUNC` over it (0 = fixed function).
  `SETSTREAMSOURCE2`'s offset is the walker's. `SETRENDERTARGET2`
  (index 0) and `SETDEPTHSTENCIL` become DX7's `SETRENDERTARGET` pair;
  targets 1..3 (v17, `NumSimultaneousRTs` 4 with independent write masks,
  bit depths and blending) go to the host as the runtime's token, which
  keeps them per context, binds them with the context's target, marks
  them drawn into for the readback, and forgets one that is released.
  Integer / boolean constants and the scissor go to the host.
- **Instancing** (v16). SETSTREAMSOURCEFREQ's dividers are context state
  like the stream bindings. An indexed draw under a declaration with
  stream 0 at `INDEXEDDATA | n` is instanced: the DRAW8 carries stream
  0's value in its stream-count word pair and each stream's own in the
  word that was padding, and an `INSTANCEDATA | d` stream sends its first
  ceil(n / d) elements instead of the draw's vertex range. The host draws
  through `DrawIndexedPrimitiveUP`, which has no stream frequency, so it
  draws the geometry n times with instance k's element interleaved into
  every vertex (element k / d).
- **Mip generation** (v15). The driver claims `D3DCAPS2_CANAUTOGENMIPMAP`
  and `D3DFORMAT_OP_AUTOGENMIPMAP` on the 16- and 32-bit RGB formats. A
  `D3DUSAGE_AUTOGENMIPMAP` texture is one video-memory surface with
  `DDSCAPS3_AUTOGENMIPMAP` (0x800) in `dwCaps3`, and the application
  sees one level, so the guest keeps level 0 alone and registers it with
  `D3DPT_VS_AUTOGEN`. The host creates it with `D3DUSAGE_AUTOGENMIPMAP`
  and makes the levels after each upload of level 0 and before sampling
  a target drawn into since (DXVK does neither on its own), and at the
  stream's GENERATEMIPSUBLEVELS, which the walker passes through.
- **A render-target texture's levels** (v19). The host makes such a
  texture with all its levels. A level has a handle of its own, which a
  SETRENDERTARGET names, so the driver links each to its texture
  (`D3DPT_OP_VRAM_MIP_LEVEL`) and the host serves it as the texture's
  `GetSurfaceLevel`. The runtime's BLT names a level as the texture's
  handle and a level index, so the driver reads back, or marks dirty, the
  level's own handle.
- **An unbound stream reads as zeros.** A declaration that reads a
  stream nothing is bound to draws with that stream's elements zero, as
  Direct3D 9 does (black for a missing colour, the point size of a
  missing PSIZE element), instead of the host skipping the draw.
- **A new context starts on a fresh device.** One host device serves
  every context, and the runtime sends a new context its render and
  stage states but never a light it has not enabled. So the host captures
  the device's state at its first context and gives each later new
  context that state with every light it saw enabled turned off (d3d8
  visual's `lighting_test` found a light d3d9 visual had left on).
- **Volumes from d3d9.dll.** A VOLUMEBLT takes DXT volumes in whole
  blocks (a slice is its block rows apart), and a system-memory source
  with no pixel format as the target's format, as TEXBLT does.
- **SetLOD and UpdateTexture.** `d3d9.dll` manages a managed texture's
  LOD itself and sends no SETTEXLOD: it makes the video-memory copy again
  without the levels above the LOD and TEXBLTs the whole system-memory
  chain into it, the rectangle in the source's level 0. UpdateTexture
  from a bigger chain looks the same. So a TEXBLT whose source is larger
  than its target leaves out the source's top levels until the widths
  match and scales the rectangle down with them.
- **Blits.** BLT (StretchRect, and `GetRenderTargetData` into system
  memory), SURFACEBLT (UpdateSurface) and COLORFILL run in the driver on
  the surfaces' memory, like TEXBLT: the record ends before one that
  follows other tokens, a video-memory surface is read back first
  (nothing happens when the host has not drawn into it), and the one
  written gets `VRAM_DIRTY`.
- **Queries.** Event and occlusion. A result goes back in the **command
  buffer**: on a successful call a DX9 runtime reads `dwErrorOffset` as
  the bytes of responses at the buffer's start (`RESPONSEQUERY`: the DP2
  command with the entry count, the block's bytes, then {id, size, data}
  per query; `d3d9.dll` 0x4fd75950). The occlusion count is the host's;
  an `ISSUEQUERY` starts a record, so the draws before it have run when
  its end asks for the count. Responses are written after the whole call
  is walked, because they overwrite the commands.
- **Surfaces.** `d3d9.dll` registers a texture's system-memory copy
  with no pixel format (`DDRAWISURF_HASPIXELFORMAT` clear; a DXT1 one is
  its block rows' bytes wide), so the core marks it (`SURF.nopf`) and a
  `TEXBLT` from it takes the target's format and size. An offscreen plain
  surface (`CreateOffscreenPlainSurface`, `GetRenderTargetData`'s target)
  needs `D3DFORMAT_OP_OFFSCREENPLAIN` on its format; the RGB formats carry
  it when the last runtime to send `DXVERSION` was DX9, never to
  `d3d8.dll`. A texture with a full mip chain in video memory (the
  default pool, or a managed texture's copy) is a *lightweight mipmap*:
  one surface with `DDSCAPS3_LIGHTWEIGHTMIPMAP` in `dwCaps3`, whose
  levels the driver keeps. `DdCreateSurface` sizes it for the whole
  chain (`surf_lw_layout`) and the host gets each level's offset; the
  runtime never locks a sublevel and its `TEXBLT` carries every level
  (M16 track, finding 8).
- **Shaders.** vs / ps 2.0 and 3.0 reach DXVK with the version and END
  checks only (no SM2/3 validator for v1, user decision); 1.x keeps
  `sm1_valid`, which takes the `dcl` instructions `d3d9.dll`'s vs 1.1
  carries (`d3d8.dll`'s never has them).
- **The DX9 formats** (step 3). After `d3d8.dll`'s list come the ones
  only `d3d9.dll` is told of (`d3d_fmt9_n`): A8B8G8R8, X8B8G8R8,
  A2B10G10R10, A2R10G10B10, G16R16, A16B16G16R16, L16, V16U16,
  Q16W16V16U16, A2W10V10U10 and the six float formats R16F to
  A32B32G32R32F, the render-capable ones with
  `OFFSCREEN_RENDERTARGET` and `OFFSCREENPLAIN` (for
  `GetRenderTargetData`), the float ones with `VERTEXTEXTURE` under vs
  3.0. The ones with no DDPIXELFORMAT arrive as FOURCC surfaces whose code
  is the D3DFORMAT, as DX8's Q8W8V8U8, so the NT layer lists 36, 67 and
  110..116 among its FOURCC codes; `pf_format` maps the RGB-mask ones.
  For the DX9 runtime the formats also carry `SRGBREAD` (the 8-bit RGB
  ones and the DXTs), `SRGBWRITE` (X8R8G8B8, A8R8G8B8) and the ARGB group
  (`D3DFORMAT_MEMBEROFGROUP_ARGB` 0x80000 plus `CONVERT_TO_ARGB`, on
  X8R8G8B8, A8R8G8B8, R5G6B5, X1R5G5B5, A1R5G5B5), without which the
  runtime refuses a StretchRect or `CheckDeviceFormatConversion` between
  two formats. The BLT converts inside the group through a D3DCOLOR;
  COLORFILL packs every listed format, the float ones by integer long
  division (the kernel-mode driver keeps off the FPU).
  `StretchRectFilterCaps` claims point and linear; both take the nearest
  texel for now. `D3DDEVCAPS2_CAN_STRETCHRECT_FROM_TEXTURES` is claimed.
  A StretchRect between two video-memory depth buffers is the host's
  (v18): depth never comes back to VRAM, so the walker sends that BLT on
  as the runtime's token and the host runs DXVK's StretchRect on the two
  buffers.
- **Samplers.** A DX9 stage past the fixed function's eight is a sampler
  only: 8..15 for ps 2.0's sixteen, and **257..260 for the vertex
  samplers** (D3DVERTEXTEXTURESAMPLER0..3, seen in a trace). The
  sampler states arrive as texture stage states: DX7's 12..21, DX8's
  ADDRESSW 25, and 29..31 for sRGB read, element index and displacement
  offset (the host's `sampler_state`). ADDRESSW went to
  `SetTextureStageState` before, where D3D9 has no such state.
- **Lazy targets.** `d3d9.dll` sends a SetRenderTarget and the viewport
  it resets at the next draw or clear, not when called. A surface
  released while still the context's target is forgotten by the context
  (its handle is reused at once), and a DP2 call whose context names no
  target binds the one its SETRENDERTARGET brings. A CreateStateBlock in
  between captures the old target's viewport on the host, so executing a
  state set leaves a DX9 context's viewport as the stream last set it
  (D3DFEAT9's 4x4 float target put its whole frame through a 4x4
  viewport until then).

## Windows 7 (track M18)

The same driver installs and runs on Windows 7 SP1 (32-bit), which still
loads XP-model display drivers: no Aero (the desktop compositor needs a
WDDM driver, track M18), and everything else as on XP. DirectDraw takes
the HAL, `d3d9.dll` / `d3d8.dll` get the DX9 / DX8 DDI with shader model
3.0, and D3DGAME9, D3DGAME8, D3DFEAT9 and D3D7TEST give the native frames
byte for byte (`tools/xp-driver-test.sh` with `MEM=2048`, 2026-09-27).
32-bit Windows 7 loads an unsigned kernel driver after the "can't verify
the publisher" prompt; 64-bit would need a signed one.

**The screen starts 4 MiB into VRAM on Windows 6.x.** Windows 7's boot
screen draws through VBE on the adapter's linear frame buffer (1024x768x24,
the first 2.25 MiB), and the kernel keeps that view, uncached, for the blue
screen. From Vista on the memory manager gives every later mapping of the
same physical pages the same cache type, so the miniport's cached views
(`d3dptvid.c`, "VRAM mappings") came out uncached over exactly that range
(QEMU's `info tlb`: PCD and PWT on those 576 pages alone). The primary and
a full-screen flip chain live there: DDTEST wrote its frames at 30
frames/s (every frame 2 x 15.6 ms, which first looked like a timer) and a
windowed blit to the primary took 70 ms. The miniport now puts the screen
at `NT6_FB_BASE` (4 MiB, which also covers a 32-bit boot screen) when
`PsGetVersion` says 6 or later, and reports it as `FrameBufferBase -
VideoRamBase`. The display driver keeps every offset VRAM's and adds the
base to the primary, GDI's surface, the flip back to GDI and the heap's
start; an older display driver reads 0 there. XP and Win98 are unchanged.
DDTEST then runs at the refresh (75 Hz, the rate Windows 7 picks) and the
windowed blit at 291 frames/s.

## The ddflags bits

`-device d3dpt-vga,ddflags=N` (the adapter's DDFLAGS register; `DDFLAGS=`
in `tools/xp-driver-test.sh`) switches driver behaviour with no
reinstall. The bits are `DDF_*` in `core/d3dpt_core.h`; the 9x layer's
own are `D9F_*` (doc 19).

| Bit | Name | Effect |
|---|---|---|
| 0x1 | `DDF_NO_GETDRIVERINFO` | no GetDriverInfo |
| 0x2 | `DDF_TEX_ANYSIZE` | textures of any size (no `POW2`) |
| 0x4 | `DDF_NO_SURFACE_CB` | only MapMemory + CanCreateSurface |
| 0x8 | `DDF_ENGINE_BITMAP` | engine-bitmap primary |
| 0x10 | `DDF_GDI_CAP` | add `DDCAPS_GDI`: the HAL-drop repro |
| 0x20 | `DDF_NO_D3D` | DirectDraw only (the driver decides; not `no-exec`) |
| 0x40–0x800 | `DDF_NO_3D_CAP` … `DDF_NO_PARSEUNKNOWN` | one Direct3D answer each: 3D caps, D3D GetDriverInfo, buffer callbacks, Callbacks3, Misc2, ParseUnknownCommand |
| 0x1000 | `DDF_NO_TNL` | no hardware T&L |
| 0x2000 | `DDF_NO_DX8` | a DirectX 7 driver to d3d8.dll |
| 0x4000 | `DDF_NO_SHADERS` | no shader versions |
| 0x8000 | `DDF_NO_VSYNC` | flips complete instantly |
| 0x10000 | `DDF_NO_CKEY` | no colour keying, no P8 |
| 0x20000 | `DDF_CKEY_NOBLTCB` | key caps without a Blt callback: HAL-drop repro |
| 0x40000 | `DDF_EB_MAXVERT_65535` | the DX3 `E_OUTOFMEMORY` repro |
| 0x80000 | `DDF_NO_PARSEUNKNOWN_CALL` | never call the runtime's `D3DParseUnknownCommand` |
| 0x100000 | `DDF_NO_HWVB` | buffers in system memory (pre-v9) |
| 0x200000 | `DDF_ONE_STREAM` | one stream (pre-v10) |
| 0x400000 | `DDF_NO_CUBE` | no cube textures |
| 0x800000 | `DDF_NO_BUMP` | no bump formats |
| 0x1000000 | `DDF_NO_VOLUME` | no volume textures |
| 0x2000000 | `DDF_NO_ANISO` | no anisotropic filtering |
| 0x4000000 | `DDF_NO_MORE_FMTS` | none of the nine extra formats |
| 0x8000000 | `DDF_NO_MSAA` | no multisampling |
| 0x10000000 | `DDF_NO_GAMMA` | no gamma ramp |
| 0x20000000 | `DDF_TEX_256` | textures of 256² at most, as a Voodoo 2's (doc 19) |
| 0x40000000 | `DDF_NO_DX9` | no DirectX 9 face: `d3d9.dll` sees the DX8 driver (M16) |
| 0x80000000 | `DDF_SM2` | the DX9 face claims vs / ps 2.0 (M16). The last free bit |

## Debugging the driver

- **The DEBUG register** → the QEMU log (`d3dpt-vga: guest: d3dptdisp:
  …`) is the driver's only output. No kernel debugger is used.
- **`D3DPT_DP2_TRACE=<flag file>`** in QEMU's environment: `touch` the
  file when the screen shows the scene in question, and the executor
  logs one whole frame (states, tokens, textures, first vertices) and
  writes each bound texture's levels and the target after every draw as
  `.ppm` next to the flag file, then removes it. `docs/testing.md` lists
  what it dumps; counting pixels per `draw-<n>.ppm` names the draw that
  paints an artefact. A readback with no draw before it does not end the
  frame (`readback of N with no draw, the frame goes on`), because
  Crimson Skies reads its target back after every target switch.
- **`D3DPT_DDI_REREAD=1`** re-reads every texture from VRAM at every bind
  (to tell a stale host copy from VRAM the guest never wrote).
  **`D3DPT_DDI_NOFOG=1`** forces fog off.
- The image's `ddraw.dll`, `d3dim.dll`, `d3d8.dll` and `dxg.sys` can be
  pulled out (`qemu-img convert` + `7z x`) and disassembled with
  `i686-w64-mingw32-objdump`. Most rules above came from there, because
  the DDK documentation stops short of them.

## Open items

- **Present the host frame through the player's 3D path** instead of the
  per-frame readback into VRAM, and a vertical blank in phase with the
  player's swapchain rather than the `FRAMES` clock.
- **A blitter** behind `DDCAPS_BLT` (needed for windowed multisampling),
  and `DrvDeriveSurface` (an optimisation now that the target shadow
  exists).
- **The mode table from the player** (M2): a `modes=` property or an
  embed API call replacing the static table, with pixel-aspect flags per
  entry.
- **RT- and N-patches** (PATCHTST is the one probe not offered). Cube,
  volume and DX7-list entries for the extra formats when a title asks.
- **BUMPTEST's Q8W8V8U8 cases on XP** are unmeasured, because the
  install on a `winxp-m7` overlay failed when they were new
  (`docs/00-status.md`).
- FIFA 2000's intro videos play at 320×240 in the middle of the 640×480
  mode. Blit caps are not the cause: with them claimed nothing blitted,
  since the game writes decoded frames through `Lock`. A RAM-backed
  palette page if a title animates faster than MMIO allows. More 8 bpp
  titles (StarCraft, Age of Empires, Caesar 3).
- The two `640×480×4 / 800×600×4 @ 1 Hz` entries in the mode list are
  vga.sys' (VgaSave stays registered as the VGA-compatible device). A
  `VgaCompatible = 1` INF variant would hide them, but it makes our
  driver the one bootvid uses. Left alone.
- Not planned: multi-monitor, DPMS, hot-unplug. Windows 2000 is untested
  (same DDI version).
