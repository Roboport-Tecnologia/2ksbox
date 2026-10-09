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
