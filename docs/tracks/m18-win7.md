# Track M18: Windows 7, and Aero on it

Opened 2026-09-27 (user: "make our driver work on windows 7", then
"ultimately what I want is to run full Aero on windows 7"). The machine is
the user's `win7` bundle: Windows 7 Ultimate N SP1, **32-bit**, English,
KVM, 2 GB. Read `docs/00-status.md` first for the track rules; doc 15 is
the XP driver this starts from.

## Step 1: the XP-model driver on Windows 7 (done 2026-09-27)

Windows 7 still loads XP-model (XDDM) display drivers, and ours runs there
as it does on XP: DirectDraw, the DX7 / DX8 / DX9 DDIs with shader model
3.0, gamma, the cursor. D3DGAME9, D3DGAME8, D3DFEAT9 and D3D7TEST give the
native frames byte for byte; DDTEST's chains run at the refresh. Doc 15
"Windows 7" has the one real fix: the screen starts 4 MiB into VRAM on NT
6, above the boot screen's uncached pages. The guest-tools SETUP installs
all four NT components and names the system; its new manifest keeps
Windows 7's compatibility assistant quiet (guest-tools README). XDDM
means **no Aero**: that is step 2.

How it is driven headless (`tools/xp-driver-test.sh`, which reads the
family from `ver`): an administrator's console comes from the Start menu's
search box (`cmd`, Ctrl+Shift+Enter, UAC's Alt+Y; the Run box has no
Ctrl+Shift+Enter before Windows 10), and the unsigned-driver prompt never
has the keyboard, so it is clicked on the USB tablet. `MEM=2048`. A
program elevated by UAC runs in a console of its own, so its output goes
to a log file (`SETUP /LOG`), not to COM1 through a redirect.

## Step 2: a WDDM driver (ADR-022)

Aero's compositor (DWM) runs only on a WDDM driver: it composes every
window through Direct3D 9Ex surfaces shared between processes, which only
WDDM's memory manager provides. So Aero is a second driver model beside
the XP one, not an extension of it. The executor, the protocol, DXVK and
the device stay; the guest side is new:

- **The kernel-mode driver** (KMD), registered with `dxgkrnl.sys` through
  `DxgkInitialize` and some 60 `DxgkDdi*` callbacks: the adapter's
  memory segments (VRAM as one linear segment), allocations, the paging
  buffers that move them, DMA buffers and their submission, fences with
  an interrupt, the display paths (VidPN: modes, the scanout address,
  which is our `OFFSET` register), the cursor, vertical-blank
  interrupts (DWM paces on them) and timeout recovery.
- **The user-mode driver** (UMD) for Direct3D 9: `OpenAdapter` and the
  `D3DDDI` device functions, which are the D3D9 API one level down. It
  would emit the DP2-style records the executor already decodes, so the
  host side mostly stays.
- **The device** gains what WDDM expects of a GPU: an interrupt line,
  a fence register, and command buffers read from guest memory by DMA
  rather than written into the VRAM window. Register-set additions only
  (`D3DPT_FB_VERSION`), so XP, Win98 and old snapshots are untouched.
- Without DWM, Windows 7's own desktop already needs most of the KMD: the
  canonical display driver (`cdd.dll`) draws GDI in system memory and
  presents through the KMD's `Present`. That is the first milestone: the
  desktop on our WDDM driver, basic theme. Direct3D 9 through the UMD is
  the second, DWM and Aero the third.

The user's decisions (ADR-022, amended 2026-10-02):

1. **The WDK's headers, built on Windows.** 2026-09-27 chose our own
   header subset to keep the Linux cross build; 2026-10-02 reversed it.
   The driver builds natively on Windows with MSVC and the WDK
   (`d3dkmddi.h`, `d3dumddi.h`, `dispmprt.h`, `displib.lib` for
   `DxgkInitialize`), which the build reads from the installed WDK and
   never commits. The Windows host build moves to MSVC too (ADR-026),
   later and outside this track.
2. **32-bit first.** The user's Windows 7 is 32-bit, which loads an
   unsigned driver after a prompt. 64-bit Windows 7 refuses unsigned kernel
   drivers outside test mode.

## Step 2's plan (reopened 2026-10-02)

The branch was `track/m18-win7` again, from `main` at the M21 merge,
merged and deleted on 2026-10-04 (user); then `track/m18-wei` (WinSAT,
SETUP's WDDM install, the launcher's Windows 7 family), merged and deleted
the same day (user), and `track/m18-install` (the fresh install, finding
14) and `track/m18-w7test` (`tools/win7-aero-test.sh`, finding 15),
likewise. The open items go on a new `track/m18-*` branch. The
driver work happens on the user's PC (Visual Studio + the WDK); the
device's half is plain QEMU C and builds anywhere.

1. **The toolchain, checked before any code.** Find the newest WDK that
   still builds a 32-bit kernel driver for Windows 7. Recent WDKs dropped
   Windows 7 as a target, and possibly 32-bit kernel drivers too; an
   older WDK may need its matching Visual Studio toolset. Write down the
   versions, where they install, and the command that builds, in a new
   "The WDDM driver" section of `docs/build-windows.md`. If no WDK fits,
   stop and ask: the choices are an older WDK, Windows 7 x64 in test
   mode, or Windows 10 as the first target.
   **Done 2026-10-02:** the EWDK for Windows 10 2004 (10.0.19041, VS 2019
   Build Tools 16.7), one ISO mounted, nothing installed (user's choice
   over VS 2019 + the WDK installers). Its kernel-mode toolset still takes
   `TargetVersion=Windows7` on Win32; `guest-tools/build-wddm.cmd` builds.
   `docs/build-windows.md` "The WDDM driver".
