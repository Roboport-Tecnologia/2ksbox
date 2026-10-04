# 21. The Voodoo 2 device (M14)

A real 3dfx Voodoo 2 on the machine's PCI bus: 86Box's emulation of the
chip (Sarah Walker's PCem rasterizer with 86Box's x86-64 and ARM64
recompilers), vendored verbatim and wrapped as a QEMU PCI device,
`-device voodoo2`. The guest runs 3dfx's own drivers and the game's own
`glide2x.dll` / `glide3x.dll` / `GLIDE2X.OVL` against the registers they
were written for; nothing in the guest is ours. This doc covers the
device's mechanisms and the trap behind each, for anyone changing
`voodoo/` or debugging a Glide title on the card.

The decisions are ADR-016 and ADR-020 (doc 10). Track state, test loop
and open items are in `docs/tracks/m14-voodoo2.md`, the test tools in
`docs/testing.md`.

## 1. Why a chip, and why it is the only Glide

A Glide game once had a choice: qemu-3dfx's `hw/3dfx` carried the
guest's Glide calls to the host, where our OpenGLide build turned them
into OpenGL (doc 12 §5). The host GPU drew, so it was fast, but only as
complete as the wrapper: Glide 3 was missing (62 entry points), each of
the era's LFB tricks needed a patch, and a title with Glide linked
statically could not be reached. Nobody ever played a title on it by
hand, and the user found the card a much better experience, so ADR-020
removed the pass-through on 2026-09-23. **The Voodoo 2 is now the only
Glide on a 2ksbox machine.**

The chip is **complete by construction**: Glide 2 and 3, the DOS
overlay, static links, every LFB and texture-format corner. It costs a
software rasterizer at a Voodoo 2's own limits (800×600, 16-bit, 256×256
textures) and a vCPU trap per guest access to the card outside RAM (§9).

**It does not replace qemu-3dfx's OpenGL half.** `hw/mesa` and the guest
`opengl32.dll` serve GLQuake, Half-Life in GL
mode and wglgears, which a Voodoo 2 covers only through 3dfx's MiniGL/ICD
at rasterizer speed. The Direct3D device (docs 14/15) is a third path.

## 2. What is vendored, and from where

`voodoo/86box/` holds 86Box's `src/video/vid_voodoo*.c` and
`src/include/86box/vid_voodoo*.h` at the commit in `voodoo/86box/UPSTREAM`,
**unmodified**; `scripts/sync-86box-voodoo.sh <commit>` drops in a newer
upstream. Changes 86Box's code needs are QEMU queue patches on the
overlaid copy in `qemu/hw/voodoo/86box/` (64, 71, 72; §7, §9, §13), each
dropped once upstream has it. Not vendored: the Banshee / Voodoo 3 files
(the four entry points the Voodoo 1/2 code still calls are stubbed,
unreachable for `type < VOODOO_BANSHEE`) and the 32-bit x86 recompiler.
Licence GPL-2.0-or-later.

The emulation is one `voodoo_set_t` of one card, fixed at `VOODOO_2`. A
Voodoo Graphics is a property away but has no command FIFO (§9), so it
is not offered. SLI is not modelled.

## 3. Layout

```
voodoo/
  86box/            the vendored sources + UPSTREAM
  shim/cpu.h        what "cpu.h" resolves to for them
  shim/86box/*.h    86Box's platform headers, reimplemented (§4)
  voodoo_shim.h     the device <-> shim seam (hooks, config)
  voodoo_shim.c     the shim's implementation over QEMU
  voodoo2.c         the PCI device
  undither.c/.h     the scanout undither (§12)
  meson.build       -> hw/voodoo/meson.build
patches/qemu/62-voodoo2-device.patch   subdir('hw/voodoo') in meson.build
scripts/sync-86box-voodoo.sh
```

`scripts/prepare-qemu.sh` rsyncs `voodoo/` to `qemu/hw/voodoo/`. Meson
builds the 86Box files as their own static library with `-msse2` on
x86-64 and 86Box's warning set. The include path is `hw/voodoo` for
`<86box/vid_voodoo_*.h>` and `hw/voodoo/shim` for the rest, so their
`"cpu.h"` is the shim's, never `target/i386/cpu.h`. The library is
`link_whole`d into the i386 sourceset, so `qemu-system-i386` and
`libqemu-embed` both carry it.

## 4. The shim: 86Box's platform over QEMU

