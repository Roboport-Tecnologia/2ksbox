# Track M9: TCG on Apple Silicon

Making the emulated CPU faster on the M1 Air, an aarch64 host where TCG
is the only option. M8 (docs 13 and 16) did floating point; this track
took the rest of the vCPU's time, each change chosen from profiles of
real workloads. This doc is the track's record: scope, how to profile,
what each patch found and bought, the hardware-MMU verdict, the lessons
and what is open. Elsewhere:

- how each patch works: its row in `patches/qemu/README.md` and doc 22 §3;
- the queue's measured effect, ablated per switch: `docs/22-tcg-evaluation.md`;
- the literature and spikes of its ideas: `docs/23-dbt-literature.md`;
- patch 21's design: `docs/18-pinned-guest-registers.md`;
- every tool named here: `docs/testing.md`.

## State

- All on `main`. `track/m9-tcg-aarch64` is deleted; `track/m9-hwmmu`
  stays on the remote, parked. The patches are 13–20, 24, 35–39 and
  41–45 (21 until M21). Each has an `-accel tcg` or `-cpu` switch (patch 29 gave the
  last three theirs) except 13, 14 and 41, which change no guest-visible
  behaviour.
- **Optimization is closed by user decision (2026-09-12).** Both 3DMark
  99 game tests sit at the 60 Hz flip cap. The Mac verification is done.
- **The hardware-MMU design is abandoned for the time being** (user
  decision, 2026-09-16): 1.1–1.2x on every workload ("Gauging the gain").
- **Patch 21 (pinned registers) is gone**: parked on 2026-09-16 (off by
  default and not offered, user: too unstable for too little gain), then
  not ported to QEMU 11.1 (M21, user decision, 2026-10-01: 10.1's TCG
  backend rewrite made the port a rewrite). Doc 18 keeps the design.
- The rest were ported to QEMU 11.1 in M21 (doc 22's
  numbers are 9.2's until re-measured).
- Later TCG work has other owners: x87 at PC=64 (patches 47–49, 67) is
  doc 13's, the code-buffer placement (patch 63) doc 22 §5.0's.

## Scope and files

All under `tools/`:

- profiling: `tcg-profile.sh`, `tcg-profile.py`, `tcg-hot.py`,
  `tcg_profile_lib.py`, `tcg-fps.py`, `tcg-perf-cut.py`,
  `tcg-form-weights.py`, `memtrace.c`;
- workloads: `xp-moto-race.sh`, `moto-watch.py`, `w98-3dmark.sh` +
  `w98-3dmark-tests.py`, `w98-blood.sh`, `smc-diff.py` (two captures of
  a code page, diffed and disassembled);
- oracles: `rep-guest-test.py` (patch 17), `smc-guest-test.py` (18, 24),
  `string-bench.py`, the x87 / SSE batteries (36–39, 45);
- the VM design: `hvf-el1/` (the EL1 probe) and `hwmmu/` (census and
  projection). Their READMEs own the tool detail, this doc the reading.

Plus the QEMU patches listed under State. Shared, edited minimally:
`tools/qmpc.py`, `scripts/test.sh`, `patches/qemu/README.md`.

## How profiling works

`tools/tcg-profile.sh <image> <name> ['guest command']` boots the image
headless as a snapshot with `-perfmap`, types the command into the Run
dialog, waits `WARM` seconds and samples QEMU for `SECS` (macOS `sample`
at 1 ms, `perf record` on Linux). `tcg-profile.py` splits the vCPU
thread's self time by kind (generated code, helpers, softmmu slow path,
translation, waiting...) and maps generated-code samples through the
perf map to guest modules, pages and instructions. `--hot` prints a
`-dfilter` list; a second run with `DFILTER=` logs those TBs' guest
bytes, TCG ops and host code, and `tcg-hot.py` weights them by samples.
For a run with phases (3DMark), `perf` the whole run
(`w98-3dmark.sh <name> whole`) and cut it with `tcg-perf-cut.py` by a
test's seconds from `tests.txt`.

Traps:

- `sample` shows no thread names (the vCPU thread is the one in
  `cpu_exec_loop`) and cannot unwind through generated code (those
  samples are leaves under `cpu_tb_exec`).
