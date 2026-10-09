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
  MSYS2, the WDK from NuGet) and a test ISO; `scripts/windows-drivers.sh`
  publishes the PC's build by source hash and fetches it elsewhere, beside
  M18's WDDM driver.
- `guest-tools/viogpudo/viogpudo-install.ps1`: test mode, a certificate,
  signing, upstream's viogpudo out of the store, ours in.
- This doc; the `build-windows.md` section "Our viogpudo".

Not here: the launcher. Step 4 needs small pieces of QEMU (a
virtio-gpu patch), the embed API and the player, and the resolution
service touched the drivers disc's `install.ps1` (M23's); each is named
in its commit. `build-virtio-win.sh` keeps shipping upstream's signed
driver.

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
   does (`DwmFlush` is the measure).

   **The user's eye, on the PC's 144 Hz screen (2026-10-09):** 60 "much
   better" than upstream; **72 "very smooth"** (each guest frame two host
   refreshes, where 60 alternates two and three; DWM holds 13.88 ms, 13.46
   to 14.30, once Windows has settled, 36 Hz in the first minute after
   boot, "took a while to settle"); upstream (`VSyncHz=0`) again "is
   choppier". So the blank's rate should divide the host screen's, which
   step 4 makes automatic. Still to do: the Air (Windows 11 on Arm) and
   `tools/win11-frames-test.py`'s intervals there.

   **The driver's own cost (the user: "see if it's not something slow in
   the driver itself, like unnecessary copying").** Per present, viogpudo
   copies each move and dirty rectangle from DWM's surface into the
   framebuffer (`CopyBits32_32`, a `RtlCopyMemory` per row, the one copy
   a display-only driver cannot avoid), then queues
   `TRANSFER_TO_HOST_2D` and `RESOURCE_FLUSH` for the rectangles'
   bounding box, without waiting on either (two notifies, which patch
   86's `sync-ctrl` serves inside the exit). QEMU's
   `virtio_gpu_cmd_res_xfer_toh_2d` and `virtio_gpu_cmd_res_flush` trace
   events (`-msg timestamp=on -D <log>`, switched on over QMP with
   `trace-event-set-state`; Windows' timestamps are ~1 ms coarse) over 18 s
   of the probe: 628 presents, one every 16.66 ms (median, 5th to 95th
   percentile 15.8 to 17.9), the host's transfer of a whole 1792x1344
   screen ~1 ms and of the probe's 384x261 window under the clock's
   resolution. The full-screen updates (112) come in bursts of a few
   hundred milliseconds while windows open (Windows' animations redraw
   most of the screen), not from the bounding box: once the window is up
   every present is the window alone. Nothing in the driver is slow
   enough to matter at 60 Hz; the bounding box (two rectangles at
   opposite corners send the screen between them) is the one thing to
   change if a workload shows it.
   **The desktop following the window (the user, 2026-10-09: "automatic
   resizing the windows guest when I resize the window is not working").**
   Not the driver, which already does its half: the device's display
   event makes it ask for the new size (`GET_DISPLAY_INFO`), put it in
   its custom mode and signal `Global\VioGpuResolutionEvent<n>`. A kernel
   display-only driver cannot change the desktop's mode; upstream's user
   half does: `vgpusrv` (a service) starts `viogpuap` in the console
   session, which waits on that event, asks the driver for the size
   (`VIOGPU_GET_CUSTOM_RESOLUTION` escape) and applies it with
   `SetDisplayConfig`. virtio-win ships both beside the driver, and our
   drivers disc has carried them in `$WinPEDriver$\viogpudo` all along,
   but nothing installed the service (Windows Setup installs only what
   the INF names), so the desktop took the window's size at boot alone
   (M22's "viogpudo 0.1.302 takes it only when it starts"). The drivers
   disc's `2ksbox\install.ps1` now installs it (for upstream's signed
   driver too), `build-virtio-win.sh` checks the two files are there, and
   `build-viogpudo.sh` and `viogpudo-install.ps1` carry and install the
   ones built with ours.

   Run on the PC's test machine (2026-10-09, a fresh overlay of
   `drv.qcow2`, the drivers disc's `2ksbox\install.cmd` as SYSTEM): the
   service ran, `viogpuap` ran in the console session, and still the
   desktop stayed at its size while the guest asked QEMU for the new one
   (`virtio_gpu_cmd_get_display_info` traced). Upstream's `viogpuap`
   finds virtio-gpu's display devices by walking `EnumDisplayDevices`,
   and `FindDisplayDevice` returned FALSE at the first device that was
   not virtio-gpu's, which ended the walk: with the standard VGA (or
   ramfb) enumerated first it found nothing. **Patch 03** walks every
   device; with it the desktop followed the window at once (1928x1266,
   then 1778x1116 with our driver). `build-virtio-win.sh` now puts our
   `vgpusrv` / `viogpuap` on the drivers disc over upstream's (from
   `build/viogpudo`, built or fetched by `windows-drivers.sh`; upstream's
   with a note when there is none), so upstream's signed driver gets the
   fix too.

4. **The host's own vertical blank (design, 2026-10-09; step 3 helped).**
   Step 3 showed two things a fixed `VSyncHz` cannot give: the rate
   should divide the host screen's (72 on the PC's 144 Hz was "very
   smooth", 60 there alternates two and three refreshes a frame), and a
   guest timer, however exact, free-runs against the screen's real rate
   (a "144 Hz" screen is 143.9x Hz), so its frames slide across the
   host's vertical blank and one lands a refresh late every so often. Two
   phases, each worth having alone:

   **A. The rate follows the screen** (2026-10-09: done on the PC; embed
   API v15, `player-core/src/screen.rs`, patch 02; the screen's rate is
   read on Windows and macOS so far, so Linux sends 60 Hz; on the Air the
   player logs `host screen at 60.002 Hz`, Core Video's nominal period). On the
   PC's 144 Hz screen the player logs `[display] host screen at 144.000
   Hz: the guest's blank at 72.000 Hz`, the driver takes 71.999 Hz from
   the EDID (its pixel clock is in 10 kHz steps), and DWM composes every
   13.885 to 13.888 ms (three runs of 300 `DwmFlush`es, 5th to 95th
   percentile 13.46 to 14.33 ms, none doubled), also after a resize. No new device interface: QEMU's
   `QemuUIInfo` already has `refresh_rate` (mHz), and virtio-gpu already
   writes it into the EDID it gives the guest (the preferred timing's
   pixel clock; 75 Hz when unset).
   - The player learns its window's screen's refresh, exactly (the
     rational the system reports, not a rounded integer): Windows
     `QueryDisplayConfig`'s `vSyncFreq` for the window's monitor (or
     `DwmGetCompositionTimingInfo`'s `rateRefresh`), macOS the
     `CVDisplayLink`'s nominal period (`NSScreen.maximumFramesPerSecond`
     for ProMotion), GTK `gdk_monitor_get_refresh_rate`; again when the
     window moves to another screen. It picks the guest's rate as the
     screen's divided by the smallest whole number that brings it to at
     most a cap (90 Hz: 144 → 72, 120 → 60, 165 → 82.5, 60 → 60, 240 →
     80; `PLAYER_GUEST_HZ_MAX` overrides).
   - Embed API v15: `qemu_embed_set_refresh_rate(e, mhz)`, carried to the
     console's UI info by the same bottom half as v9's window size, so the
     device raises its display event with a new EDID.
   - viogpudo, patch 02: with no `VSyncHz` in the registry, the blank's
     rate is the EDID's preferred timing's (pixel clock over the total
     size), read at start and on each display event; the timer's schedule
     restarts at the new rate, and the modes report it. `VSyncHz` stays
     the override (and 0 still upstream's behaviour);
     `viogpudo-install.ps1` without `-VSyncHz` now removes the value,
     so an earlier A/B's does not pin the rate.

   **B. The phase follows the screen** (2026-10-09: done on the PC; QEMU
   patch 90, embed API v16, `screen::HostVBlank`, viogpudo patch 04; the
   blank is waited on on Windows and macOS so far, macOS through a
   `CVDisplayLink` for the window's screen; on Windows 11 on Arm the
   guest stalled with it until QEMU patch 91, below). Measured on the test
   machine at the PC's 144 Hz screen: DWM composes every 13.884 to
   13.887 ms (three runs of 300 `DwmFlush`es, 5th to 95th percentile
   13.69 to 14.12 ms, against 13.46 to 14.33 from the timer alone). The
   proof that the host's blanks drive it: with the driver's own timer
   held at 60 Hz (`VSyncHz=60`) every interval is still a whole number
   of 13.89 ms (13.9 or 27.8; DWM composes on the host's ticks and skips
   one now and then for its 60 Hz), where the timer would give 16.67.
   Built as designed below, with one change: the ticks are turned on and
   off by a control command from the driver's worker thread, which also
   now runs `ConfigChanged` only for a real display event (a tick must
   not make `viogpuap` resync the resolution). `PLAYER_HOST_VBLANK=0`
   is the A/B. The user's eye, with all of it in (2026-10-09): "it's
   super smooth, desktop resizes correctly". The host's blank raises the
   guest's, so a frame never
   slides across it:
   - The player calls `qemu_embed_vblank(e)` (v15) on each host refresh
     that starts a guest frame (every Nth of A's divisor), from wherever
     its toolkit hears the blank (Windows `IDXGIOutput::WaitForVBlank` on a
     thread of its own, or `DCompositionWaitForCompositorClock`; macOS the
     `CVDisplayLink` callback; GTK the frame clock's `update`). Any thread;
     the library schedules a bottom half.
   - QEMU, a new patch on virtio-gpu: a feature bit of ours,
     `VIRTIO_GPU_F_HOST_VBLANK` (a high device-specific bit, documented as
     2ksbox's and dropped if upstream ever takes the number), and an event
     bit in `events_read` (`VIRTIO_GPU_EVENT_VBLANK`). With the feature
     negotiated and the guest's interrupt on, each `qemu_embed_vblank`
     sets the bit and raises the config interrupt. The guest turns the
     ticks on and off with a control command of ours
     (`VIRTIO_GPU_CMD_SET_VBLANK`, in a range upstream does not use),
     sent when dxgkrnl turns the interrupt on or off (from
     `DxgkDdiControlInterrupt`, or through the driver's worker thread
     if that is called above `PASSIVE_LEVEL`, where the control queue
     cannot be used), so an idle desktop costs the host nothing.
   - viogpudo, patch 03: when the device offers the feature, the
     config-change interrupt (MSI vector 0) with the vblank bit raises
     `DXGK_INTERRUPT_DISPLAYONLY_VSYNC` from the interrupt routine itself
     and clears the bit; the timer stays for a device without it, and as
     a watchdog that fills in if the host stops ticking (a minimized
     window gets no blanks; dxgkrnl's `TdrDodVSyncDelay` resets a driver
     that misses them for two seconds).
   - Latency: the guest composes right after the host's blank and its
     frame is shown at the next one (or the one after on a divided
     rate), a fixed delay instead of a drifting one.

   Order: A, then B. Neither needs QEMU's 9.x-era `retrace` or anything
   of the standard VGA. Files outside this track (the embed API, a QEMU
   patch, `player-core`, the front ends' refresh hooks) are touched
   minimally and named in each commit, per the tracks rule. Testing waits
   on the user (no guests started meanwhile): the PC's 144 Hz screen,
   `dwm-pace.ps1` for the guest's pace, QEMU's flush trace for the host's
   cadence, and the user's eye.
   **On the Air, Windows 11 on Arm (2026-10-09).** An overlay of the
   launcher's `win11` machine (build 26100, Home), `viogpudo-test.iso` as
   its first disc. Four findings:
   - *Smart App Control blocks test signing.* The machine had it in
     Evaluation (`HKLM\SYSTEM\CurrentControlSet\Control\CI\Policy`
     `VerifiedAndReputablePolicyState` = 2): `bcdedit` read `testsigning
     Yes`, Secure Boot was off, and still the kernel's code integrity
     options (`NtQuerySystemInformation` class 103) were 0x5, no
     `TESTSIGN` (0x2), and `pnputil` refused our package ("The publisher
     of an Authenticode(tm) signed catalog was not established as
     trusted"; setupapi.dev.log: "signer is not trusted by system, and
     Code Integrity is enforced"). With the state set to 0 and a restart,
     Test Mode came on and ours installed. Smart App Control cannot be
     turned on again without reinstalling Windows, which weighs on step 5:
     a test-signed driver asks that of every user who has it on.
   - *`viogpudo-install.ps1` removed upstream's driver before ours was
     accepted*, so the refusal left the virtio-gpu with no driver: a black
     window (the player stayed on the virtio-gpu's console), then, after a
     restart, Basic Display on ramfb drawn at the firmware's 800x600 into
     a ramfb the player reads at 1280x800 (garbled).
   - *The driver's timer alone is choppy on Arm* (the user: "it's
     choppy"; `PLAYER_HOST_VBLANK=0`). With ramfb's Basic Display still
     extended beside ours, DWM composed on dxgkrnl's simulated 64 Hz blank
     (`rateRefresh` 24000000/375000; `DwmFlush` median 16.95 ms, 5th–95th
     percentile 15.6–30.3 ms, 31 of 300 at 28–37 ms). With the
     virtio-gpu's screen the only one (`DisplaySwitch /internal`; the
     guest agent had not made it so on this machine) DWM took our 60.001
     Hz, and the pace got worse: 146 intervals at 16–17 ms, ~110 at
     30–35, 23 at 0–2. Blanks bunch on a coarse tick: the second
     high-resolution timer that keeps x64's clock fine (step 1) does not
     here, under HVF.
   - *With the host's blank the guest's display stalls.* Our driver
     takes the screen (the player follows the virtio-gpu at the window's
     size) and the window stays black; Windows itself runs (the logon
     sound, AHCI traffic), but QEMU's trace shows not one virtio-gpu
     command, not even cursor updates, and one vCPU is busy in the guest
     kernel (`hv_trap` in the player's samples). The host side is idle
     (the `CVDisplayLink` thread waits for each refresh, QEMU's main loop
     polls). MSI-X is on (the `virt` board's emulated ITS), so the
     config interrupt is message 0 as on x64. Same driver and device code
     as the PC's, where it works.

   **The stall was lost interrupts: QEMU patch 91 (2026-10-09).** Once
   the guest froze (this time after the user had logged in: "it even
   worked for a bit"), QMP hung in `run_on_cpu` and the player's samples
   showed three vCPUs asleep in Hypervisor.framework's own
   `wait_for_interrupt` for good. Under HVF with Apple's in-kernel VGIC
   the `virt` board takes MSIs through GICv2m (not the ITS the PCI
   capability suggested), which pulses an SPI: `hv_gic_set_spi(intid,
   1)`, then `0` at once, and a pulse the VGIC does not sample is gone.
   The host's blank adds an MSI 60 times a second, enough to lose one that
   mattered within minutes. QEMU's own GIC (`-accel
   hvf,kernel-irqchip=off`) was no A/B: Windows 11 boot-looped into
   Recovery on it. Patch 91 latches each rising edge as pending
   (`GICD_ISPENDR`, write one to set) beside the line. With it, the same
   overlay, the host's blank on: the user, "man, now this is smooth,
   it's awesome", and DWM composes every 16.661 ms (300 `DwmFlush`es,
   median 16.661, 5th to 95th percentile 16.45 to 16.93, all between 15
   and 18 ms), tighter than the PC's step 4 B. The coarse-tick bunching
   of the timer alone stays, but with the host's blank the timer is only
   the watchdog.
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

- Windows 11 on Arm: the driver's timer bunching on a coarse tick under
  HVF (only the watchdog now, patch 91 above), and Basic Display on
  ramfb drawn at 800x600 into a 1280x800 ramfb (step 4, "On the Air").
- ramfb's screen left extended on Arm was a machine without the guest
  agent, not an Arm bug. The user, switching with Win+P between Extend
  and the virtio-gpu's screen alone, with patch 91 in: "the difference in
  smoothness is very obvious" (DWM keeps the simulated 64 Hz blank while
  ramfb's screen is on). The Air's `win11` machine had never run the
  drivers disc's `2ksbox\install.cmd` (no `C:\2KSBOX\agent.log`); with it
  run, left on Extend and restarted, the agent made the virtio-gpu's
  screen the only one a few seconds after logon, and the desktop was
  smooth (2026-10-09). The gap: nothing runs `install.cmd` on a machine
  whose owner did not, and that machine also has no clipboard and no
  shared folder (M23).
- `viogpudo-install.ps1` (2026-10-09, after the Air): it now reads the
  running kernel's code integrity options before touching the installed
  driver. Without `TESTSIGN` it only turns test mode on and stops, naming
  Smart App Control when that is on or evaluating; if `pnputil` still
  refuses ours, it puts upstream's back from the drivers disc
  (`$WinPEDriver$\viogpudo`). Run on the Air's overlay with test mode on:
  it reinstalled ours with no lasting black screen, and after a restart
  the desktop came back as it was. The ISO takes it and `dwm-pace.ps1` from
  the tree, and `windows-drivers.sh`'s hash for viogpudo no longer
  covers `guest-tools/viogpudo`: the key moved, so the PC publishes once
  more for this checkout.
- A 144 Hz host screen (the PC's): 60 guest frames on 144 refreshes
  judder 2-3-2-3 whatever the guest does; `VSyncHz=72` or 144 would
  divide it, and step 4 makes it follow the screen.
- x64's two screens: the launcher's x64 machine keeps the standard VGA
  for setup and recovery (the user: "the virtio guests always need two
  screens"), whose screen Windows makes the primary. The guest agent now
  makes the virtio-gpu's screen the only one once the desktop is up (doc
  24 §4, job 3): on the test machine it logged `display: the virtio-gpu's
  screen made the only one (0)` right after the drivers disc's install
  (upstream's driver), and `already the only one` after a restart.
- Upstream's `AddSingleTargetMode` copies `TotalSize` into `ActiveSize`
  before `BuildVideoSignalInfo` fills it, so target modes have a zero
  active size; patch 01 sets it in its own mode only.
