# Track M17: the display driver's speed, measured on Max Payne 2

Opened 2026-09-27 (user: "Use max Payne 2 to profile and optimise our
driver"). The driver here is everything between a game's Direct3D calls
and DXVK: the 9x HAL (`d3dpt9hl.dll`, `guest-tools/src/d3dptvid/core/`),
the device (`d3dpt/hw/d3dpt_vga.c`) and the executor
(`d3dpt/exec/d3dpt_exec_ddi.cpp`). Microsoft's runtime, the game and TCG
are not, except where the driver makes them work harder.

Read `docs/00-status.md` first for the track rules. Doc 15 has the
driver, doc 14 the executor, doc 22 the TCG work this sits on.

## The benchmark

`tools/w98-mp2.sh <name>`: Max Payne 2 v1.01 in `base98-br` (Win98, TCG,
256 MB, the Voodoo 2 on, as the launcher runs it), the game's launcher
clicked, Resume Game on its menu, the user's save: Max standing in the
hospital corridor. The user's settings are every one at its maximum:
800x600x32, 4x MSAA. A frame is about 207 draws in 6 DrawPrimitives2
calls, 12 vertex / index buffer writes and one readback. The scene is
steady from about 250 s after the desktop. The run is uncapped
(`ddflags=32768`, no vertical blank on the flip): with the blank the
game stops at 60 frames/s, which hides a gain.

**Read an A/B inside one run.** The same build gave 61.3 and 58.0
frames/s in two launches; TCG's own speed moves about 5 % between
launches (doc 22 §5.0). The executor's `D3DPT_DDI_FLUSH_AB=n` alternates
two settings, one 5 s rate line each, and `tools/ddi-rate.py` compares
adjacent periods. A change with no switch gets its number from the
profile: the samples it removed.

## State

**Closed 2026-09-27 (user: "end here, it's good enough already")**
after the two fixes below. The next steps are left as written, for
whoever takes the driver's speed up again.

**Where the time goes** (2026-09-27, `perf` + QEMU's `-perfmap`,
`tools/guest-code-owner.py`). The vCPU thread is 95.8 % busy and the
process averages one core: the guest CPU is the limit, not the GPU (DXVK's
threads together are about 2 %). Of the vCPU thread:

| | share |
|---|---|
| guest code (the JIT) | 74 % |
| — the game's engine DLLs (`e2_d3d8_driver_mfc` 17 %, `e2mfc` 11 %, `X_GameObjectsMFC` 9 %, physics 4 %, ...) | ~52 % |
| — `msvcr71.dll` (the game's copies) | 6 % |
| — Microsoft's `D3D8.DLL` | 5 % |
| — **our HAL** `d3dpt9hl.dll` (`walk` 2.1 %, `walk_draw` 0.5 %) | 3.8 % |
| QEMU (TB lookup, softmmu) | 14.5 % |
| **the executor**, synchronous inside the guest's doorbell write | 8.5 % (after the fixes) |

The executor's wall time, from the device's own line (`batches in 5.0 s,
N ms of them in the executor`), is larger than its samples because it
includes waiting for the GPU.

**Done:**

1. **A texel mean computed on every texture bind for a trace line**
   (`Dp2::stage_state`, TEXTUREMAP): it read every 8th texel of the level
   in VRAM, and a quarter of a DXT texture's bytes, whether or not a trace
   was on. 7.3 % of the vCPU thread. Now inside `if (d.trace)`. The
   executor's wall share went from 19 % to 15 %; the capped run with
   `-perfmap` went from 52.1-53.2 to 55.5-59.4 frames/s.
2. **The flush hint** (doc 15, "The GPU starts on a frame before its
   readback"). The readback waited 1 ms a frame for the GPU to draw what
   DXVK had held back. An EVENT query's `GetData(FLUSH)` every 16 draws:
   the wait went from 283 to 116 ms in each 5 s, the executor's share
   from 15 % to 11 %, and the frame rate **+2.76 frames/s (+4.7 %)** over
   52 adjacent periods (standard error 0.24). 4 draws against 16: +0.15
   (0.20), no difference.
3. The rate line reports the readbacks' time and their wait; the
   harness's `ddi: frames` count had matched nothing since the log lines
   gained a prefix, and `DUMP_EVERY` went with the Direct3D DLLs.

## Next steps

What is left of ours, in the vCPU thread's samples, largest first. Each
is small, so each needs its own A/B switch.

1. **Draws** (3.2 %): `draw8` re-biases every index into a vector and
   calls `DrawIndexedPrimitiveUP`, which copies the draw's vertex range
   into DXVK's upload buffer every time. Host vertex / index buffers
   mirroring the guest's VRAM buffers, updated from the VRAM_DIRTY_RANGE
   records the driver already sends (37 MB in each 5 s here, against the
   whole vertex range every draw), would drop the copy and the loop. The
   risk is a guest that writes a buffer outside a lock (doc 15's
   dangling `lpDDVertex`), which the copy at draw time tolerates.
2. **The HAL's two walks** (half of 2.9 %): `dp2_record` walks the
   command stream once to size the record and again to write it. One walk
   into the space left in the command window, falling back to two when
   it overflows, saves the first. The pass-1 duties (the render-state
   mirror, TEXBLT, the colour-key check) need their own flag rather than
   `!w->out`.
3. **The readback's wait** (2.3 %): what is left is the frame's last
   chunk and the fence. A readback completed at the next doorbell rather
   than at once would hide it, but the guest may show or read the VRAM in
   between (the flip right after), so it needs the flip to complete it.
4. **The readback's copies** (2.1 %): a compare against the shadow and
   two copies of 1.9 MB a frame, at memory speed. Skipping the compare
   needs to know which pixels the guest wrote, and QEMU's display already
   consumes the VGA dirty bitmap.
5. Out of reach of the driver: the executor off the vCPU thread (it reads
   textures and buffers from VRAM at draw time, which the guest may
   rewrite as soon as the doorbell returns), and `D3D8.DLL`'s 5 %, whose
   hot code has not been read yet.