- **The perf map spans every code-buffer epoch.** After a `tb_flush` one
  host address names a different TB. The report takes the epoch from
  `info jit`'s flush count before and after the sample; a window that
  straddles a flush is mis-mapped for its earlier part.
- `-perfmap` costs up to 12 % of the vCPU on a retranslation-bound game
  (`PERFMAP=0` for fps runs). `DFILTER` logs every retranslation (6 GB in
  30 s on Moto Racer): `log none` over QMP, or filter a cold page.
- Without capstone, `-d out_asm` is raw hex; `tcg-hot.py` classifies
  aarch64 words by opcode field instead.
- `sample`'s PC is the oldest unretired instruction, so samples skid
  (a "41 % on register loads" was skid off the softmmu chain).
- In `hot.txt` a TB's last guest instruction carries patch 06's slow
  blocks, so per-instruction averages there mean nothing.
- On the Brazilian XP images Program Files is `C:\Arquiv~1`, and US-Intl
  dead keys swallow the space after a typed `"`. Type 8.3 paths unquoted.
- perf cannot unwind out of glibc's AVX `memcpy`/`memset`;
  `memtrace.c` (an `LD_PRELOAD`) counts them per caller.

**Frame rates.** `%` of a busy vCPU says where time goes, not how much
work gets done, so the A/B is a game's frame rate beside a fixed-work
oracle (7-Zip, Super PI). `tcg-fps.py` counts distinct screendumps: it
misses 3D presents and a still camera, and saturates near 40–52 fps.
Prefer the adapter's `d3dpt-vga: N page flips in 5.0 s` line, or VBE
index-9 writes for a Build-engine game. `DDFLAGS=32768` takes the 60 Hz cap off.

## Findings, patch by patch

The first profiles ran on XP SP3, `-cpu pentium3`, one vCPU; 43 % of
7-Zip's generated-code samples sat on the softmmu TLB lookup. The games
then showed pathologies, not "TCG is slow". Gains are at landing; doc 22
ablates every switch.

| patch | cost found (workload) | fix | gain at landing |
|---|---|---|---|
| 13 | `-perfmap` Linux-only | built on every host | the profiler |
| 14 | 12 % in `pthread_jit_write_protect_np` (Super PI) | W^X state tracked per thread | Super PI 1M 1:36.2 → 1:25.3 |
| 15 | TB invalidation storms: vAPIC stubs, list walks per data write, a jump-cache flush per TB (Moto Racer intro) | four cuts on the write path | TB maintenance 25 % → ~9 %; 7-Zip +2–5 % |
| 16 | dynamic TLB at 64–256 entries, 6 M victim swaps/s (Moto Racer) | 4096-entry floor | slow path 49 % → 1.3 %; race 4.9 → 7.2 fps with 15 |
| 17 | 80–90 % of the demo's generated code one `rep movsd` blit | REP MOVS/STOS as host `memcpy`/`memset` per page run | 2.15 → 0.07 ns/element; race unchanged |
| 18 | race 53 % translation: ~700 k code-page stores/s, 94 % same value | same-value store invalidates nothing | race 7.3 → ~39 fps (probe-bound) |
| 19 | 8.8 % in `_tlv_get_addr`: nested RCU locks per code-page store | locks and `tcg_ctx` read once | 8.8 → 2.5 %; standing start 37.9 → 40.4 fps |
| 20 | 13.9 % in `helper_lookup_tb_ptr` (7-Zip) | jump-cache probe inline, branch-free | 7-Zip compress +12 %, decompress +7 % |
| 21 | guest registers through `env` at every block boundary | `eip` + GPRs in x20–x28 across chained TBs | 7-Zip decompress +16 %, compress +3 %; parked |
| 24 | the other 6 %: operands really patched per span (Moto Racer, Blood) | soft immediates, loaded from the code bytes | race 41 → 58 fps; Blood 9.4 → 131 fps |
| 35 | 57 % of QEMU walking TB lists of Win9x code/data pages (3DMark 99) | 64-chunk code map per page | 3334 → 5894 3DMarks |
| 36 | a 16-byte SSE operand stored as two halves, reloaded as one | assembled in the vector unit | CPU 3DMarks +16 % |
| 37 | x87 PC=24 recomputing an inexact flag already sticky | TB flag for sticky PE | CPU 3DMarks +8 % |
| 38 | the inline lookup rebuilding mode bits from `env` | the leaving TB's bits as constants | CPU 3DMarks +5 % |
| 39 | SSE lane checks through `env` | `vec_allsign_i32` TCG op | CPU 3DMarks +4 % |
| 41 | a 13.6 KB zero-fill per translation (8.7 GB a run) | `DisasContext` uninitialised | no measurable change |
| 42 | jump cache emptied by 2,400 CR3 writes/s (Win98) | generation-stamped, kept, 64 K entries | CPU 3D Speed window 18.2 → 19.0 fps |
| 43 | 1.85 M main-loop entries/s from non-jump block ends | chain when no interrupt is pending | 12.3 M → 5.7 M entries per 10 s |
| 44 | ~185 page walks per CR3 flush | retire the TLB, reuse checked entries | 95 % of walks gone; within noise on the Ryzen |
| 45 | x87 PC=24 rounding through two conversions | binary32 shadows | CPU 3DMarks 16295 → 16899 |