| 86Box | Here |
|---|---|
| `thread_create/wait` | `qemu_thread_create` (joinable) / `qemu_thread_join` |
| events (`thread_create_event`, `set`, `reset`, `wait_event(ms)`) | manual-reset over `QemuMutex` + `QemuCond`; `wait_event` returns 1 on timeout, 0 when set; `< 0` waits for ever, as upstream |
| mutexes | `QemuMutex` |
| `timer_add`, `timer_advance_u64`, `timer_set_delay_u64`, `timer_is_enabled`, `timer_get_ts_int`, `TIMER_USEC`, `tsc` | `QEMUTimer` on `QEMU_CLOCK_VIRTUAL`; units in §6 |
| `mem_mapping_add/set_addr/disable` | records the handlers; the device's `MemoryRegion` calls them (§7) |
| `pci_add_card`, `pci_burst_time`, `pci_nonburst_time` | recorded / constants; the device forwards config bytes (§7) |
| `plat_mmap(size, executable)` | `mmap` (+ `MAP_JIT` on macOS), `VirtualAlloc` on Windows |
| `plat_timer_read`, `plat_delay_ms` | `get_clock()`, `g_usleep` |
| `device_get_config_int(name)` etc. | a table the device fills from its properties before `voodoo_init` |
| `cycles -= …` (charging the CPU for a PCI access) | a dummy int; TCG charges nothing beyond the trap |
| `svga_get_pri`, `svga_set_override`, `svga_doblit`, `monitors[].target_buffer` | the display path, §7 |
| `calloc` / `free` (macros in the shim's `86box.h`) | allocations of 1 MB or more get **64 MB of zero pages after them**: the display timer indexes `fb_mem` by `front_offset + line * row_width` unbounded, from guest-set registers (Glide put 6.8 MB into 4 MB on a reopen), so an overrun reads zeros instead of faulting |
| `fatal()` | **returns**, unlike upstream: the write is refused and counted, the first dumped (§8) |

The shim's `86box.h` keeps upstream's atomics verbatim (`volatile` on
x86, C11 atomics elsewhere), because the FIFO/render thread protocol is
written in them.

## 5. Threads and the one invariant

86Box runs the guest CPU, the register handlers and the timers on **one
thread**, beside the Voodoo's FIFO thread and 1–4 render threads, and
the code assumes it: `voodoo_callback` (the display timer) and
`voodoo_writel` share `line`, `front_offset`, `dirty_line` and
`swap_pending` with only `swap_mutex` between them.

Here the handlers run on the vCPU thread and the timers on the main
loop, **both under the BQL**, so they exclude each other the same way.
The FIFO and render threads touch nothing of QEMU's. The region is
therefore **not** `memory_region_clear_global_locking`'d, which would
need a lock of our own between handlers and timer; with the ring in RAM
(§9) what still traps is mostly status polls, so it is not worth it.

Upstream already handles the one deadlock. The vCPU can block in a
Voodoo wait with the BQL held (`voodoo_flush` on a register read,
`voodoo_queue_command` on a full FIFO, LFB reads) while the main loop
cannot fire the retrace that completes a swap the FIFO thread waits on.
`voodoo_wait_for_swap_complete` breaks the wait when `flush` is set or
the FIFO is full, the two cases in which the vCPU blocks. A status poll
(how Glide waits for a swap) is not a block; the vCPU drops the BQL
between reads.

## 6. Timers and their units

86Box timers hold a 32.32 timestamp in TSC ticks. Here a *delay* is
nanoseconds of `QEMU_CLOCK_VIRTUAL` in that 32.32 form, so the timers
follow `-icount` and stop with the VM. `TIMER_USEC = 1000 << 32` and
`cpuclock = 1e9`, so `clock_const` is ns per pixel and `line_time` a
delay in these units; `tsc` is the virtual clock in ns, which keeps
`hvRetrace` consistent. The *expiry* a timer holds is `ns << 16`, 48
bits of nanoseconds (78 hours) with a fraction fine enough that a
non-integer line time does not drift. **It is not 32.32**: that wrapped
4.3 s after boot, re-armed in the past, `timerlist_run_timers` never left
it, and QEMU stopped answering SIGTERM.

The **display timer fires per scanline** (~32 µs at 640×480), 31 000
BQL-taking wakeups a second. The shim coalesces them: a timer whose
callback re-arms it less than `slack_ns` ahead is called again in the
same wakeup (`shim_timer_fire`). With 1 ms of slack the main loop wakes
~1000 times a second and runs ~32 lines each time. `voodoo_callback` is
a state machine over `line` with no dependence on wall time, so the only
visible effect is that `SST_vRetrace` advances in steps and `hvRetrace`
is coarse. The FIFO thread's 100 µs wake timer gets no slack.

## 7. The device: PCI, MMIO, display

**Identity.** Vendor `121a`, device `0002`, revision 2, class 0400, one
16 MiB memory BAR. Config bytes 0x40–0x43 are `initEnable`, answered by
86Box's handler (byte 1 carries the `0x50 |` strap 3dfx's driver reads
to know it has a Voodoo 2). The device echoes the command register and
the BAR's top byte to it, so `pci_enable` / `memBaseAddr` track the PCI
core.

**0x54 is `siProcess`**, the silicon-process monitor. Glide's
`sst1InitMeasureSiProcess` loads a countdown into bits 27:16, sets RUN
(bit 28), polls until the field reads zero, then reads the
ring-oscillator count from bits 15:0. QEMU keeps whatever a guest writes
past the 64-byte header, so without a model the count read back for ever
and **every Glide program froze in `grSstWinOpen`** (86Box answers 0 to
the whole register). Here the countdown ends as soon as RUN is read
back, and the count is a production board's 8000 (Glide wants over
5000).

**MMIO.** The BAR is a `MemoryRegion` calling the handlers
`mem_mapping_add` recorded (`voodoo_readw/readl/writew/writel`, masked to
24 bits: registers, LFB, texture space, command-FIFO window). Byte
accesses get `0xff` / nothing, as on the hardware, since 86Box registers
no byte handler. The memory core splits wider accesses (an SSE store into
the LFB) into dwords. The ring's window is guest RAM by default (§9).

**`fbiInit1` bit 23, scanline interleaving, is masked.** There is no
partner card, and 86Box would draw the odd lines from `set->voodoos[1]`,
which is NULL. A stray write walking the register file set it (§11).

**Packet 3's packed colour** (patch 71). Bit 28 says the per-vertex
colour is one packed ARGB word; 86Box read it only under the RGB bit.
Glide sends **iterated alpha over a constant colour** with the packed bit
set and the RGB bit clear (`gglide.c`), a word per vertex that both
consumers skipped, desynchronising the ring. Our packet walk (§9) counts
by the same rule. The device says once when it meets such a packet.

