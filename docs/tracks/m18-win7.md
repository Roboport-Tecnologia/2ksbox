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

The user's decisions (2026-09-27, ADR-022):

1. **Headers: our own.** Neither mingw-w64 nor Wine ships the WDDM driver headers
   (`d3dkmddi.h`, `d3dumddi.h`, `dispmprt.h`), and `DxgkInitialize` comes
   from the WDK's `displib.lib`. Writing our own subset from Microsoft's
   public documentation keeps the Linux cross build and the open-source
   rule (chosen); the WDK on the PC was the other option.
2. **32-bit first.** The user's Windows 7 is 32-bit, which loads an
   unsigned driver after a prompt. 64-bit Windows 7 refuses unsigned kernel
   drivers outside test mode.
