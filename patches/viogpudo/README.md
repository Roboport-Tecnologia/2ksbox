# Our viogpudo's patches (track M24)

virtio-win's display-only driver for virtio-gpu (`viogpu/viogpudo/` in
[kvm-guest-drivers-windows](https://github.com/virtio-win/kvm-guest-drivers-windows),
BSD-3-Clause), at upstream commit `fbcc19d763f92a8281abe364c7ea1de3de87e61d`
(2026-07-22, the last before virtio-win 0.1.302's viogpudo, DriverVer
100.103.104.30200, which our drivers disc carries signed).
`scripts/build-viogpudo.sh source` checks the pin out clean into
`build/viogpudo/src` and applies these with `git apply` in filename
order; the patches are `git diff` output from the upstream tree's root
(`a/` and `b/` prefixes). A new or regenerated patch is made in that tree
and checked by `source` from clean before it is pushed.

The result is test signed in the guest
(`guest-tools/viogpudo/viogpudo-install.ps1`); see the track doc,
`docs/tracks/m24-viogpudo.md`, for why and what would ship it.

| Patch | What it does | Drop when |
|---|---|---|
| `01-vsync.patch` | A vertical blank of the driver's own: `DxgkDdiControlInterrupt` and `DxgkDdiGetScanLine`, every mode at `VSyncHz` (60) with a 1/20 blank instead of an unspecified rate, `DXGK_INTERRUPT_DISPLAYONLY_VSYNC` from a high-resolution timer. `VSyncHz` = 0 under the `VioGpuDod` service's `Parameters` key is upstream's behaviour. | upstream reports a vertical blank (then the A/B against its signed build decides) |
| `02-edid-rate.patch` | The vertical blank at the rate of the device's EDID (its preferred timing), which 2ksbox's player sets to divide the host screen's refresh (embed API v15; 72 Hz on a 144 Hz screen), read at start and again on each display event; 60 Hz with no EDID. `VSyncHz` in the registry still overrides it. | upstream reports a vertical blank at the EDID's rate |
| `03-viogpuap-adapters.patch` | The resolution helper (`viogpuap`) walks every display device for virtio-gpu's instead of stopping at the first that is another card's (the standard VGA or ramfb, enumerated first on 2ksbox's machines), so the desktop follows the window. | upstream fixes `FindDisplayDevice` |
| `04-host-vblank.patch` | The host screen's vertical blank as ours: with QEMU patch 90's `host-vblank` feature the driver asks for the host's blanks while dxgkrnl wants the interrupt and raises the blank from the config interrupt that carries one; the timer of 01 fills in only when they stop. | upstream gives virtio-gpu a vertical blank |