**A consumer stuck inside a packet** is the shape every ring desync
takes. `cmdfifo_get` waits for the next word whenever `depth_rd ==
depth_wr`, so a header promising more words than Glide wrote parks the
FIFO thread with `voodoo_busy` set, and Glide, polling for idle, never
frees it. A guest reset does not reset the card, so the next login's
helper (§11) hangs on it too. After two 5 s windows of it the device
prints `voodoo2: the command FIFO is stuck inside a packet` with the
awaited word, the ring around it and the guest's last 64 accesses. It
also warns on a packet word written narrower than a dword (86Box's
`writew` takes only the frame buffer), and logs Glide's device probe
(every LFB readback with the FIFO off).

**Display.** With `fbiInit0`'s VGA_PASS bit a Voodoo 1/2 drives the
monitor instead of the 2D adapter, which 86Box models as overriding the
primary SVGA. The shim turns `svga_set_override` into
`graphic_hw_passthrough(console 0)` (patch 30's hook, which skips the
VGA device's `gfx_update` while set), and `svga_doblit`, the end-of-frame
call, into a copy of the monitor bitmap into our `DisplaySurface` plus
`dpy_gfx_update_full`. The frame reaches the player through the ordinary
VGA surface path, so `screendump` and VNC see it; there is no 3D-frame
path and no player change. The monitor bitmap is 4096×4224, lazily
mapped, because `h_disp`/`v_disp` are 12-bit fields the display code
indexes unchecked.

When VGA_PASS clears, the console is invalidated, so **an invalidate
must put the adapter's own surface back on the console**, not only
repaint. QEMU's VGA core does; `d3dpt-vga` does since it left Win98
showing the Voodoo's last frame after every full-screen switch.
`voodoo-guest-d3dpt` checks it and patch 66 (the adapter's hardware
cursor hidden while the Voodoo has the monitor).

**Black until the first swap.** At a takeover or a change of
`h_disp`×`v_disp` the frame buffer holds 3dfx's init test patterns or
the last mode at a new pitch. A real monitor was dark while it
re-locked, so the device shows black until the guest swaps
(`front_offset` moves, or `frame_count`) and one frame more (a mid-frame
swap leaves the lines above it stale); a guest that never swaps is shown
after 2 s (`VOODOO2_BLANK_MS`). While black, every line is marked dirty
at each present, since the display timer presents only frames with dirty
lines.

**Reset.** 86Box has no reset entry, but a reboot must give the monitor
back: the device clears `fbiInit0`, `fbiInit7`, `initEnable` and the
mapping state and drops the override.

**Log.** Every 5 s of activity:

    voodoo2: 640x480 on: 61 frames, 12034 triangles, 480211 writes (23011
      texture), 3122 reads in 5.0 s; regs read 0x000:2981 0x218:19;
      written 0x120:279 0x114:263; config read 0x040:12

`frames` counts presents (swaps with dirty lines); §9 explains `(N new)`.
The histograms are the four most-read registers (by `addr &
0x3fc`), the four most-written and the four most-read config dwords, so
**a spinning guest names what it spins on** (`0x000` is `status`;
`0x054` in the config column was the siProcess loop). Further clauses
appear when non-zero: refused writes (§8, §11), the RAM ring's words
(§9), `busy:` with outstanding commands and FIFO depths, ordering waits,
ring holes and LFB writes by buffer (§13). `display on (VGA
pass-through)` / `off (VGA back)` mark the switch, and `640x480 shown
after 180 ms of black (first swap)` a new mode's first frame.
`VOODOO2_TRACE=1` prints every register and FIFO-window access, status
polls collapsed to a count, for reading an open sequence against 3dfx's
`sst1init` source.

## 8. Not modelled, and the hostile-guest question

- **SLI** and the **Voodoo Graphics** type are not offered.
- **Interrupts.** 86Box `fatal()`s on a write to `intrCtrl` /
  `userIntrCMD` and on a malformed packet (`CMDFIFO packet 5 bad space`
  and a few more). **Here `fatal()` returns**; every call site `break`s
  or falls through after it, so the write is refused and the stream goes
  on. The first is printed with the device's state (`initEnable`,
  `fbiInit0/7`, the FIFO's base, end, read pointer and depth) and the
  last 64 accesses; the rest are counted (`N writes refused`). **It is
  not `noreturn` in the shim's header**: declared so, the compiler
  dropped the code after the call and the return landed in the next
  case, a SIGSEGV that looked like 86Box's.
- **Migration.** No vmstate, so a snapshot does not carry the card; the
  driver re-creates its state.
- **Byte accesses** to the LFB: none on the hardware, none here.

## 9. Performance: the command FIFO in RAM

86Box JIT-compiles the per-pixel pipeline for the current register state
(x86-64 SSE2, or ARM64 NEON, which PCem lacks and is why we took 86Box's
code) and runs it on 1, 2 or 4 render threads (`threads=`, 2) owning
alternate scanlines, on cores the one-vCPU guest leaves idle. A Voodoo 2
filled ~90 Mpixel/s; a 640×480 game at 30 fps needs 20–30, within a
modern core.

The cost that is ours is on the vCPU. Every access to the card is an MMIO
trap, and under TCG an MMIO store that is not a block's last instruction
is a `cpu_io_recompile`. In Quake II's demo that was a third of the
saturated vCPU, with the MMIO path proper another sixth. Glide feeds the
Voodoo 2's **command FIFO** (`fbiInit7` bit 8), packets written into a
window of card memory, so that window is where the traps were.

**`ramfifo=on` (the default) makes the ring guest RAM**, an alias of a
page-aligned `fb_mem` (86Box's calloc gets a replacement at realize), so
Glide's packets are plain stores. Glide rings no doorbell, so at every
other guest access to the card the device walks the ring from the last
word it counted and adds what has been written to the depth. Consumed
words are **poisoned with `0xdeadbee7`**. The guest learns a slot is free
only by reading `cmdFifoRdPtr`, which the device answers after poisoning
up to exactly what it returns, so the guest never writes over a word
that is not poison. A packet the walk cannot follow (JSR/RET, AGP,
Banshee types) sends the window back to MMIO with a warning.
`ramfifo=off` is the per-dword MMIO path, the A/B.

**Quake II `timedemo demo1`: 41.1 → 147.5 fps**, with the chip now the
busy side (~14 M words a second). UT's CityIntro flyby: 33.6 → 40.7.

The walk keeps three rules, each learned from a hang:

- **The poison is a word no guest writes.** It was `0xffffffff`, a white
  texel, and a texture download read as unwritten ring (3DMark 99 hung).
  `0xdeadbee7` is packet type 7, which does not exist, and absurd as a
  float or a texel pair. The look-ahead past poison applies to a
  packet's **payload** only; an unwritten header is always a gap, since
  the header is written first.
- **It counts words, not packets.** The chip runs words as they arrive,
  and 86Box's consumer blocks inside a half-written packet by itself.
  Glide writes each packet whole anyway (`packets met part-written`
  stays 0).
- **The read pointer never passes what the guest has written.** Glide's
  free space is `rp - wp - 1`; a read pointer past the write pointer is
  a state the chip cannot reach, and the guest waits for space for ever.
  FIFA 2000's loading screen died that way (47 unwritten words of a
  66-word LFB packet taken as data after 64 idle polls). Nothing is taken
  that the guest has not written, however long the wait.

The `voodoo-guest` check drives the ring as Glide does (one batch across
a JMP, one after a read-pointer read); under `ramfifo=on` only the walk
can have drawn the frames. Its **partial-packet phase** checks the third
rule: with a header written and its value not, 256 reads of
`cmdFifoRdPtr` must stay at `00300004`, then reach `00300008` when the
value arrives. `voodoo-guest-mmiofifo` is the same with `ramfifo=off`.

**Measuring a game's frame rate.** The 5 s line's `(N new)` column counts
presents that showed a buffer the one before did not: the game's frames
as the monitor shows them, capped at the refresh. `frames` counts every
present with a dirty line (an FMV through the LFB reads 225 with 0 new);
86Box's `frame_count` counts only swaps on a retrace (0 with vsync off).
Two swaps in one refresh show as one, so the column is a floor (~35
against Quake II's `timedemo` 41.3). The benchmark recipe is in the
track doc.

### Dither subtraction in both recompilers (patch 64)

A Voodoo dithers what it writes, so a pixel read back for blending
carries its position's dither offset; the chip subtracts it when
`fbzMode`'s `DITHER_SUB` (bit 19) is set and 86Box's `dithersub` setting
allows (`dither-sub=on|off` here, on). 86Box's interpreter applied it,
but neither code generator did: a grey blended onto itself at falling
alpha walked away from the colour (7c0f → 6b4d). Patch 64 gives both
generators the interpreter's three table lookups at the destination
read-back (`dithersub_rb` / `dithersub_g` or the 2x2 pair, keyed on the
dither write's x and `real_y`); the block cache already keys on the whole
`fbzMode`. Both emissions were checked against the tables for every byte
value at every dither position (ARM64 under Unicorn), and the ARM64 one
runs on the Air (track doc, "The Air"). The `voodoo-guest`
dither phase requires a screendump of one 4x4 tile with the recompiler
on and off (`RECOMP=off`); `DITHER_SUB=off` is the control that fails.

### Knobs

| Property | Default | |
|---|---|---|
| `threads=1\|2\|4` | 2 | render threads |
| `recompiler` | on | off: 86Box's interpreter, the A/B for a rasterizer bug |
| `bilinear` | on | |
| `dither-sub` | on | above |
| `filter` | off | 86Box's scanline filter, a no-op until the guest programs `maxRgbDelta` (§12) |
| `undither` | off | the dither reconstructed away at scanout (§12) |
| `fbmem=2\|4` | 4 | MB of frame buffer |
| `texmem=2\|4` | **2** | MB per TMU: the 8 MB board. `texmem=4` is the 12 MB board, whose 4 MB TMUs break a 1997 game (§13) |
| `ramfifo` | on | the ring in guest RAM |
| `lfb-order` | off | a wait for the ring before an LFB write (§13) |
| `fifo-off-regs` | off | on: a FIFO-window write with the FIFO off walks the register file, as 86Box does (§11) |
| `mmio-holes` | on | off: 86Box's per-write count on the MMIO ring (§13) |

On the Windows PC `scripts/win-voodoo-ab.sh <prop>=<value>` runs the
A/B.

## 10. The guest side

Nothing is ours. The card wants **3dfx's Voodoo2 reference drivers**
(Win9x: the `3dfxV2` package, drivers 3.02/3.03, with `glide2x.dll` and
`glide3x.dll`; XP: the last 3dfx reference driver or the community's),
the user's own downloads, never in the repository or the guest-tools
ISO. A DOS Glide game carries its own `GLIDE2X.OVL`, 3dfx's, which
talks to the chip. 3dfx's driver finds the card by scanning PCI for
`121a:0002` and reading `initEnable`; no pass-through cable is needed.

**Our guest tools stay out of its way.** 3dfx's driver installs
`GLIDE2X.DLL` / `GLIDE3X.DLL` / `FXMEMMAP.VXD` in the system folder; the
last is the device mapper our Direct3D and OpenGL DLLs also need
(qemu-3dfx's copy is 3dfx's own binary, 4.10.01.0013, so either serves
both). SETUP looks for a *present* 3dfx PCI device (9x: the devnode
tree in `HKEY_DYN_DATA`; NT: `CM_Locate_DevNode`) and, with one, leaves
an existing `FXMEMMAP.VXD` alone rather than downgrade it; the disc
carries no Glide of its own (ADR-020). `VOODOO=1
tools/setup-guest-test.sh` checks it on 98 and XP.

**The PCI map.** On both 2D adapters SeaBIOS puts the BAR at
`0xfd000000`, clear of the fixed pass-through windows (Mesa
`0xea000000`–`0xefffefff`, d3dpt `0xd8000000`–`0xdfffefff`; the Glide
device's, `0xfb000000`–`0xfb7fffff` and `0xfbdff000`, is no longer on
the machine). Nothing
reserves those windows, so a guest that moved the BAR could overlap
them; an ACPI Win98 keeps the firmware's placement.

**In the launcher** the card is one checkbox on the machine form
("Emulated 3dfx Voodoo 2", `voodoo2` in the bundle, doc 07), off unless
picked, on every family. It adds `-device voodoo2,addr=0x05`, the slot
after the sound card's. A headless run must use the same slot; any other
is new hardware and a restart prompt before the shell. For Win98 the
pairing is our adapter plus the chip: Direct3D on the doc 19 driver,
Glide on the chip. The `voodoo2` check walks the checkbox to
`query-pci`, and `tools/voodoo-guest-test.py` exercises the device with
no 3dfx code at all (`docs/testing.md`).

### What real titles do on it

Run by hand on `base98-br` with 3dfx's Win98 driver:

- **Unreal Tournament** is Glide 2 (`GlideDrv.dll` imports
  `glide2x.dll`; its log says `Found Glide: 2.56`). **Quake II** goes
  through 3dfx's MiniGL, Glide 2 too.
- **NFS Porsche Unleashed is Direct3D on a Voodoo 2 unless told
  otherwise.** Its `3DSetup\3dsetup.ini` sends a Voodoo 2 to `/M:dx`
  (`dx7z.dll`); its Glide 3 renderer `voodoo2z.dll` is for a Voodoo 1 or
  Rush. 3D Setup (run once) writes `HKLM\SOFTWARE\Electronic Arts\Need
  For Speed - Porsche 2000` `Thrash Driver` = `dx`. Setting it to
  `voodoo2` (a `.reg` through `regedit /s`) is the whole switch: the menu
  at ~50 fps against Direct3D's 30.
- **Porsche's green tyre smoke is the game's, not the chip's**: green
  under Glide on the chip (and it was on the retired pass-through), unchanged by
  `recompiler=off`, not green in Direct3D at 32-bit. Period reports give
  32-bit colour as the cure, which a Voodoo 2 lacks. Don't debug it
  again.
- **Carmageddon's DOS 3dfx build** puts a 640x400 front end in a 640x480
  Glide buffer; the menu's black bands are in the guest's own frame
  buffer, not a display-path stretch. Its races measure 4:3.
- **DxDiag (DirectX 9.0c)** shows the card as display 2, "Voodoo2
  DirectX 7 Driver", `3dfxV2.drv` 4.11.0001.1151, DDI 7, 12 MB. Its
  DirectDraw test passes full screen at 640x480. Its **Direct3D 7 test
  fails at step 46, `GetDC`, `0x88760249` (`DDERR_CANTCREATEDC`)**;
  whether a real Voodoo 2 fails it too is open (the card has no GDI).
  The Direct3D 8 test runs on `d3dpt-vga` (DirectX 8 has no device for a
  DDI 7 driver) and the 9 one is skipped. Recipe: `NO_DRIVER=1
  EXTRA='-device voodoo2,addr=0x05'
  GUEST_CMD='C:\WINDOWS\SYSTEM\DXDIAG.EXE' CLICKS=…
  tools/win98-game-test.sh <image> <name>`.

## 11. Two Glides on one card: 3dfx's login helper

3dfx's driver installs a Run entry, `Voodoo2`: `rundll32.exe
3dfxv2ps.dll,UpdateRegSettings` at every login. That DLL, the driver's
DirectDraw/Direct3D HAL on **Glide 3**, brings the board up through
Glide 3's own copy of the init library:
`sst1InitMapBoard` (a first map shuts the video down) and
`sst1InitRegisters`, which zeroes the video timing and resets `fbiInit7`,
turning the command FIFO off. Under TCG, with the init library's
200 000-iteration clock-settle loops (`init/video.c`), that takes **3 to
19 s**.

**A Glide program started inside that window loses.** It opens its
window (video mode, `sst1InitCmdFifo(FXTRUE)`, a first swap); then the
helper's init, from another process with its own static state, turns
the display off, zeroes the video registers and the FIFO. The program
goes on writing packets into the window at `0x200000`, where with the
FIFO off they are register writes: a garbage `videoDimensions`,
`intrCtrl` fatals, `fbiInit1`'s SLI bit, and a guest spinning on
`cmdFifoRdPtr`. By hand it looks like a game freezing with a stray
`rundll32` in the task list. `VOODOO2_TRACE=1` names the module behind
each register write (the PE image around the guest's program counter):
`GLIDE2X.DLL` under GLIDETEST.EXE, then `GLIDE3X.DLL` under
RUNDLL32.EXE.

**The device names it.** A config write of `initEnable = 1` (how
`sst1InitRegisters` opens) while the command FIFO is on can only be
someone else's init, because Glide's own close turns the FIFO off first.
The device warns `the card is being re-initialised (sst1InitRegisters)
while a Glide window has the command FIFO on`.

### The stranded client's packets are refused

At a game's close that warning cannot fire, because the close already
cleared `fbiInit7`. FIFA 2000 did an ordinary `grSstWinClose`, then a
full `sst1InitRegisters`, and then a client streamed packets into the
window with the FIFO off. A **healthy** reopen is unmistakable
(`VOODOO2_TRACE=1` on GLIDETEST):

    initEnable <= 00005001
    wr 0001e0 020101c2   cmdFifoBaseAddr      wr 0001f0 001c1ffc   amax
    wr 0001e8 001c2000   rdPtr                wr 0001f4 00000000   depth
    wr 0001ec 001c1ffc   amin                 wr 0001f8 00000000   holes
    wr 00024c 0ffb8300   fbiInit7, bit 8: the FIFO on
    initEnable <= 00005003

FIFA's had none of it: the client resumed on the old ring after someone
else's init switched the FIFO off under it, spun, and the machine reset
five seconds later. Why a client resumes on a dead ring is open (track
doc).

**So the walk is refused by default.** Glide writes that window only
when it believes the FIFO is on, so every dword there with the FIFO off
is a stranded client's packet. Walking the register file with them ruins
the card for everything after: `videoDimensions` zeroed and never
rewritten (no retraces, `status` never idle), `fbiInit7` flipping the
FIFO at random, `intrCtrl` reaching `fatal()`. A real chip survives this,
so the permanence is the emulation's. `fifo-off-regs=on` is the A/B, the
walk as 86Box decodes it. Offsets below `0x100` pass either way: under
Glide's alternate mapping those are the vertex and triangle registers,
and a stray triangle renders and is over.

The warning also **names both sides**, once, without the trace: `the
packet comes from …` (module and call chain around the guest's program
counter, with its `cr3`) and `the last sst1InitRegisters was …`. The
same `cr3` in both is one program re-initialising under itself; a
different one is a second Glide client.

The `voodoo-guest` stranded-client phase writes a burst into the window
with the FIFO off, at the offsets of `cmdFifoBaseAddr`,
`videoDimensions` and `fbiInit7`; the ring's register must read back
unmoved, and move with `FIFO_OFF_REGS=on` (`03010300 -> 02AD02EF`).

### The guest tools' start-up guard

`SETUP /I 6` on 98/Me with a 3dfx card present
(`guest-tools/src/v2start.c`) moves 3dfx's `Voodoo2` value out of HKLM's
Run key into `HKLM\SOFTWARE\2ksbox\Voodoo2` (`Command`) and puts
`C:\WINDOWS\V2START.EXE` there instead. At login V2START runs the
command and waits for it (up to 120 s). If the init is still going after
half a second it shows a small topmost notice, "Voodoo 2 driver is
loading, please wait before running 3dfx games". It informs and does not
block (user decision). When done it writes `C:\WINDOWS\V2START.LOG`,
which harnesses wait on (`tools/win98-game-test.sh`, `VOODOO_WAIT` the
cap; an image without the guard gets a fixed wait). A driver reinstalled
after SETUP puts the Run value back; V2START moves it again at the next
login and waits for the rundll32 Explorer already started instead of
starting a second init.

A guest program that waits at login must not poll (a DOS-box CHOICE loop
made the helper's init twelve times slower). Use `TESTS\WAITFILE.EXE`
(`docs/testing.md`).

## 12. The dither, undone (`undither=on`)

The chip renders colour at more than 16 bits and stores RGB565 through an
ordered dither. 3dfx's RAMDAC box-filtered the scanout, partly undoing it
("22-bit colour"). Two filters exist here.

**`filter=on`** is 86Box's, leilei's approximation of the RAMDAC: a pair
of 256x256 blend tables (`voodoo_generate_filter_v2`) applied in
`vid_voodoo_display.c`. On a Voodoo 2 it is a **single-scanline** pass
(`voodoo_filterline_v2()`, taps at `src[x±1..3]`), so it leaves the
vertical half of every pattern. It runs only once the guest has written
a non-zero threshold to **`maxRgbDelta`** (0x230, `SST_scrFilter`, with
`initEnable & 1`); nothing seeds `scrfilterThreshold`, so it is a no-op
until 3dfx's driver programs it. It is kept as what the hardware looked
like.

**`undither=on`** (`voodoo/undither.c`, ours; the launcher's "Voodoo3
undither filter" checkbox, named after the Voodoo3's "22-bit" filter
that users know the idea by, doc 07) inverts the dither. The
matrix is the table the rasterizer dithered with
(`86box/vid_voodoo_dither.h`, indexed by `(real_y & 3, x & 3)`), and for
a linear non-SLI buffer the phase at scanout is `(y & 3, x & 3)`, known
exactly. Inverting the table gives, per phase and stored code, the
interval of 8-bit values that dither to it. Pixels from one pre-dither
colour have intervals with a common member, and the intersection is what
that colour can have been. Two window sizes, in order:

- **4x4, centred.** The interval is exactly **one value wide** for every
  value and phase of both 4x4 tables: the colour the rasterizer had.
- **2x2, anchored.** The fallback where a 4x4 holds more than one colour
  (an edge, a steep gradient), within 2/255 (`dither_rb` 2, `dither_g`
  1, both 2x2 tables 1).

The 4x4 stage is not optional: a 2x2's midpoint **moves with the
phase**, so 255 of 256 flat colours come back from a 2x2 alone with a
1-LSB pattern, and all 256 exact with the 4x4 first. It is cheap, the
intersection of four 2x2 results a row already has.

An empty intersection at both sizes proves no single colour could have
dithered into those pixels, so they are an edge, written as the
unfiltered path writes it (`code << 3`, `code << 2`). **An edge is an
arithmetic impossibility, not "a difference bigger than N"**: no
threshold, no blur across edges, noisy texture untouched.

Error against what was rendered, on a synthetic frame (sky gradient, lit
sphere, checkered ground, noise panel) through the real tables:

| | mean | worst |
|---|---|---|
| dithered, as the card shows it | 3.11 | 14 |
| `filter=on` (threshold 0x202020) | 2.91 | 42 |
| a naive 2x2 box, for reference | 4.94 | 90 |
| `undither=on` | **0.92** | 14 |

The box filters smear the checkerboard and the noise. The undither's
worst case is the unfiltered one, since that is what it writes where it
cannot fire.

**The check is `voodoo-guest-undither`** (`UNDITHER=on
tools/voodoo-guest-test.py`): one grey (130,130,130) blended onto itself
must come back flat at exactly that colour (`0 interior pixels are not
the one colour (130, 130, 130)`), where the setting off gives the 4x4
tile. The outer two rows and columns are exempt (clamped window).

It runs in `voodoo2_present()` over the front buffer's stored 565 codes
in `fb_mem` (`frame->line[]` is what 86Box already made of them), on the
main loop with the BQL, once per presented frame. **It costs 1.4 ms a
frame at 640x480** on the M1 Air: `decode_row` 0.4 ms (the scalar
gather), `quad_row` 0.1, `emit_row` 0.9. The loops are plane-at-a-time,
the CLUT lookup is skipped for an identity ramp, and `combine_span` is
forced to vectorize (clang declines it; 2.2x). That BQL hold is well
clear of the 10–14 ms stalls that made audio click
(`tools/audio-glitch-test.py`), but it is ~20 % of the main loop's core
at Quake II's 147 fps, ~8 % at 60 Hz. **Off by default.**

**Why not a shader in the player.** The dither belongs to the *card*;
the player is the monitor (doc 03). The player's surface has been
through the CLUT, so a shader could invert the codes only for an
identity ramp, and its output exists only in the window, where **no QMP
screendump sees it**. In the device the result lands in the console
surface for screendumps, VNC and the player alike.

It declines a frame it cannot answer for, says why once, and the
ordinary copy runs: the guest is not dithering (`FBZ_DITHER` clear in
the last `fbzMode`: an LFB-blitted menu, a 2D screen), the colour buffer
is tiled, or the front buffer would run past the frame buffer.
`VOODOO2_UNDITHER_PATTERN=4x4|2x2` overrides the pattern `fbzMode`
reports, for the A/B. On any frame it accepts, it supersedes
`filter=on`.

## 13. One order: the ring, the teardown, and the LFB

On the chip a command-FIFO packet, an LFB or texture write and a
register write all arrive in the guest's order. 86Box has **two**
queues, `voodoo->fifo` for frame-buffer and texture writes and the ring,
and upstream's `voodoo_fifo_thread` empties the first before looking at
the second; a register write does not queue at all. So the ring is the
stream that can be overtaken. What restores the guest's order is in
`voodoo/voodoo2.c` ("one order") and patch 72.

### The teardown hang

Glide's `grSstWinClose` writes its last packets and then clears
`fbiInit7`'s FIFO bit. Applied at once, that ended 86Box's consumer loop
(it runs while `cmdfifo_enabled`) with words unrun, so
`cmdfifo_depth_rd != cmdfifo_depth_wr` for ever, and `SST_status`'s busy
bit (0x380), built from that difference, read **busy to every later
`grSstIdle` poll**. Carmageddon's DOS build spun there for minutes:

    voodoo2: 640x480 on: 122 frames (1 new), 0 triangles, 4 writes, 27298405 reads
      in 5.0 s; regs read 0x000:27298401; busy: 0 cmds outstanding (wr 49668 rd
      49668), fifo depth 59495109/59495111

**The fix.** Before a write that reconfigures the command FIFO
(`fbiInit7` clearing the enable, or the ring's own pointers), the device
**runs the ring out**, which puts the register behind the packets, where
the bus puts it. The wait sets `voodoo->flush`, 86Box's own escape, so a
swap in the ring completes without the retrace the display timer cannot
deliver while the vCPU holds the BQL. It is bounded at 250 ms
(`VOODOO2_DRAIN_MS`, a one-time message when hit); a consumer parked in
an unfinished packet returns at once. Behind it, a write that leaves the
FIFO off with words counted and not run equalises the depths: nothing
will run them, and a card busy for ever is worse than a lost packet.
This fires only on the off transition, so a `cmdFifoDepth` write with
the FIFO already off (a deliberate difference) is untouched.

The `voodoo-guest` teardown phase puts a cyan fill and a swap in the
ring and turns the FIFO off at once; the frame must be cyan and the
status idle. It takes the screendump half a second after the idle poll
ends, because the swapped frame is scanned out over the *next* frame
period.

### The swap is counted twice, on purpose

`SST_status`'s busy bit ors in `cmd_written + cmd_written_fifo -
cmd_read`. On a Voodoo 2 a swap **packet** increments `cmd_read` but not
`cmd_written_fifo` (only Banshee and later count one,
`vid_voodoo_fifo.c`). Glide balances it by also writing `swapbufferCMD`
to the register window, which with the FIFO on only counts (status bits
31:28's swap backlog). A program that sends the packet alone drives the
difference negative and the card reads busy, so the guest test writes
both. Correcting either counter alone leaves BUSY stuck.

### The memory FIFO and the ring interleave (patch 72)

Upstream's FIFO thread drained the memory FIFO once per wake, then
stayed in the ring as long as the guest fed it (in a race, the whole
frame, ~20,000 words behind). Carmageddon writes its HUD with
`grLfbWriteRegion` (~23,000 dwords a frame) before the frame's packets,
so the HUD landed after the next swap and **flickered or vanished**.
Letting the ring yield whenever the memory FIFO had something broke the
other direction: the LFB write jumped ahead of ring words published
before it, and the world painted over the HUD.

**Neither queue goes first.** Patch 72 tags each memory-FIFO entry with
the ring's write pointer at queueing (`cmdfifo_mark`). The entry runs
once the ring has been consumed that far, and the ring loop yields the
moment the memory FIFO's head is due: the guest's order, with no wait. A
mark the ring can never reach (a `cmdFifoDepth` write resets the read
pointer under queued entries) is *stale* and taken as due; otherwise the
next guest write blocks in `voodoo_queue_command`. Draining the memory
FIFO from the guest's side instead is wrong: it means holding `flush`
while a swap goes past, and `flush` flips the buffer where the beam
stands (the teardown frame tore 64 rows down).

The 5 s line's `N publishes behind the LFB queue` counts ring publishes
made with the memory FIFO not yet empty, the case patch 72 sequences; a
consumer already parked in an unfinished packet is out of its reach
(`partial`, 0 in every run so far). `lfb-order=on` adds the mirror, a
wait for the ring before an LFB or texture write, kept as an A/B. The
ordering phase (192 full-screen fills in the ring, a 38,400-dword LFB
block, then the swap) states correctness but does not discriminate: that
needs a rasterizer really behind, which is a game under TCG, read
through the 5 s line.

### A card cannot be busy with nothing to do

No event resynchronises the `written - cmd_read` difference, so when the
two swap counts fail to cancel for any reason it stands for ever.
Carmageddon froze on its first race frame with `wr 3243 rd 3242`, the
ring caught up and 26 M status reads in 5 s.

So the device puts the difference back, with a one-time message, when
the guest polls status and every sign of work is clear **for 20 ms of
continuous polling** (`VOODOO2_IDLE_MS`): both FIFOs empty, the ring
caught up, no render thread busy, no swap pending, the consumer not in
its loop. The hold makes it safe, because between dequeueing a command
and counting it the consumer briefly looks exactly like this. The gate
is the idle state itself, not polls since the last guest write:
`cmd_written_fifo` rises on every triangle packet (300,000 in 5 s in a
race), so that rule never fired.

### LFB writes by buffer

`LFB writes: F to the front buffer, B to the back, E elsewhere, rows
lo..hi` in the 5 s line says which buffer each write aimed at
(`fb_write_offset` against `params.front_offset` / `back_offset`) and the
rows touched, for a flickering overlay. A few writes land on the wrong
side of a swap in progress, so a small `front` count beside a large
`back` one is noise.

### The ring through MMIO counts what is contiguous

**Glide writes a two-word packet value first, header second** (every
single-register packet: `color1`, `fastfillCMD`, `swapbufferCMD`). The
chip's `cmdFifoAMin` / `AMax` / `Holes` count what has been written
*contiguously*, and the depth advances over that alone. 86Box counted
`cmdfifo_depth_wr++` on every write, so a caught-up consumer woken by
the value read the header slot's stale word from the last lap. A stale
type-1 header ("256 values follow") ate the next 256 words, the next was
decoded as a Banshee packet (`fatal()`), and **Carmageddon's race start
froze on `ramfifo=off`**. The RAM ring never had the bug: its walk stops
on a poison header.

The device does the chip's counting on this path
(`voodoo2_mmio_ring_write`; `mmio-holes=off` is 86Box's count). A word is
stored where 86Box put it and counted only once every word before it has
arrived; a word ahead of the next expected one waits in a 64-bit bitmap
until the gap closes. The write side **follows the ring as the consumer
will** (`voodoo2_mmio_step`, header by header with
`voodoo2_packet_words`), so a JMP moves the expected address to its
target. Glide writes `cmdFifoAMin` / `AMax` only at `grSstWinOpen`, so
without that the base's value-first pair after every wrap reads as a
jump back and the next lap is held behind a hole. The count starts where
`cmdFifoRdPtr` says, and the ring's registers (`cmdFifoBaseAddr`,
`RdPtr`, `Depth`, `fbiInit7`) restart it. A write that lands where
nothing expected it (a hole wider than the bitmap, a ring taken up with
no register written) is counted as it comes, with a warning naming the
first. The 5 s line says `N ring words written ahead of a hole, M
counted with one open, K jumps followed`.

The `voodoo-guest` **swapped-pair phase** checks it
(`voodoo-guest-mmiofifo` is the variant that discriminates): a stale
type-1 header claiming 256 values sits in the ring's second slot, the
consumer rests on it, the `color1` value goes in first, `cmdFifoDepth`
is polled to zero, then the header, a fill and a swap; the frame must be
yellow. The **wrap phase** does the same across a JMP back to the base;
the frame must be green.

### The 8 MB board is the default

With the ring exact, Carmageddon still crashed 20–50 s into every race,
printing a Glide error through BRender's `FATAL ERROR: %s` (a page fault
in the game's `strnlen`). The one card-dependent error of the nine is
`grTexDownloadMipMapLevelPartial: mipmap level cannot span 2 Mbyte
boundary`. The game sizes its texture space from `grTexMinAddress` /
`grTexMaxAddress`, and the device reported **4 MB per TMU** (the 12 MB
board); a 1997 allocator written for 2 MB TMUs walked past the 2 MB line.
An 8 MB Voodoo 2 (86Box's default too) has 2 MB TMUs and cannot reach
that error. **`texmem=2` is the default**; `texmem=4` is the 12 MB board
for a title that wants it.
