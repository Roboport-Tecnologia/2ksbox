# Track M4: the paravirtual Direct3D device (doc 14, ADR-006/007)

The first Direct3D 8/9 path: the decoder and executor over DXVK, the SysBus
`d3dpt` device and the guest `d3d9.dll` / `d3d8.dll`. The milestone closed
on 2026-09-04 (P0–P4, `docs/08-roadmap.md`). The display driver (M7 on XP,
M10 on Win98, doc 15, ADR-008) took the guest side over, and M16 step 7
(2026-09-27, ADR-021) retired the guest DLLs (`D3D8.DLL`, `D3D9.DLL`,
`DDRAW.DLL`) and the SysBus `-device d3dpt`. What stays is the executor,
its host harness and the reference workload. This doc keeps the track's
scope, test loop and open items; the design and numbers are in doc 14.

## Scope and files

- Protocol and host side:
  - `d3dpt/d3dpt_proto.h`: bump `D3DPT_PROTO_VERSION` on any wire change.
  - `d3dpt/d3dpt_enc.h`
  - `d3dpt/exec/` (`libd3dpt_exec`; the Wine host program is M15's, the
    DDI decoder `d3dpt_exec_ddi.cpp` M7's)
  - `d3dpt/hw/d3dpt_exec_load.c` (the loader; patch 40 adds the meson
    subdir)
- Reference workload: `guest-tools/src/d3d9test.c`, `d3dfeat9.c`,
  `d3dgame9.c` and `d3dgame8.c`, built into the ISO by
  `guest-tools/build-wrappers.sh`. They run on `d3dpt-vga` through
  Microsoft's own runtime.
- DXVK: `third_party/dxvk` + `patches/dxvk/`, `scripts/*dxvk*` and
  `scripts/build-d3dpt-exec.sh`.
- Host tests: `tools/d3dpt-dp2-test.cpp`, `tools/d3dgame9-native.cpp`,
  `tools/d3dfeat9-native.cpp`, `tools/bmpdiff.py` and the rig goldens in
  `reference/d3d/`.
- Shared with M7 and M15: `d3dpt_proto.h`, `d3dpt/exec/` and `d3dpt/hw/`.
  Rebase first, edit minimally, and name the track in the commit.

## Test loop

```sh
scripts/build.sh      # QEMU, DXVK, the executor and the guest-tools ISO
scripts/test.sh       # host checks: d3dpt-dp2, d3dgame9-nat, d3dfeat9-nat
scripts/test.sh all   # + the guest stage: XP on the display driver
```

- **Guest stage.** `tools/xp-dx9-test.sh` installs the driver on a fresh
  overlay of `~/vms/winxp.qcow2` and runs `DDVMTEST`, `D3DGAME9`,
  `D3DGAME8` and `D3DFEAT9` through XP's own runtime. D3DGAME9/8 must be
  pixel-identical to the native frame outside the HUD and within
  `D3D_GOLDEN_BUDGET` of the rig golden; D3DFEAT9 must be byte-identical to
  the native frame, with the same query and getter lines.
- **After a protocol bump,** rebuild the executor and the ISO, or the
  suite fails with `protocol mismatch` or a guest that never attaches.
- Tool detail is in `docs/testing.md`; the executor's env knobs in
  `docs/development.md`.

### A game on the device

`tools/xp-game-test.sh` runs a game headless on the display driver (the
image needs it installed). Discs go on the IDE slots (`CDS=`).

| Option | What it catches |
|---|---|
| `SHOTS=` | launchers, error boxes and the game's own frames |
| `DRW_AFTER=` | every thread's stack, through Dr. Watson |
| `PAGEHEAP=1` | heap overruns, faulting where they happen |

A "frozen" game and KVM `-cpu host` breaking Max Payne are in
`docs/00-status.md` "Gotchas".

## What stayed open

- The DLL path's stubs (P8 textures, volume textures, swap chains,
  `GetFrontBuffer`, `ProcessVertices`, the lost-device protocol) left with
  the DLLs in M16 step 7; games now meet Microsoft's runtime on the
  driver's DDI (M16, doc 15).
- **Performance, when a game asks for it:** zero-copy present through
  DXVK's Vulkan interop, Present pacing against the player's vsync, and a
  decoder thread off the vCPU (doc 14 defers it until a measurement asks).
  Measure first with `PLAYER_LATENCY=1`. M17 measured the driver path.
- **A real-workload x87/SSE number.** A D3D title with and without
  `-cpu pentium3,x87-fast=off,sse-fast=off`. Shared with M8.
