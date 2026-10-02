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
2. **An empty kernel driver that loads.** `guest-tools/src/d3dptvid/wddm/`
   (`km/` the kernel driver, `um/` the user-mode one later), an MSBuild
   project on the WDK's kernel-mode toolset, an INF for `d3dpt-vga`'s PCI
   ID. `DriverEntry` calls `DxgkInitialize` with every callback stubbed;
   `DxgkDdiStartDevice` logs through the device's DEBUG register (doc 15's
   rule: the QEMU log, never a debugger). Proved when Windows 7's Device
   Manager shows our adapter started, with the XP-model driver as the
   fallback a reinstall brings back.
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