Not a patch: an executor fix (`5d07018`, DX7 indexed draws hand DXVK
only their vertex range), and a stats build that found patch 18's 94 %.

## Moto Racer 1997: three self-patching loops

The workload is the software renderer at 640×480×16 (`winxp-m7.qcow2`,
`DDFLAGS=32` forces it; the Direct3D 5 renderer runs at 77–120 fps and
invalidates nothing). `smc-diff.py` found three hot spots:

- `0x46054e`, the `rep movsd` rectangle blit (37 host instructions per
  dword before patch 17). It dominates the attract demo, whose fixed
  tick hides any gain. Measure the race.
- `0x436000`, the textured span loop unrolled four times: texture base,
  u/v steps and carries patched per span (patches 18 and 24).
- `0x4357f0`, the translucent RGB565 span loop, 14 immediate fields per
  use (the tyre smoke, below).

`tools/xp-moto-race.sh` drives the menus to a Speed Bay practice race
with the throttle held. The fps depends on the track section (three
mid-race windows of one binary read 19.0 / 20.3 / 21.5), so the A/B is
the standing start, both binaries back to back. `RACE_DELAY=` picks the
moment; `RACE_SAMPLE=` profiles inside the race.

### The tyre smoke

The user reported that braking "almost hangs the game" in the software
renderer. `tools/moto-watch.py` (per-second `info jit` counters while it
drives the bike) showed host code generated going from 30–36 MiB/s under
throttle to 57–69 MiB/s in the brake's first second, all in the
translucent span loop. Patch 24 absorbs its writes: invalidations
31,045 / 36,548 a second → 0 / 1, the race 41 → 58 fps mean (worst
window 32.4 → 44.8), at the 60 Hz cap for most of it. Headless the smoke
is a thin trail; the user's full plume was not reproduced.

## Blood: the shift counts

Blood (DOS Build engine in a Win98 DOS box, 640×480 VESA) ran 9.4 fps
facing the starting corridor and 154 facing a wall, with ~40,000
retranslations a second. 95 % were one column loop at `0x8ae56623`,
which patches 17 disp32, 5 imm32 and **14 imm8 shift and rotate counts**
per column; patch 24 kept the counts constant, so the block failed soft
four times and gave up. Patch 24 now makes shift/rotate counts (and
`gen_IMUL3`) soft, and hashes its invalidation counters
multiplicatively: two blocks two bytes apart had shared a `pc >> 2` slot
and reset each other. Corridor 9.4 → 131 fps, wall 154 →
556, invalidations per 10 s ~460,000 → 18. Recipe: `tools/w98-blood.sh`
(`BLOOD.EXE -quick -map e1m1`, VBE index-9 writes as the frame counter).

## Win98 3D: 3DMark 99

The user found Win98 3D "a bit underwhelming" and asked for 3DMark 99's
first-person test at 60 fps, TCG only. The benchmark is 3DMark 99 Max on
`claude98` (Win98 SE, DirectX 9.0c, our display driver, 800×600×16,
`-cpu pentium3`), driven by `tools/w98-3dmark.sh`.

