# Track M14: the Voodoo 2 device

86Box's 3dfx Voodoo 2 emulation as a QEMU PCI device, `-device voodoo2`,
the machine's only Glide since ADR-020 removed the Glide pass-through
(2026-09-23), beside qemu-3dfx's OpenGL pass-through. Doc
21 is the design and holds what was learned (why a chip, the shim,
threads and timers, the display path, the RAM command FIFO, the login
helper, the stranded client, the dither, the ring's ordering). This file
is the working state: scope, test loop, traps, open items. ADR-016 is the
decision, ADR-020 the one that made the chip the only Glide. The track
runs on `main`.

## Scope and files

- `voodoo/`: `86box/` (vendored **verbatim**, with `UPSTREAM`), `shim/`
  (86Box's platform headers reimplemented), `voodoo_shim.c`, `voodoo2.c`
  (the device), `undither.c`, `meson.build`.
- Patches on the overlaid `hw/voodoo/86box/` copy: 62 (meson subdir), 64
  (dither subtraction in both 86Box code generators), 71 (packet 3's
  packed colour word), 72 (the FIFO thread's order).
- `scripts/prepare-qemu.sh` (the overlay), `scripts/sync-86box-voodoo.sh`.
- `tools/voodoo-guest-test.py` and its four checks in `scripts/test.sh`.
- `guest-tools/src/v2start.c` (`SETUP /I 6`, the login-helper guard) and
  SETUP's "leave a 3dfx card's mapper alone" rule (doc 21 §10).
- The form's "Emulated 3dfx Voodoo 2" checkbox (`voodoo2` in the bundle,
  `-device voodoo2,addr=0x05`), shared with M6.

## State

- **3dfx's own Win98 driver and Glide 2 and 3 run on it.** By hand on
  `base98-br`: Quake II (MiniGL), Unreal Tournament (Glide 2), NFS Porsche
  Unleashed, FIFA 2000 (full matches), Carmageddon's 3dfx build (races,
  since `texmem=2`), and 3DMark 99 on the Windows PC. DxDiag's DirectDraw
  test passes full screen. A second game after one has quit sometimes
  starts glitched (open item 1).
- **Performance.** The RAM command FIFO (`ramfifo=on`, the default) took
  Quake II's `timedemo demo1` 41.1 → 147.5 fps and UT's CityIntro flyby
  33.6 → 40.7 (doc 21 §9). What still traps is mostly status polls while
  the guest waits for the chip, so a BQL-free MMIO region is not worth it
  now.
- **The Glide hang at `grSstWinOpen` is 3dfx's login helper** initialising
  the card from another process (doc 21 §11). `V2START.EXE` waits it out
  with a notice, and the device warns. A client left streaming into a FIFO
  switched off under it is refused (`fifo-off-regs=on` is the A/B).
- **The ring is exact on both transports** (doc 21 §13; `mmio-holes` is
  the switch).
- The 8 MB board (`texmem=2`) is the default. The 12 MB board's 4 MB TMUs
  let a 1997 allocator straddle a 2 MB boundary, which Glide refuses.
- `undither=on` removes the dither at scanout (doc 21 §12).

## Test loop

```sh
scripts/build.sh                      # or, after a voodoo/ edit:
scripts/prepare-qemu.sh && ninja -C build/qemu qemu-system-i386 libqemu-embed-i386.so
python3 tools/voodoo-guest-test.py    # ~15 s; build/voodoo-guest/
VGA=d3dpt python3 tools/voodoo-guest-test.py    # beside our adapter
RAMFIFO=off python3 tools/voodoo-guest-test.py  # the ring through MMIO
UNDITHER=on python3 tools/voodoo-guest-test.py
RECOMP=off python3 tools/voodoo-guest-test.py   # 86Box's interpreter
scripts/test.sh all                   # voodoo-guest{,-d3dpt,-mmiofifo,-undither}
```

The phases and their controls (`DITHER_SUB=off`, `FIFO_OFF_REGS=on`,
`MMIO_HOLES`, `LFB_ORDER`) are in `docs/testing.md`.

A game on the card, headless: `tools/win98-game-test.sh <image> <name>`
with `EXTRA='-device voodoo2,addr=0x05'`, the launcher's slot; any other
slot is new hardware to Windows and brings a restart prompt. The harness
waits for `WINDOWS\V2START.LOG` when the image has the guard
(`VOODOO_WAIT` the cap), a fixed wait otherwise. The benchmark is Quake
II's `timedemo demo1` with `gl_swapinterval 0` on a raw copy of
`base98-br` (doc 21 §9 has the numbers and the `(N new)` column's limits).

The device writes `voodoo2:` lines on QEMU's stderr: the configuration at
start, `display on/off`, and every 5 s the frames (`(N new)` column),
triangles, register and texture traffic, and three histograms (registers
read, written, config dwords read) that name what a spinning guest polls.
`VOODOO2_TRACE=1` logs every register and FIFO-window access, spins
collapsed, and names the guest module behind each `sst1InitRegisters`. The
first refused write dumps the FIFO state and the last 64 accesses.

## Traps

- **A hung QEMU cannot be attached to here** (ptrace scope). Run the same
  command line under gdb in the background and interrupt it:
  `gdb -q -batch -ex run -ex 'thread apply all bt 14' --args
  build/qemu/qemu-system-i386 …`, then
  `kill -INT $(ps -C qemu-system-i386 -o pid=)`.
- **A stray `rundll32` in the guest is 3dfx's login helper**, and a Glide
  program started under it wedges (doc 21 §11). On an image without the
  guard, wait out the login before any Glide run.
- **Get the Glide source** before reading register traces:
  `https://codeload.github.com/sezero/glide/tar.gz/refs/heads/master`,
  `glide2x/cvg` (`glide/src/gsst.c` is `grSstWinOpen`, `init/` the
  `sst1Init*` layer).
- **Long runs of `status` reads are not timeouts.** Glide's fixed
  200 000-iteration clock-settle loops (`init/video.c`) always run to the
  end.
- `FX_GLIDE_NO_SPLASH=1` skips 3dfx's splash, which turns a torn-down card
  from broken into hung. `FX_GLIDE_REQUIREOEMDLL` wants `GR_SKIP_OEMDLL`
  (0xee1feef) in **decimal**, 249691887; a wrong value silently loads
  `FXOEM2X.DLL`.
- A killed Win98 guest never flushes its FAT, so an empty `GLIDETEST.LOG`
  is no evidence; read COM1 or the screendump.
- **NFS Porsche on a Voodoo 2 is Direct3D unless told otherwise**, and its
  green tyre smoke is the game's at 16 bpp, not the chip's. Don't debug
  it again (doc 21 §10).
- Menus cannot be clicked headless while the card has the monitor:
  `relclick` steers by `d3dpt-vga`'s cursor registers, which do not move
  then.
- The 86Box command counters (`cmd_written` against `cmd_read`) balance
  only because Glide rings the swap doorbell *and* posts the packet;
  correcting either alone leaves BUSY stuck (doc 21 §13).
- One TCG guest at a time, and end scripted Win98 runs with the ACPI power
  button (`docs/00-status.md` "Gotchas").

## The Air (2026-10-04)

The ARM64 recompiler runs under `MAP_JIT` with no change: the shim maps
code with `MAP_JIT` and 86Box's generator toggles
`pthread_jit_write_protect_np` itself. lldb on `voodoo_generate` counts
four blocks compiled during a passing `voodoo-guest`, so the JIT draws
the dither scene, and patch 64's ARM64 half is checked on hardware:
`DITHER_SUB=off` fails with the documented walk (7bef … 6b4d, 10,240
pixels off the tile) and `RECOMP=off` passes. The four `voodoo-guest`
checks pass in `test.sh all`.

Games, headless on a raw copy of `base98-us` with 3dfx's 3.02.02 driver
installed into the copy (the user's package, `~/isos/others/voodoo2-30202.exe`):

| | Air | Linux (2026-09-15) |
|---|---|---|
| Quake II `timedemo demo1`, `3dfxgl`, 640×480, `gl_swapinterval 0` | **147.9 fps** | 147.5 |
| the same, `recompiler=off` | 71.6 fps | |
| UT CityIntro flyby, Glide, one loop of `(N new)` | **39.9 fps** | 40.7 |

The JIT is 2.07× the interpreter here. Neither launcher machine on the
Air had 3dfx's driver: `GLIDE2X.DLL` in `SYSTEM` alone is the old
pass-through's leftover, and a run on such an image meets "Add New
Hardware" and never opens Glide. Installing into a scratch copy: unzip
the package into the raw copy's `WINDOWS\INF` (8.3 names, upper case),
boot with the card, and answer the source prompt with
`KEYS="40:ret,45:c,45:shift+semicolon,45:backslash,…,50:ret,90:ret"`
(`C:\WINDOWS\INF`, then the INF's `Reboot`). UT needs
`GameRenderDevice=GlideDrv.GlideRenderDevice` and `FirstRun=436` in its
INI, or it opens its first-run wizard.

## Open, in order

1. **Left-over state between Glide windows.** A second game after one has
   quit sometimes starts glitched (the user, by hand; not re-checked since
   the ring and teardown fixes). Trace with `VOODOO2_TRACE=1` what the
   last window left in `videoDimensions`, `fbiInit*` and the ring.
2. **Why a client resumes on a dead ring** (FIFA 2000's close, doc 21
   §11): it streamed on the old session's ring after someone else's
   `sst1InitRegisters`, with none of a healthy reopen's writes. The device
   now names both modules and their `cr3`; the next such log says whether
   it is one program or two.
3. **DxDiag's Direct3D 7 test fails at step 46, `GetDC`**
   (`DDERR_CANTCREATEDC`). Unknown whether a real Voodoo 2 with this
   driver fails it too (the card has no GDI) or the device lacks
   something.
4. ~~**The M1 Air.**~~ Done 2026-10-04 ("The Air" below). Left there:
   Diablo II and the hand test through the player.
5. **Diablo II**, the title the route was chosen for, measured, and
   Tirtanium (one of the titles that stranded the screen) run.
6. **Upstream.** Offer patches 64 and 71 and the two command-counter
   asymmetries to 86Box; a sync that brings them in drops the patch.
7. A Voodoo Graphics (`VOODOO_1`) type only if a title wants one; SLI
   never.

## Rules

- The vendored files stay verbatim. A change 86Box needs goes upstream,
  into the shim, or, when it must be in 86Box's code, into a QEMU queue
  patch on the overlaid copy, dropped once upstream has it.
  `scripts/sync-86box-voodoo.sh` must keep working.
- Every claim about a game or a driver comes from a run with its log and
  screendump in `build/`.
- The OpenGL pass-through (qemu-3dfx's `hw/mesa`) is not retired by this
  device and is not this track's to touch.