2. **An empty kernel driver that loads.** `guest-tools/src/d3dptvid/wddm/`
   (`km/` the kernel driver, `um/` the user-mode one later), an MSBuild
   project on the WDK's kernel-mode toolset, an INF for `d3dpt-vga`'s PCI
   ID. `DriverEntry` calls `DxgkInitialize` with every callback stubbed;
   `DxgkDdiStartDevice` logs through the device's DEBUG register (doc 15's
   rule: the QEMU log, never a debugger). Proved when Windows 7's Device
   Manager shows our adapter started, with the XP-model driver as the
   fallback a reinstall brings back.
   **Built 2026-10-02, not yet booted:** `wddm/km/d3dptkmd.c` registers
   every WDDM 1.1 callback (each stub declared with the WDK's own
   `DXGKDDI_*` type, so the header checks it); add/start/stop/remove are
   real, start maps the register BAR and logs the magic, version and VRAM
   size, and every stub names itself in the QEMU log, so the first boot
   shows what dxgkrnl asks for after StartDevice (QueryAdapterInfo's
   driver caps, segments, child relations, a VidPN): those answers are
   the rest of this step. `d3dptkmd.inf` (NTx86, no user-mode driver yet).
   **First boots, 2026-10-02** (the user's `win7` image through an
   overlay on the PC, TCG, `DRVINST.EXE D:\WDDM\D3DPTKMD.INF`; the INF
   installs and binds, the unsigned prompt clicked). What each boot taught:
   - Lines before StartDevice maps the BAR go to QEMU's debug console,
     port 0xE9 (`-debugcon file:<log>`); without it the first boots looked
     as if the driver never loaded (`sc query` said "never started", and
     `driverquery` showed `VgaSave` running), when dxgkrnl had loaded it
     and dropped the device.
   - **LinkDevice stays NULL.** dxgkrnl calls it right after AddDevice
     when set (linked adapters), and a stub that fails drops the device.
   - **The adapter needs an interrupt.** With no interrupt pin dxgkrnl
     queries the I2C and OPM interfaces (optional; NOT_SUPPORTED is right)
     and removes the device without calling StartDevice. `-device
     d3dpt-vga,irq=on` (new, off by default: XP, 9x and snapshots see no
     change; nothing raises it yet) gives the pin, and StartDevice runs.
     Part of step 4 brought forward; the launcher has to pass it for a
     Windows 7 machine on the WDDM driver. Committed after `scripts/test.sh
     all` passed on Windows (2026-10-02).
   - **The BARs by address.** A VGA-class device's resources carry the
     legacy 0xA0000 window ahead of the BARs; StartDevice reads BAR 0 and
     BAR 1 from config space (`DxgkCbReadDeviceSpace`) and matches them.
     It now logs `magic=0x42463344 version=5 vram=0x08000000`.
   - Not the cause of the device being dropped before StartDevice:
     `UserModeDriverName` / `InstalledDisplayDrivers` in the software key
     (tried, no change).
   - **Past QueryChildRelations.** With one video output answered dxgkrnl
     still stopped the adapter right after it. Three changes together
     ended that, not isolated one by one (a boot is ~5 min under TCG): the
     output reported as `D3DKMDT_VOT_HD15` (as VirtualBox's WDDM driver
     does) instead of `D3DKMDT_VOT_OTHER`, `HpdAwarenessInterruptible`
     instead of `AlwaysConnected` (QueryChildStatus answers connected), and
     `UserModeDriverName` in the software key, which the INF now writes
     (naming `d3dptumd.dll`, plan step 6's driver, before it exists).
   - **The DDI's structures at Windows 7's sizes.** The kit's headers
     default to WDDM 2.7 whatever `TargetVersion` says, so
     `sizeof(DXGK_DRIVERCAPS)` was larger than the 0x208 bytes Windows 7
     passes and DRIVERCAPS was refused (`STATUS_INVALID_PARAMETER`).
     `d3dptkmd.c` defines `DXGKDDI_INTERFACE_VERSION` as
     `DXGKDDI_INTERFACE_VERSION_WIN7` before its includes.
   - **Reached CreateDevice** (2026-10-02): DRIVERCAPS, the segment query
     (twice), QueryChildStatus (connected), QueryDeviceDescriptor (no
     EDID), RecommendMonitorModes (21 modes), many
     EnumVidPnCofuncModality rounds over every pivot, then CreateDevice,
     a stub, three times, and QueryInterface for another interface.
     Windows stays on its boot screen.
   - **The desktop through the WDDM driver** (2026-10-02, 640x480, no
     user-mode driver, so no DWM): GDI draws into dxgkrnl's shadow
     surface and cdd.dll presents it to the primary. The "GPU" is the
     CPU: the DMA buffers carry the driver's own packets (transfer, fill,
     blit, color fill, flip, aperture map/unmap), run at SubmitCommand
     through a kernel mapping of the segment, and the fence is reported
     done right there, the way an interrupt would
     (`DxgkCbSynchronizeExecution` → `DxgkCbNotifyInterrupt(DMA_COMPLETED)`
     → `DxgkCbQueueDpc` → `DxgkCbNotifyDpc`): the device raises no
     interrupt yet. What it took, each one a boot that stopped short:
     - **CreateDevice's `pInfo`.** `DXGKARG_CREATEDEVICE` puts `Flags` and
       `pInfo` in one union; Windows 7 passes a pointer there (a kernel
       address) and reads the DMA buffer and list sizes the driver writes
       through it. Unfilled, the device was made three times and dropped.
     - **Release every mode set before assigning new ones** in
       EnumVidPnCofuncModality (the pinned modes copied out first): with
       the old set still acquired, dxgkrnl enumerated 87 times and never
       committed a VidPN.
     - **Source modes in A8R8G8B8**, the format cdd.dll creates the
       primary in (with X8R8G8B8 the primary was dropped right after its
       first paging fill).
     - **Patch locations from Present.** The header marks
       `pPatchLocationListOut` "Not used", but without entries the shadow
       surface stayed in system memory (segment 0, address 0) and every
       blit had no source. With one per surface dxgkrnl pages the shadow
       into VRAM and calls Patch, which re-reads the allocation list into
       the packets.
     - An aperture segment (segment 2, 64 MiB at GPU address 0x80000000,
       MAP/UNMAP_APERTURE_SEGMENT kept as kernel mappings of the pages)
       went in on the way; the shadow lands in VRAM, so it is not
       exercised yet.
     Trace and boot loop: `w7test.sh` in the session scratchpad (a
     throwaway overlay on a frozen base where the driver is installed but
     disabled, so Windows boots to its VGA desktop; the CD with the
     build, the driver copied and enabled from an elevated console, a
     reboot). A boot that hangs costs nothing.
   - **1024x768 from an EDID.** With no descriptor Windows started at
     640x480 and ignored the preferred mode of RecommendMonitorModes;
     QueryDeviceDescriptor now answers a made-up EDID 1.3 block
     (manufacturer "TKS", name "2ksbox", preferred timing 1024x768 at 60
     Hz VESA DMT) and the desktop starts there.
   - **The hardware cursor** (register set v4, as the XP driver uses it):
     the image in the 16 KiB above the segment (taken off its top),
     monochrome / colour / masked colour converted to a8r8g8b8, Windows 7
     hands a 64x64 colour pointer. SetPointerPosition's X / Y are the
     image's corner, not the hot spot: with the pointer clicked at
     (700, 300) the device read CURSOR_X / Y = 700 / 300 with HOT 3 / 3
     (`xp /9wx <BAR 1>+0x90` in the monitor).
   - **Every mode, proved as step 5 asked** (2026-10-02): SETMODE from a
     console to 640x480, 800x600, 1024x768, 1152x864, 1280x960,
     1280x1024 and 1600x1200 at 60 Hz, 1024x768 at 75 and 1280x1024 at
     85: each a CommitVidPn of that size and pitch and a screendump of
     that size showing the desktop. 32 bpp only: Windows 7's desktop
     under WDDM has no 8 / 16 bpp primaries.
   - **The vertical blank** stands in for the interrupt the device does
     not raise yet: ControlInterrupt(CRTC_VSYNC) starts a periodic timer
     at the committed refresh whose DPC reports `CRTC_VSYNC` with the
     address being scanned out (OFFSET, which a flip packet moves), under
     the interrupt's lock, as an ISR would. Nothing enables it while there
     is no DWM, so it is untested.
   Left of step 5: timeout recovery (ResetFromTimeout / RestartFromTimeout
   answer success, never triggered). Next: step 6, the user-mode driver
   that DWM needs, and step 4's device interrupt to replace the timer.