**The correction.** The "first person" fps this work was steered by
(4.5 → 19.5) was the 40–56 s window after the click, mostly the
**Synthetic CPU 3D Speed** test; the first-person test (25–45 s) read
60.0 on 3DMark's own counter in every run since 2026-09-11's evening.
Those figures are withdrawn as game numbers. `tests.txt` places each rate
line by the test on screen (a screendump every 5 s). Read a test's rate
there, never off a bare rate line.

| step | 3DMarks | CPU 3DMarks |
|---|---|---|
| before | 3334 | 10969 |
| patch 35 | 5894 | 11648 |
| 36 / 37 / 38 / 39 | 5912 / 5917 / 5932 / 5950 | 13549 / 14690 / 15389 / 15940 |
| executor fix, 41 | 6014 / 6012 | 15712 / 15483 |
| 44 (with 42, 43) | 6003 | 16295 |
| 45 | 6005 | 16899 |

CPU 3DMarks moves ±2–3 % between identical runs. The uncapped game
tests and the per-switch ablation are doc 22 §6.2.

The vCPU thread was 98 % of wall time and the executor + DXVK under
1 %, so every lever was TCG. `info jit` twice inside the window
(`JIT_SNAPS="43 53"`) found the 2,400 same-value CR3 writes and 1.85 M
main-loop entries a second behind patches 42–44 (doc 22 §3.2, §3.4);
MSVC's `_ftol` alone does two `fldcw` per call. The driver's
vertical-blank wait (34,000 FRAMES polls a second) is wall time, not a
lever.

**User rules for this work (2026-09-11).** Only optimizations that help
*any* guest: no title-specific hooks or DLL replacements. Exact work
before inexact. A per-machine `fp-relaxed` switch (off by default: SSE
without per-op checks, x87 without PC=24 rounding, no FIP/FDP stores)
was agreed in principle and deferred.

## The HVF EL1 probe

The structural fix for the TLB chain: run TCG's Arm output inside a
Hypervisor.framework VM whose stage-1 tables mirror the x86 guest's page
tables, so a guest load is one host load. `tools/hvf-el1/` is a 37 KiB
bare-metal Rust guest at EL1 plus a host sharing a 64 MiB arena at the
same address in both worlds, so `env`, the TB cache and guest RAM could
stay where QEMU has them. Raw output is in
`tools/hvf-el1/results-*m1air-2026-09-05.txt`. The VM offers a 4 KiB
granule, 8-bit ASIDs, 36-bit IPA, no hardware A/D updates and
`DIC/IDC=0` (cache maintenance after JIT writes).

| primitive | ns |
|---|---|
| VM exit (`hvc` round trip; MMIO load via stage-2 miss) | 845–870; 830 |
| in-VM exception with a full frame | 40 |
| in-VM helper call (`bl`) | 0.9 |
| host thread kick → guest IRQ handler | 1,900–2,200 |
| first touch of a mirrored page (abort → x86 walk → `eret`) | 110–160 |
| hardware TLB miss, tables warm | 6–6.5 |
| x86 #PF delivered to the resume point | 67 |
| first write to a clean page (fault → D → remap → `tlbi`) | ~495 |
| CR3 switch with ASIDs, tables kept | 19 |
| JIT patch + cache maintenance + call | 134 (native with W^X: 173) |

An exit costs ~900 helper calls and 7-Zip calls a helper every ~20 ns,
so **helpers must run inside the VM**: the vCPU core (~45 KLOC: `tcg/`,
`accel/tcg/`, `target/i386/tcg/`, softfloat, qht) built freestanding,
`cputlb.c` rewritten as the fault-driven mirror, and a mailbox to the
device model for I/O, interrupts, DMA invalidation and clocks.

Against today's softmmu sequence, a mirrored load is 1.4–2.6x faster
while the working set fits the L2 TLB (3072 entries, 12 MiB); beyond it a
dependent random load is ~2x slower (nested walks). The TLB's mask/table
in registers buys 0 % on dependent loads, 10–20 % on independent ones.
On the Moto Racer blit loop (ns per dword): today 2.1, pinned registers
1.17, mirror + pinned 0.40, a per-page-run memcpy 0.09, which is why
patch 17 came first.

