# Track M24: our viogpudo, a vertical blank for Windows 11's desktop

Opened 2026-10-09 (user: "let's see if we can solve the windows 11
choppiness by optimizing the official virtio display driver"; then "test
signing is fine"). Branch `track/m24-viogpudo`.

## Why

M22 step 7 (`tracks/m22-mitsuami-player.md`) took the player's side of
Windows 11 on Arm's choppy desktop as far as it goes: every guest flush
shown at once (embed v12 `on_flush`), each viogpudo present copied before
the next (QEMU patch 86, `sync-ctrl`), only what changed copied. The
user saw patch 86 help and nothing else. What M22 measured and left
open: **Windows reports 1 Hz and DWM composes on a 64 Hz timer.**

The cause is in viogpudo (virtio-win's display-only driver):
`BuildVideoSignalInfo` gives every mode `D3DKMDT_FREQUENCY_NOTSPECIFIED`.
A display-only driver must do that when it reports no vertical blank of
its own, and dxgkrnl then simulates one; DWM ends up on the 15.6 ms
system tick. **A 64 Hz guest on a 60 Hz host screen drops a frame about
four times a second**, which no amount of publishing on the host can
hide. The WDK's rule ("Saving Energy with VSync Control"): a KMDOD that
implements both `DxgkDdiControlInterrupt` and `DxgkDdiGetScanLine` must
give real `PixelRate`, `HSyncFreq` and `VSyncFreq` and report
`DXGK_INTERRUPT_DISPLAYONLY_VSYNC` at that rate (dxgkrnl's
`TdrDodVSyncDelay` watchdog, 2 s, otherwise); one that implements neither
must leave all three unspecified. viogpudo implements neither.

## Signing (user decision 2026-10-09)

Windows loads a changed viogpudo only test signed: 64-bit Windows takes
no kernel driver without a trusted signature, and Microsoft's is only on
upstream's builds. **Test signing is fine for now**: 2ksbox's Windows 11
machines have Secure Boot off (no keys enrolled), so `bcdedit /set
testsigning on` works, with Windows' "Test Mode" watermark. Shipping it
needs either Microsoft's attestation signing (Partner Center, an EV
code-signing certificate for Roboport Tecnologia) or the change upstream
in virtio-win (Red Hat signs its releases); decided after the
measurement says whether it is worth it.

## Scope and files

- `patches/viogpudo/`: our patch queue on upstream
  `virtio-win/kvm-guest-drivers-windows` at `fbcc19d7` (the last commit
  before 0.1.302's viogpudo, the one on our drivers disc), and its README.
- `scripts/build-viogpudo.sh`: the source (any host), the build (the PC,
  MSYS2, an EWDK for Windows 11), publish / fetch by source hash (as
  `wddm-prebuilt.sh`), and a test ISO.
- `guest-tools/viogpudo/viogpudo-install.ps1`: test mode, a certificate,
  signing, upstream's viogpudo out of the store, ours in.
- This doc; the `build-windows.md` section "Our viogpudo".

Not here: QEMU, the player, the launcher, the drivers disc
(`build-virtio-win.sh` keeps shipping upstream's signed driver).

## Steps

1. **The vertical blank, from a timer (patch 01, 2026-10-09: done on
   x64).** `DxgkDdiControlInterrupt` and `DxgkDdiGetScanLine`; each mode
   at `VSyncHz` (60) with a 1/20 blank; while dxgkrnl has the interrupt
   on, an `EX_TIMER_HIGH_RESOLUTION` timer raises
   `DXGK_INTERRUPT_DISPLAYONLY_VSYNC` through
   `DxgkCbSynchronizeExecution` and queues the DPC (which already calls
   `DxgkCbNotifyDpc`). `VSyncHz` under
   `HKLM\SYSTEM\CurrentControlSet\Services\VioGpuDod\Parameters`, read at
   `DriverEntry`: 0 is upstream's behaviour exactly, so the A/B is a
   registry value and a restart, not a reinstall. The timer as first
   written was periodic, and its lateness added up: DWM composed every
   16.79 ms (59.6 Hz), a frame slipped every two or three seconds against
   a 60 Hz screen. It is now one-shot, re-armed toward an absolute
   schedule on the QPC, beside a second high-resolution timer that does
   nothing but stay armed (without it the clock left its finest resolution
   between expiry and re-arm, and blanks came up to a 15.6 ms tick late,
   intervals from 2 to 57 ms).
2. **Built on the PC (2026-10-09).** Not with an EWDK: the WDK and SDK
   from NuGet with Visual Studio 2022 (user decision; `build-windows.md`
   "Our viogpudo"), ~30 s for both drivers. The only fix the first build
   needed: the union member is `DisplayOnlyVsync`.
3. **Measured on x64 Windows 11 on the PC (2026-10-09; the user: "we
   will compile and test this here for windows x64 as well").** The test
   machine (`D:ms\win11-test`, `run-m24.sh` there: an overlay of
   `drv.qcow2`, the launcher's standard VGA plus
   `virtio-gpu-pci,sync-ctrl=on`, WHPX), `viogpudo-install.ps1` through
   the command channel, `guest-tools/viogpudo/dwm-pace.ps1` as the
   logged-on user. Windows loads it (no code 52, no TDR), and the mode
   reads 60 Hz where upstream's reads 1 Hz. DWM's pace by `DwmFlush`,
   300 compositions with a window animating:

   | `VSyncHz` | DWM's period | mean | median | 5th–95th percentile |
   |---|---|---|---|---|
   | 0 (upstream) | 15.625 ms | 16.14 ms | 16.04 ms | 15.70–16.34 ms |
   | 60, periodic timer | 16.667 ms | 17.01 ms | 16.79 ms | 16.58–17.22 ms |
   | 60, the schedule | 16.667 ms | 16.66 ms | 16.65 ms | 16.26–17.09 ms |

   So DWM does pace on a display-only driver's blank. Two things found on
   the way: **on x64 the standard VGA's screen is the primary** (DISPLAY1,
   Basic Display, 1280x800) and the virtio-gpu's the second, and only the
   primary is DWM's clock; and Windows 11's
   `DwmGetCompositionTimingInfo` counters move once a second whatever DWM
   does (`DwmFlush` is the measure). Still to do: the Air (Windows 11 on
   Arm), the host side (`tools/win11-frames-test.py`'s intervals) and the
   user's eye, `VSyncHz` 60 against 0.
4. **The host's own vertical blank (if step 3 helps).** A free-running
   60 Hz guest still drifts against the host screen's 60 Hz, a skipped
   or doubled frame every few seconds. The real fix is the player's
   display link (CVDisplayLink, the swapchain's present timing) driving
   the guest's blank: a virtio-gpu extension in QEMU (an interrupt or an
   event per host refresh, negotiated by a feature bit) that this driver
   takes instead of its timer when present, and `VSyncHz` following a
   120 Hz ProMotion screen. Needs its own design here first.
5. **Then the decision on shipping** (attestation signing, upstream, or
   both) with the numbers in hand.

## Test loop

The PC builds (`build-viogpudo.sh build`, ~30 s after the first
fetch), publishes, and the Air fetches and tests; x64 Windows 11 can be
tested the same way on Linux (KVM), where the drivers disc has
viogpudo too. Never on the user's own image: an overlay
(`qemu-img create -f qcow2 -b <image> -F qcow2 <overlay>`), as for every
Windows 11 test here.

## Open

- A 144 Hz host screen (the PC's): 60 guest frames on 144 refreshes
  judder 2-3-2-3 whatever the guest does; `VSyncHz=72` or 144 would
  divide it, and step 4 makes it follow the screen.
- x64's two screens: the launcher's x64 machine keeps the standard VGA
  for setup and recovery, whose screen Windows makes the primary; with
  viogpudo in, the virtio-gpu's should be the only one.
- Upstream's `AddSingleTargetMode` copies `TotalSize` into `ActiveSize`
  before `BuildVideoSignalInfo` fills it, so target modes have a zero
  active size; patch 01 sets it in its own mode only.