3. **How the binaries reach the guest.** The guest-tools ISO is built on
   Linux, the WDDM driver on the PC. Decide in this step: build the ISO on
   the PC too (`build-windows.sh guest` already runs there), or copy the
   driver into the Linux checkout's `build/` for the ISO stage. SETUP's NT
   role then installs the WDDM driver on Windows 7 and the XP one on XP.
   **Decided 2026-10-04: the ISO is built on the PC** (user: fold the
   WDDM build into `build-windows.sh`). Its `wddm` stage runs
   `build-wddm.cmd` when an EWDK is mounted (skipped with a note
   otherwise), and the `guest` stage after it stages `build/wddm/x86` as
   `WDDM\` on the ISO, after the ISO's mingw checks (the user-mode DLL is
   MSVC; its own code is now `/arch:IA32` for the Pentium III floor, and
   the static C runtime picks its SSE2 routines by a CPU check); an ISO
   built on Linux had none until 2026-10-04 (user): the PC publishes the
   drivers by a hash of their sources (`build-windows.sh wddm --publish`)
   and `build.sh` on Linux and macOS fetches the matching one
   (`scripts/wddm-prebuilt.sh`, `docs/build-windows.md` "The WDDM
   driver"). The drivers' three files are part of the ISO's
   stamp in both build scripts. SETUP installs it (finding 12) when the
   adapter has its interrupt: the driver starts only with `-device
   d3dpt-vga,irq=on`, which the launcher does not pass yet; README.TXT on
   the disc gives the manual steps too.
4. **The device's additions** (`d3dpt/hw/d3dpt_vga.c`, `d3dpt/d3dpt_fb.h`,
   with M7): an interrupt line, a fence register, DMA command buffers read
   from guest memory. Registers are only added (`D3DPT_FB_VERSION`), so
   XP, Win98 and old snapshots see no change.
   **The vertical blank done 2026-10-03** (register set v6): `IRQ_ENABLE`
   / `IRQ_STATUS` at 0xb8 / 0xbc and `CAP_IRQ`, only with `irq=on`. The
   device raises `IRQ_VBLANK` every period of `HZ` (60 when unset) on
   QEMU's virtual clock while it is enabled, level-triggered on INTx until
   acknowledged; a paused guest gets none, and a missed deadline is
   skipped, not caught up. The kernel driver enables it from
   ControlInterrupt(CRTC_VSYNC) and its ISR acknowledges the bit and
   reports CRTC_VSYNC with the scanout address, an interrupt with nothing
   in IRQ_STATUS being another device's (the line may be shared). Without
   CAP_IRQ (a v5 device, which the driver still takes) the KTIMER stands
   in as before. Proved under DWM: the driver takes the device's
   interrupt, dxgkrnl switches it on and off as it needs the vertical
   blank (about 200 interrupts in the run), DWM composes at up to 57
   flips a second while D3DGAME9 runs (about 45 with the timer, whose
   period the system clock rounds to 15.6 ms), and D3DGAME9's frame
   stays 0 pixels off native.
   **The fence and the records from the DMA buffer done 2026-10-04**
   (register set v7, `CAP_DMA`):
   - `FENCE` (0xd0) takes a submission's fence once its work is done:
     `FENCE_DONE` (0xd4) reads it back and `IRQ_DMA` raises the interrupt,
     whose ISR reports DMA_COMPLETED with it. SubmitCommand writes it after
     running the buffer, instead of reporting the completion itself
     through SynchronizeExecution (kept for a device without CAP_DMA /
     CAP_IRQ). The work is still all done inside SubmitCommand, so a
     fence is done when written; the register is where an asynchronous
     host would report later ones.
   - `DMA_ADDR_LO` / `HI`, `DMA_BYTES`, then `DMA_APPEND` (the record
     count, 0xc0..0xcc): the device copies whole records from guest memory
     to the end of the window's batch and updates its header, as the
     encoder would have. The kernel driver hands it each run of the
     user-mode driver's records from the DMA buffer (contiguous, its
     physical address from SubmitCommand) instead of copying them record
     by record on the vCPU; a record whose result is wanted gets its
     return slot written into the buffer, goes alone, and the batch runs
     before the result is copied out. A refused append (`DMA_APPEND` reads
     `NO_ROOM` / `BAD`) falls back to the CPU copy, logged. The executor's
     API and the protocol are unchanged.
   Proved 2026-10-04: D3DGAME9 and D3DGAME8 0 pixels off native,
   D3DFEAT9 byte-identical with native query lines, D3D7TEST equal to
   the host frame, every record through ~85 appends a 5 s line (~800 KiB),
   fences through the interrupt. Left of the step: nothing the driver
   needs; true asynchrony (the host running a batch while the vCPU goes
   on) would be the executor's own change, not planned.
5. **The desktop, basic theme.** Segments (VRAM as one linear segment),
   allocations, paging buffers, DMA submission and fences, VidPN (modes,
   the scanout address in `OFFSET`), the cursor, vertical blank, timeout
   recovery. `cdd.dll` draws GDI and presents through `DxgkDdiPresent`.
   Proved by a screendump of the desktop at every mode the XP driver
   lists.
6. **Direct3D 9 through the user-mode driver.** `OpenAdapter` and the
   `D3DDDI` device functions, emitting the DP2 records the executor
   already decodes. Proved as step 1 was: D3DGAME9, D3DFEAT9 and the DX8 /
   DX7 programs give the native frames byte for byte.
   **Started 2026-10-03.** `wddm/um/d3dptumd.c`, built by
   `build-wddm.cmd` beside the kernel driver (MSVC user mode, static C
   runtime; `docs/build-windows.md`), installed by the same INF. The
   design, all of it in `wddm/d3dpt_wddm.h`:
   - **The host side does not change.** The user-mode driver writes the
     records the XP display driver writes into the window (CTX_CREATE,
     DP2 with the DDI's tokens, DRAW8 draws, READBACK, VRAM_DIRTY), but
     into the runtime's command buffer. Render copies them into a DMA
     buffer behind a registration packet; SubmitCommand moves them into
     the window and rings the doorbell, one batch per submission.
   - **Every video-memory resource is one allocation, VRAM only**, its
     levels / faces inside it at offsets the user-mode driver lays out
     (the XP driver's lightweight-mip layout), carried in the allocation's
     private data. The kernel driver gives each D3D allocation a host
     handle at CreateAllocation and sends VRAM_SURFACE at submit time
     whenever the allocation sits somewhere new to the host; a destroyed
     one is released on the host by the next submission.
   - **Allocations are named by allocation-list index plus a fix-up**
     (D3DPT_PATCH_HANDLE / OFFSET). The handles are written at Render, the
     addresses kept from the last Patch: dxgkrnl's slot-id optimisation
     leaves out patch locations whose allocation did not move since it was
     last patched in that slot, and then calls no Patch at all, so the
     first boots had every buffer after the first carry handle 0.
   - **The caps are the XP driver's DX9 face**, from the same code:
     `core_caps.c` / `core_surf.c` link into the DLL, `umd_core.c` gives
     them a register page holding the ddflags the kernel driver reports
     (QueryAdapterInfo(UMDRIVERPRIVATE)) and answers GetCaps through
     `core_gdi2_answer`.
   - **Its log** goes through `D3DKMTEscape` to the kernel driver and the
     DEBUG register, as `d3dptumd:` lines in the QEMU log.
   - **Tokens before the host context** (d3d9.dll sets every render state
     before it sets a render target) are kept and replayed into the
     context's first DP2 record.
   What the first boots taught: d3d9.dll opens the desktop's shared
   primary (dxgkrnl's own allocation, our private data) with OpenResource
   inside CreateDevice, and a failing OpenResource fails CreateDevice
   (`0x80004001`). With it, D3DGAME9 creates its device (windowed
   640x480, vs / ps 3.0 reported), runs its frames and presents, and with
   the handles written at Render **its scene draws through the host**
   (render-to-texture, textures, particles; ~70 fps under TCG, the host
   at ~3000 draws a second). The windowed present is the backbuffer read
   back into its VRAM, then dxgkrnl's Present blit to the primary.
   - **Results back from the host** (the occlusion query's count): the
     host answers into the window's return area, which the user-mode
     driver cannot see. A record only the kernel driver reads
     (`D3DPT_UMD_OP_RETURN`, before the one whose result is wanted) has it
     give that record a return slot, ring the doorbell right after it and
     copy the answer into a page of VRAM the user-mode driver locks. The
     VRAM offsets the records carry are resolved at submit time from where
     the last Patch saw the allocation, like the registrations.
   - **ColorFill and Blt** (StretchRect, GetRenderTargetData,
     UpdateSurface) are the XP driver's CPU paths: a readback of what the
     host drew, the copy or fill through locks, VRAM_DIRTY. Their pixel
     helpers (`fill_pack`, `px_unpack`, `px_copy`) moved from
     `core_dp2.c` to `core_surf.c` so both drivers link them.
   **Proved 2026-10-03** (`build/w7/w7d3d.sh`, a scratch disk for the
   dumps): **D3DGAME9's frame 300 differs from the native d3d9 frame in 0
   of 307200 pixels, D3DFEAT9's is byte-identical** and its occlusion
   query and getter lines are native's (21316 pixels). d3d9.dll did not
   call StateSet: with a non-pure device it records state blocks itself.
   - **DX8 and DX7** reach the same driver: d3d8.dll opens a device with
     interface 8, ddraw.dll with interface 7, and both ask GetCaps for the
     older caps (type 12, D3DCAPS8, 212 bytes; type 8, the HAL's global
     data, 192; type 11, the extended caps, 116), which the XP driver's
     tables answer at the sizes the runtimes pass. **D3DGAME8's frame is 0
     pixels off the native frame** (2026-10-03). D3D7TEST finds the HAL
     and T&L HAL devices with the XP driver's caps and flips a full-screen
     chain whose second buffer is **the desktop's shared primary**, which
     ddraw.dll opens (OpenResource) and renders into: the kernel driver
     now gives dxgkrnl's primaries a host handle and render-target caps,
     and a plain video-memory surface is a possible target too, as the XP
     driver's DirectDraw surfaces are. That primary also showed that no
     single DDI sees every allocation's place (its Patch is cdd.dll's),
     so the kernel driver keeps each allocation's place from paging
     transfers and fills, every Patch's whole list and SetVidPnSourceAddress.
   **Plan step 6 proved 2026-10-03**: D3DGAME9 and D3DGAME8 0 pixels off
   the native frame, D3DFEAT9 byte-identical with native query / getter
   lines, **D3D7TEST's full-screen frame equal to the host-only frame**
   (`d3dpt-dp2-test`), all four in one boot. Left over, none of it in the
   proof: d3d9 full-screen swap chains (SetDisplayMode is wired,
   untested), render targets on a level or a face, 32-bit indices, the
   depth-to-depth StretchRect, the test loop as a repo tool (its base
   image is still made by hand), and the device's own interrupt (step 4).
   Next is step 7: Direct3D 9Ex, shared surfaces, DWM.
7. **First probes of DWM** (2026-10-03, `w7d3d.sh EXTRA=`): with the
   UxSms service running, applying `aero.theme` answers "This theme can't
   be applied to the desktop", and the driver sees only OpenAdapter,
   GetCaps (D3D9 caps, the format list, the queries) and CloseAdapter: DWM
   refuses composition before it makes a device. Not the cause, each
   tried alone: `HKCU\Software\Microsoft\Windows\DWM` `CompositionPolicy=2`
   (forcing it past the Experience Index); `D3DCAPS2_CANSHARERESOURCE`
   (now claimed: Direct3D 9Ex's sharing is real on this driver, a shared
   resource allocated and freed through the runtime's handle, an opened
   one never freed by the opener); X8R8G8B8 source modes (cdd.dll still
   makes an A8R8G8B8 primary and dxgkrnl drops it in a loop, as the first
   boots found; A8R8G8B8 modes stay, as VirtualBox's WDDM driver has
   them). **DWM's own reason**, from the guest's Application log
   (`wevtutil qe Application`, through the scratch disk): event 9007,
   "The Desktop Window Manager was unable to start because WDDM is not in
   use", then 9009 exits with `0xc00002fe` and `0x80070224`, while the
   same boot runs its desktop and Direct3D 9 on this WDDM driver.
   Windows 7's UAC dialog is not always centred: the test loop answers it
   with Alt+Y.
8. **Why DWM refused: d3d10level9's feature-level test** (2026-10-03).
   Read in the guest's own binaries with Microsoft's public symbols
   (dwm.exe, dwmcore.dll, d3d10level9.dll, win32k.sys of 7601; the
   symbol server serves their PDBs, and they are OMAP-optimised, so an
   address needs the PDB's OMAP_FROM_SRC map). The message is misleading:
   - dwm.exe's `CDwmAppHost::VerifyDisplayModesViaGDI` walks
     `EnumDisplayDevices` and wants win32k's LDDM bit (`StateFlags`
     `0x00800000`, `PDEVOBJ::bLddmDriver`, set when the device answers
     dxgkrnl's IOCTL `0x232033` with type 2) on every attached device. Our
     device has it (`flags=00900005`; the three RDP devices are not
     attached), so that check passes.
   - `VerifyDisplayModesViaMIL` then asks dwmcore for the display's
     `MilGraphicsAccelerationCaps`, and reports the *same* event 9007 when
     its "accelerated" word is 0. dwmcore sets it only after
     `CD3DDeviceTable::GetDeviceCapsForAdapter` has a **Direct3D 10.1
     device** on the adapter: DWM composes through D3D 10.1, which on a
     Direct3D 9-class driver is `d3d10level9.dll` over our user-mode
     driver. The OpenAdapter / GetCaps / CloseAdapter the driver saw was
     d3d10level9 reading our caps and refusing every level, so
     `D3D10CreateDevice1` returned `E_NOINTERFACE` (the probe below).
   - d3d10level9's test (`CapCapsAtFeatureLevel` / `ExceedsCapsBits` over
     `RequiredCaps` tables, one each for 9_1, 9_2, 9_3) wanted, for 9_1,
     what our caps lacked: `D3DCAPS2_FULLSCREENGAMMA`, and cube (and for
     L8 volume) operations on L8, Q8W8V8U8, DXT2 and DXT4 (it maps BC2 and
     BC3 to DXT2 and DXT4, the first D3DFORMAT in its table, not to DXT3 /
     DXT5). Its D3D10 format support is derived from the FORMATOPs
     (`UMAdapter::AddCapsForFormat`); the event and occlusion queries it
     wants we have. 9_2 also wants `D3DPMISCCAPS_SEPARATEALPHABLEND`,
     MaxPrimitiveCount and MaxVertexIndex at 0xFFFFF (so 32-bit indices),
     volume / cube A8, L16, Q16W16V16U16; 9_3 4096 textures, four targets.
   - DWM needs only 9_1: dwmcore's tier 2 (`GraphicsAccelerationTier::
     GetTier`, `g_rgTierRequirements`) takes 4096-texel textures, and below
     9_3 it probes them by creating a 2100-wide R8 texture, which
     d3d10level9 allows up to the hardware's own limit.
   The fix is the user-mode driver's: FULLSCREENGAMMA claimed on the
   device's gamma block, which the kernel driver now loads from
   `UpdateActiveVidPnPresentPath` (the path's 256x3x16 ramp, as the XP
   driver's DrvIcmSetDeviceGammaRamp), and the four formats' extra
   operations added in `umd_format` (the core keeps them 2D for XP and
   9x). With that `D3D10CreateDevice1` at 9_1 succeeds, and DWM goes one
   check further: event 9016, "an analysis of the hardware and
   configuration indicated that it would perform poorly", the Experience
   Index (`VerifyGraphicsAssesment`), which `CompositionPolicy` overrides.
   **With `CompositionPolicy=2` (HKLM and HKCU) DWM composes Windows 7's
   desktop on this driver: Aero glass** (2026-10-03: window frames
   transparent over what is behind them, shadows, `dwm.exe` running in
   the console session, 12-30 composed frames a second under TCG). Two
   user-mode driver bugs DWM found on the way, both making the host
   refuse a batch and lose the context creation inside it, after which
   every DP2 named a missing context (`batch error 3`, the windows'
   contents blank): a buffer whose size is not a multiple of 4 was sent
   with its width unrounded and its pitch rounded (the host wants them
   equal), and SETRENDERTARGET's depth slot was left as the reused command
   buffer had it when there is no depth surface (a stale handle, here a
   vertex buffer's). Also an indexed draw's range now ends at its buffer
   (d3d10level9 passes the whole buffer's vertex count from a base vertex
   inside it, and the host skipped the draw). The executor now names a
   VRAM surface or buffer it refuses (`ddi: surface N refused (why)`).
   Left open then: D3DGAME9 hung under composition (finding 9), the
   Experience Index itself (`winsat dwm`, instead of the policy
   override), and the device's interrupt (DWM paces on the vertical
   blank; the timer stands in).
   Tools for this (throwaway, in the session's scratch): `DDPROBE`
   (EnumDisplayDevices as DWM calls it) and `D10PROBE` (DXGI's adapter,
   `D3D10CreateDevice1` at each level, run under a debug loop for
   OutputDebugString), and a replay of d3d10level9's test on the UMD's
   GetCaps answers dumped to the QEMU log.
9. **A windowed Direct3D 9 program under DWM** (2026-10-03). D3DGAME9's
   window stayed white because DWM could not make the window's
   redirection surface: a shared X8R8G8B8 render-target texture (flags
   0x10881) that DWM creates through Direct3D 10.1 / d3d10level9, and
   the game opens. Every one failed in AllocateCb with `E_INVALIDARG`,
   after our CreateAllocation and OpenAllocation had both succeeded; DWM
   then dropped its device and started over, in a loop. The refusal is
   Windows 7's video memory manager's (dxgmms1.sys,
   `VIDMM_GLOBAL::CreateOneAllocation`, read with its public symbols):
   **a shared allocation marked CpuVisible must have only aperture
   segments** in its segment sets, and ours are VRAM only. Non-shared
   allocations are not checked, so this showed only with the first shared
   resource (CANSHARERESOURCE had been claimed, never used). Three
   changes:
   - The user-mode driver marks a shared resource in its private data
     (`D3DPT_ALLOC_DESC.shared`) and the kernel driver leaves CpuVisible
     off for it. The CPU never maps it.
   - So a Blt to or from it cannot be the CPU's: it goes to the host as
     the DP2 stream's BLT (op 81), which the executor now runs between
     two colour render targets too (StretchRect with the rectangles and
     the filter; **protocol v22**). Windows 7's d3d9 presents a windowed
     swap chain under DWM as exactly that: a Blt from the back buffer
     into the opened redirection surface, flags
     `BeginPresentToDwm | EndPresentToDwm` (0x500), then a Present.
   - With the game drawing, its frame came out 31 % off native (the
     particles over-bright) and DWM gave up composition when it quit:
     the executor's one host device carried each context's render states
     into the other's draws. The executor now keeps each context's state
     in a state block while another has the device (doc 14 "One device,
     many contexts").
   **D3DGAME9 under composition: frame 300 is 0 pixels off the native
   frame**, in an Aero window composed at ~45 frames a second under TCG,
   and DWM keeps composing after it exits (`build/w7/w7d3d.sh` with
   `EXTRA_KEYS=alt-f4`, which closes the Personalization window
   `aero.theme` leaves with the keyboard).
10. **Every program under composition** (2026-10-04, one boot with Aero
   on): D3DGAME8 0 pixels off native, D3DFEAT9 byte-identical with native
   query and getter lines, D3D7TEST's full-screen frame equal to the host
   frame. D3DGAME8 (Direct3D 8) and D3DFEAT9 each present into their
   window's redirection surface through the host's colour BLT, as
   D3DGAME9 does; D3D7TEST's full-screen mode change takes the display
   from DWM and gives it back, and DWM goes on composing after it. No
   driver change was needed. The test loop runs them as one command line
   (`w7d3d.sh CHAIN=1`): typed one per program, a slow first start let
   the next line's keys go to the program still running.
11. **The Experience Index: what DWM checks, and why WinSAT cannot tell
   it under TCG** (2026-10-04, read in dwm.exe, dwmapi.dll and
   dwmcore.dll of 7601 with Microsoft's symbols). Event 9016 is
   `CDwmAppHost::VerifyGraphicsAssesment`. It asks dwmapi's `Assessor`
   (`DwmpGetAssessment`), which is a formula, not a stored score:
   - dwmcore reads two DWORDs from `HKLM\Software\Microsoft\Windows
     NT\CurrentVersion\WinSAT`, `VideoMemoryBandwidth` and
     `VideoMemorySize` (a missing value reads 0); `winsat dwm` writes them.
   - The desktop's cost per frame is a fitted quadratic in its width
     (0.4594 w^2 + 1538.1 w + 196 for one monitor with transparency, the
     first try; 0.4594 w^2 + 18.783 w + 196 without, the second) times 4
     bytes; at 30 frames a second it must take at most 65 % of the time at
     `VideoMemoryBandwidth / 1000` MiB/s. At 1024x768 that is a value of
     at least about 362000 (about 88000 without transparency). Video
     memory must hold twice the desktop's surfaces, and system memory be
     512 MB or more.
   - `CompositionPolicy` (HKCU / HKLM `Software\Microsoft\Windows\DWM`)
     is that check's switch, `DwmpGetAssessmentUsage`: 0 asks the
     Assessor, 2 composes regardless (our override), 1 never.
   `winsat dwm` runs on this driver to the end ("Assessment Completed",
   24-33 s), but under TCG it reports `TSC Frequency : 0`, every CPU rate
   as an absurd number and **Video Memory Throughput 0.00 MB/s**, and
   writes `VideoMemoryBandwidth` 0 (Windows itself records the CPU at
   3697 MHz, so it is WinSAT's own timer calibration that fails on the
   emulated CPU, not the display driver). DWM then refuses as before. One
   boot composed after WinSAT anyway; the next, the same commands, did
   not, so that is not counted. What is left: WinSAT under KVM (the user's
   `win7` bundle on Linux), where its timer is real; or the launcher (or
   SETUP) setting `CompositionPolicy=2` for a Windows 7 machine on the
   WDDM driver, which is what the measured throughput would decide anyway
   if the host's GPU is behind it. **Decided 2026-10-04 (user): SETUP
   sets the override** (finding 12).
12. **SETUP installs the WDDM driver and turns Aero on** (2026-10-04,
   `setup.c`). The display-driver component, on 32-bit Windows 7 with
   `WDDM\` on the disc and the adapter's interrupt, runs `DRVINST.EXE` on
   `WDDM\D3DPTKMD.INF` instead of the XP INF, then writes
   `CompositionPolicy=2` to `Software\Microsoft\Windows\DWM` in HKLM and in
   the HKCU of the user running it (DWM reads HKCU first and HKLM only
   where a user has no value, and a Windows 7 profile has one, 0; read in
   dwmapi's `DwmRegGetPreferenceDword`). The interrupt is found as an IRQ
   in the configuration Plug and Play allocated to the present
   `VEN_1234&DEV_3D00` devnode (cfgmgr32 at run time, so SETUP still
   starts on 98). 64-bit Windows 7, a disc with no `WDDM\` or an adapter
   with no interrupt get the XP driver, and the log says which and why.
   Proved in one boot: SETUP's log "Windows 7: the WDDM driver ..., for
   Aero", drvinst "installed" (the unsigned-driver prompt clicked on the
   tablet, `w7d3d.sh EXTRA_CLICK=450,370`), CompositionPolicy 2 in both
   hives, and after DWM's service restarted and `aero.theme` with no
   override of our own, D3DGAME9 composed (DWM ~50 flips a second while
   it ran, the window's shared surface made and opened) and 0 pixels off
   native. Seen on the way, not chased: after drvinst reinstalls the
   driver, the user-mode driver's log lines (its escape to the kernel
   driver) no longer reach the QEMU log, while the kernel driver's do.
   (Explained by finding 14: that boot's image still had
   `UserModeDriverName` from the hand installs, which the INF never wrote.)
13. **A Windows 7 family in the launcher** (2026-10-04, user;
   `bundle::Family::Win7`, doc 06 "Windows 7"). The era PC with `-cpu max`
   (what this track's work ran on; Windows 7's software wants SSE2),
   `d3dpt-vga,irq=on` by default (the standard VGA the other choice; no
   Cirrus driver in Windows 7), HD Audio, an e1000 when networked, 2 GB
   (1-3 GB), a 40 GB disk, Automatic acceleration; the family note says
   32-bit and SETUP for Aero, the network note says 2020 for its last
   security update. `launcherx --print-args` on such a bundle gives
   `-cpu max ... -device d3dpt-vga,addr=0x02,irq=on -device
   e1000,netdev=n0,addr=0x03 -device ich9-intel-hda,addr=0x1b -device
   hda-duplex,audiodev=embed0`; the mitsuami form shows it (`wizard:win7`).
   The user's `win7` bundle is XP-family (Cirrus, AC'97, no interrupt), so
   it stays on the XP-model driver until it is remade as this family.
14. **A fresh install, end to end, on that family** (2026-10-04, on the
   PC under TCG). A machine from the launcher's own form
   (`launcherx --wizard-new win7 "Windows 7" 40`), booted with exactly
   `launcherx --print-args` (the player's audio backend swapped for a null
   one), Windows 7 Ultimate N SP1 x86 in its CD drive and an answer file
   on a USB stick: Windows setup to its first desktop in 14-19 min; then
   the guest-tools disc, `SETUP /ALL` from an administrator's console (the
   unsigned-driver prompt clicked), one restart, and **Aero composes by
   itself**: no theme applied, no service restarted, the desktop at
   1024x768 from the driver's EDID, glass taskbar and Start menu,
   `dwm.exe` with `d3dptumd.dll` loaded. Two bugs the earlier proofs hid,
   because their image had been through hand installs:
   - **SETUP saw no interrupt on a fresh install** and gave the XP driver:
     before any driver of ours the adapter's devnode has no IRQ in its
     *allocated* configuration. SETUP now also looks in its boot
     configuration (where the fresh install had it) and its requirements,
     and logs which one (`setup.c` `adapter_has_irq`).
   - **The INF never wrote `UserModeDriverName`**: it sat in an XP-style
     `[d3dpt_Install.SoftwareSettings]` section, which Windows 7 does not
     process for this driver, so no process loaded the user-mode driver
     and DWM ran without composing (a 3.7 MB `dwm.exe`, Basic look). The
     `AddReg` is now in the install section, whose `HKR` is the adapter's
     software key.
   Two traps of the answer file, not of ours: Windows setup reads
   `autounattend.xml` only from the root of a *removable* drive (QEMU's
   `usb-storage` needs `removable=on`; a floppy is tried by SeaBIOS before
   the CD and hangs the boot), and this disc's `install.wim` holds five
   editions, index 1 being Starter N ("This edition of Windows doesn't
   support themes"), so the image is chosen by name (`Windows 7
   ULTIMATEN`, as `sources\ei.cfg` names the disc's own). The run is now
   the repo's `tools/win7-aero-test.sh` (finding 15).
15. **The test loop as a repo tool** (2026-10-04):
   `tools/win7-aero-test.sh <win7-x86.iso>`, and `scripts/test.sh`'s
   `win7-aero` check when `WIN7_ISO` names the disc (skipped otherwise).
   The base (Windows setup, unattended, on a machine from `launcherx
   --wizard-new win7` in a library under `build/win7-aero-test`) is made
   once per disc and edition and kept; each run boots an overlay of it
   with the launcher's arguments and the guest-tools disc, runs SETUP, the
   restart, and gives four verdicts: SETUP chose the WDDM driver, the
   kernel driver started, `dwm.exe` has `d3dptumd.dll` loaded, and
   D3DGAME9's frame 300 under composition against the native d3d9 frame
   (from a FAT scratch disk the guest finds by a tag file, whatever letter
   Windows gives it). First run on the PC under TCG: the base in 936 s,
   the run in ~12 min, all four PASS, **D3DGAME9 0 of 307200 pixels off
   native on a fresh install**. The edition comes from the disc's
   `sources\ei.cfg`, which lives in UDF: xorriso and bsdtar see only the
   ISO 9660 stub (`README.TXT`), so `tools/udfcat.py` reads it (stdlib
   Python, one file to stdout).
7. **Direct3D 9Ex, shared surfaces, DWM: Aero.** Done for 32-bit Windows
   7: DWM composes the desktop (finding 8), every test program draws in it
   with native frames (findings 9 and 10), paced by the device's own
   vertical-blank interrupt (plan step 4); SETUP sets DWM's
   `CompositionPolicy` override, since the Experience Index cannot be
   measured under TCG (findings 11, 12), the launcher's Windows 7 family
   gives the adapter its interrupt (finding 13), and a fresh install on
   that family reaches Aero with SETUP and one restart (finding 14),
   checked by `tools/win7-aero-test.sh` (finding 15). Left: the loose ends
   of step 6 (full-screen d3d9 swap chains, render targets on a level or
   face, 32-bit indices, the depth StretchRect) and step 5 (timeout
   recovery).

After Aero, not planned yet: 64-bit (test mode, or signing, which on
64-bit Windows 10/11 means an EV certificate and Microsoft's attestation
signing), and Windows 10/11 (M20). Those keep the kernel driver's model,
and DWM there should run on a Direct3D 9-class user-mode driver through
`d3d10level9` (unchecked: whether Windows 11 still loads a WDDM 1.x
kernel driver). A Direct3D 11 user-mode driver is its own project.

**Test loop.** `tools/win7-aero-test.sh <win7-x86.iso>` (finding 15), the
`win7-aero` check of `scripts/test.sh` with `WIN7_ISO` set: Windows 7 from
its disc on the launcher's Windows 7 family, SETUP, Aero, D3DGAME9 under
composition. Run on the PC under TCG; written for Linux too (KVM there,
with an ISO carrying the PC's driver, `scripts/wddm-prebuilt.sh`), not
yet run there. The XP-model
driver on Windows 7 is still `tools/xp-driver-test.sh` on an XP-family
bundle.