**A hazard for any port.** In the VM, a store to `env` followed by a
store through the window costs ~2.2 ns extra per pair when `env` is in
2 MiB global block mappings and the window in 4 KiB non-global pages
(the `diag3` kernels). Everything translated code stores to (`env`, the
TCG stack frame, the TLB, the TB cache) must be mapped like the guest
window. Which attribute triggers it is unmeasured.

The verdict (2026-09-05): feasible, a first XP boot in 4–8 weeks, with
one known risk, the nested-TLB penalty on working sets beyond 12 MiB.

## Gauging the gain

`tools/hwmmu/` multiplies the priced primitives by what the workloads
do. Doc 22 §8.2 holds the census tables, the per-workload projection and
the decision in full; this section keeps the probe's workload-shaped
kernels, `mix4` / `mix12` (an independent load with four / twelve ALU
ops behind it) and `copy` (load + store), each as softmmu and as the
mirrored access (`docs/22-data/hwmmu/probe-clean.txt`, alone on the Air):

| set | mix4 direct / softmmu (ns) | mix12 | copy pair | Δ per load | Δ per store |
|---|---|---|---|---|---|
| 64 KiB | 0.69 / 0.97 | 1.63 / 1.98 | 0.40 / 1.17 | 0.28 | 0.46 |
| 4 MiB | 1.04 / 1.40 | 1.98 / 2.46 | 1.47 / 2.18 | 0.36 | 0.38 |
| 8 MiB | 1.17 / 1.79 | 2.14 / 2.81 | 1.52 / 2.70 | 0.62 | 0.66 |
| 16 MiB | 2.37 / 4.11 | 4.26 / 5.96 | 3.39 / 5.39 | | |

With ALU work to hide behind, the chain costs 0.3–0.6 ns per access,
not the 2–5 ns of a dependent chase. In the census, no 65,536-access
window on six workloads touched more than 1,600 pages, so the nested-TLB
risk does not occur. Projected gain
(`tools/hwmmu/project.py --reuse`): **7-Zip 1.20x (a floor), Super PI
and Quake II 1.16x, Blood 1.10x, the FP kernels 1.05–1.07x**. The
profile's "43 % on the chain" suggested two to three times that, because
samples land on the chain's stalls while the core overlaps them with
other work. A 3D title's CR3 rate and dirty logging were not censused.

**Decision (the user, 2026-09-16): abandoned for the time being.** A
fifth is not worth a freestanding vCPU core, a rewritten `cputlb.c` and
a mailbox protocol. The probe, census plugin and projection stay for the
day a workload changes the number.

## Lessons that generalize

- **Measure the workload where the user runs it.** Patch 17 was 30x on
  Moto Racer's demo loop and nothing on the race.
- **Place the window by what is on screen** ("The correction", above).
- **Count before profiling.** `info jit` twice inside the window gives
  rates a profile only hints at; temporary counters (a stats build, a
  refusal trace) found patch 18's 94 % and Blood's hash collision.
  Remove them before cutting the patch.
