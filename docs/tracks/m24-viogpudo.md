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

1. **The vertical blank, from a timer (patch 01, 2026-10-09: written,
   not built yet).** `DxgkDdiControlInterrupt` and `DxgkDdiGetScanLine`;
   each mode at `VSyncHz` (60) with a 1/20 blank; while dxgkrnl has the
   interrupt on, an `EX_TIMER_HIGH_RESOLUTION` timer raises
   `DXGK_INTERRUPT_DISPLAYONLY_VSYNC` through
   `DxgkCbSynchronizeExecution` and queues the DPC (which already calls
   `DxgkCbNotifyDpc`). `VSyncHz` under
   `HKLM\SYSTEM\CurrentControlSet\Services\VioGpuDod\Parameters`, read at
   `DriverEntry`: 0 is upstream's behaviour exactly, so the A/B is a
   registry value and a restart, not a reinstall.
2. **Build it on the PC** (next): `scripts/build-viogpudo.sh build`,
   then `publish`. It was never compiled; expect fixes (WPP's format
   checks, warnings as errors, the EWDK's ARM64 tools).
3. **Install and measure on the Air** (Windows 11 on Arm, an overlay of
   the user's machine): `build-viogpudo.sh fetch && build-viogpudo.sh
   iso`, the ISO in the machine's CD drive, `viogpudo-install.ps1`,
   restart. Checks, in order:
   - Windows loads it (Device Manager: no code 52; no TDR in the first
     minutes, which would mean the vertical blank is missed).
   - Settings > Display > Advanced display: 60 Hz, not 1 Hz.
   - DWM's pace: a probe calling `DwmGetCompositionTimingInfo`
     (`rateRefresh`, `qpcRefreshPeriod`, frames composed per second)
     with `VSyncHz` 60 and 0. Not written yet.
   - The host: `tools/win11-frames-test.py`'s published rate and the
     interval between guest flushes while a window moves (16.7 ms, not
     15.6 ms; the tool counts frames today, the intervals are to add).
   - The user's eye, `VSyncHz` 60 against 0.
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

The PC builds (`build-viogpudo.sh build`, ~2 min after the first
fetch), publishes, and the Air fetches and tests; x64 Windows 11 can be
tested the same way on Linux (KVM), where the drivers disc has
viogpudo too. Never on the user's own image: an overlay
(`qemu-img create -f qcow2 -b <image> -F qcow2 <overlay>`), as for every
Windows 11 test here.

## Open

- Whether DWM paces on a display-only driver's vertical blank at all, or
  keeps its own timer: step 3 answers it.
- Upstream's `AddSingleTargetMode` copies `TotalSize` into `ActiveSize`
  before `BuildVideoSignalInfo` fills it, so target modes have a zero
  active size; patch 01 sets it in its own mode only.
