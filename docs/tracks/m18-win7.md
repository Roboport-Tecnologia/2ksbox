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

The branch is `track/m18-win7` again, from `main` at the M21 merge. The
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
4. **The device's additions** (`d3dpt/hw/d3dpt_vga.c`, `d3dpt/d3dpt_fb.h`,
   with M7): an interrupt line, a fence register, DMA command buffers read
   from guest memory. Registers are only added (`D3DPT_FB_VERSION`), so
   XP, Win98 and old snapshots see no change.
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
   Windows 7's UAC dialog is not always centred: the test loop answers it
   with Alt+Y.
7. **Direct3D 9Ex, shared surfaces, DWM: Aero.**

After Aero, not planned yet: 64-bit (test mode, or signing, which on
64-bit Windows 10/11 means an EV certificate and Microsoft's attestation
signing), and Windows 10/11 (M20). Those keep the kernel driver's model,
and DWM there should run on a Direct3D 9-class user-mode driver through
`d3d10level9` (unchecked: whether Windows 11 still loads a WDDM 1.x
kernel driver). A Direct3D 11 user-mode driver is its own project.

**Test loop.** Open in step 3. The headless loop is
`tools/xp-driver-test.sh` on Linux (KVM, the `win7` bundle); a driver
built on the PC is either carried there, or the PC boots the bundle with
its own 2ksbox build (`scripts/win-run.sh`) by hand.