- **A/B before crediting.** The drivers' byte-loop `memcpy` looked like
  30 % of a window and changed nothing when fixed. 3DMark's page walks
  looked like 13 % (they were the slow path's entry); removing 95 % of
  them moved 0.1 fps.
- **Group samples by instruction form** (`tcg-form-weights.py`). A
  memory-operand `addps` at 10 samples against 2.7 for the register form
  was patch 36.
- **A right answer does not prove the fast path ran.** Every battery here
  asserts the path was reached (soft-imm's trace events, absorbed writes
  at four cases' fields); the x87 battery gained a sticky-PE sweep after
  patch 37's variant turned out never to have been translated by it.
- **A new TB flag has two homes,** `cpu_get_tb_cpu_state` and patch 20's
  `gen_lookup_and_goto_ptr`. Patch 37 without the second lost 25 %.
- **Any list of TLB indexes must hold the table's largest index.** Patch
  44's `uint16_t` list wrapped past 65,536 entries and Win98 died a few
  seconds into `SETUP.EXE`.
- **Cut a patch's diff against the prepared tree**, and save work in
  progress as a patch file before any `build.sh`: prepare wipes unqueued
  edits in `qemu/`, and leftover lines of a dropped patch become context
  the forward-apply refuses.
- `QEMU_TCG_OPTS=<switch>=off` is the A/B of an accelerator switch; a
  second `-accel` on the command line is ignored.
- Upstream already skips clean MMU indexes in a flush (`c.dirty`); an
  `info jit` flush event counts dirty indexes (Win98 dirties three).

## Open

1. **Patch 21's crash** (moot while 21 is not ported, M21). XP reboots or bugchecks with registers pinned:
   seen at 8, at all nine (doc 22's `pinned` run, Super PI) and at 7
   (doc 23's spike E), so it is the pinned path, not a register.
   `tools/specbench/run.sh <image> pinned` reproduces it. Also, with
   pinning on `helper_cc_compute_c` takes 4 % of the vCPU instead of 1 %,
   695 of 824 samples on its first instruction; the untested theory is
   the `dmb` after the call draining the spill stores. Once it works,
   cheapen the stores/reloads around helper calls (a thunk pair or a
   per-helper "touches GPRs" flag).
2. **aarch64 binary32 at PC=24.** On the Air the single-precision x87
   loop at PC=24 (`X87BEN2S`) takes 0.49 s against PC=53's 0.38 s (the
   Ryzen: both 0.33 s); the m64 loop at PC=24 0.71 s (Ryzen 0.44 s). Not
   profiled.
3. **TLB mask and table as immediates.** Constant once the table size is
   fixed and there is one vCPU: two loads and an instruction fewer per
   access. The probe says 0 % dependent, 10–20 % independent.
4. **Two-page blocks linkable.** 127,000 unlinked `goto_tb` exits a
   second in 3DMark are jumps to a block spanning two pages, which the
   main loop refuses to chain. The x87 / SSE slow blocks' exits to the
   next instruction (~100,000 a second) could chain too.
5. **Carry at TB entry.** Inline the ADC/SBB/shift cases of
   `gen_prepare_eflags_c`, or carry the static `cc_op` into the lookup
   key. ~3.5 % on 7-Zip.
6. **Barriers on one vCPU.** 7-Zip read +2–10 % without them (single
   runs, a hack not in the tree); doc 16's memory-operand bench and the
   probe's blit kernel saw nothing. `-smp 1,maxcpus=1` already drops
   them. A default would need an audit of every reader of guest RAM
   outside the BQL.
7. **x86-64 pinned registers** (rbx, rbp, r12, r13, r15 are free), after
   item 1.
8. **Soft-immediate leftovers**, if a workload asks: jump targets, the
   SHLD/SHRD count, RCL/RCR's 8/16-bit counts, fields past a block's
   first page (Blood's few `why=1` refusals).
9. **The `fp-relaxed` switch**, when the user asks for it.

The games on the x86-64 rig and the evaluation's other open
measurements are doc 22 §9's.

## Build / test loop

```sh
scripts/build.sh                                         # after any patch edit
tools/tcg-profile.sh ~/vms/winxp.qcow2 idle              # ~2 min
CDROM=~/vms/bench.iso tools/tcg-profile.sh ~/vms/winxp.qcow2 7zip \
    'cmd /k C:\Arquiv~1\7-Zip\7z.exe b -mmt1 40'
python3 tools/tcg-profile.py build/tcg-profile/7zip --hot   # the -dfilter line
DFILTER=... tools/tcg-profile.sh ... ; build/venv-capstone/bin/python tools/tcg-hot.py build/tcg-profile/7zip
PERFMAP=0 FPS_RATE=60 tools/xp-moto-race.sh ~/vms/winxp-m7.qcow2 race   # fps A/B
tools/w98-3dmark.sh <name>                               # then build/w98game/<name>/tests.txt
tools/hvf-el1/build.sh && build/hvf-el1/hvf-el1 build/hvf-el1/payload.bin   # ~2 s, macOS
scripts/test.sh all                                      # before every commit
```
