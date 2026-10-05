# 22. Exact fast paths for x86-on-Arm emulation: 2ksbox's QEMU patch queue, measured

*2ksbox project, 2026-09-16. Everything here is reproducible from the tree:
`tools/specbench/build-guest.sh` builds the benchmark suite,
`tools/specbench/run.sh <image> all` runs the matrix,
`tools/specbench/report.py --md` prints its tables, and the `tools/w98-*.sh`
runners take the game measurements. Numbers are from one machine, an M1
MacBook Air, at the commit that carries this document.*

*Since 2026-10-02 the tree is QEMU 11.1.2 (track M21): every patch here
was ported except 21 (`pinned-regs`, §3.6) and those 11.1 made
unneeded, and the numbers below are 9.2.4's until M21 re-measures them.*

## Abstract

2ksbox runs Windows 98 and XP guests on Apple Silicon through QEMU's Tiny
Code Generator (TCG), the software x86 translator. Stock TCG is exact but
slow on the code these guests run: x87 floating point goes through an
80-bit software float library, every guest memory access walks a
software TLB, every `ret` leaves generated code, and the era's software
renderers rewrite their own inner loops often enough that translation
dominates. We describe a queue of sixty patches over QEMU 9.2.4, about
twenty of them performance work, each behind its own switch and each
bit-exact (the guest computes the same bits with the switch on and off).

On **reproducible benchmarks** run headlessly inside the XP guest
(nbench, 7-Zip, Super PI, our own SSE/x87 kernels), the default gains
2.34x geometric mean over pristine QEMU: 5.3x on x87 code, 3.4x on SSE,
1.1x on plain integer code, with identical outputs. On the **games** the
patches were written for (3DMark 99 Max, 3DMark2001 SE, Blood, Moto
Racer, Quake II on Windows 98), every number checked against a
screendump, the default gains 2.2x to 30x over the same tree with every
switch off, because the patches for self-modifying code and for Windows
98's memory manager do nothing to a benchmark program and decide a game.
A SPEC-CPU2006-derived integer suite gains 1.07x.

The literature's remaining ideas were each tried behind a switch or
priced. A pc-indexed jump table and a back-edge-only interrupt check
land within noise; return prediction through a host call/return pair is
a loss. Running the translator inside a hardware VM with the guest's page
tables mirrored projects to 1.1–1.2x on these workloads from a memory
census and microbenchmarks, and was not built.

The evaluation's first finding was about its own method: on macOS/arm64
a third of QEMU launches place TCG's code buffer 8 GiB from the helpers
and run helper-heavy code 35–45 % slower, pristine QEMU included. A
load-time reservation (patch 63) makes every launch the near one.

## 1. Introduction

**The problem.** A Windows 98 or XP guest wants an x86 processor, and no
hardware on an Arm Mac runs x86 code directly. QEMU's TCG translates it:
it decodes guest x86 into an intermediate representation and emits
AArch64 code one *translation block* (TB, a straight-line run of guest
instructions) at a time, caching the result. TCG is correct for hundreds
of guest/host pairs and pays for that generality on every guest
instruction (§2). On the M1 Air the guest runs at roughly the speed of a
2002 Pentium 4: enough for the desktop, and on a game the difference
between playable and not.

**What we did.** The patches were written profile-first: run a real
workload (7-Zip, Super PI, Moto Racer, Blood, 3DMark 99, 3DMark2001 SE),
map the vCPU thread's samples back to guest instructions, and fix the
largest thing the profile named. They live on the pinned QEMU submodule
(`patches/qemu/`, applied by `scripts/prepare-qemu.sh`; the README there
has each patch's full story). Two rules shaped them: **only
optimizations that could help any guest** (nothing title-specific), and
**exact before fast**. A fast path is taken only when it computes the bit
pattern the slow path would have; the one inexact mode
(`x87-pc64-as-53`, §3.1) is opt-in and labelled inexact in the launcher.

§2 is background on TCG, §3 the patches, §4–§5 the benchmark tier, §6
the games, §7 related work (with a SPEC-style suite in appendix A), §8
the literature's remaining ideas tried on this tree. Every claim points
at the patch or tool that carries it.

## 2. Background: how TCG runs x86 on AArch64

A reader who knows QEMU can skip this section.

**Translation blocks and the code cache.** TCG translates guest code
lazily. If the code cache has a TB for (PC, code segment base, CPU mode
flags), execution jumps in; otherwise the translator decodes guest
instructions up to a branch and emits host code. Direct branches between
TBs are *chained* (the host jump is patched to the target TB, so no
lookup happens again). Indirect branches (`ret`, `call *`, `jmp *`) go
through a lookup: a small direct-mapped *jump cache*, then a hash table.
When a lookup fails or an interrupt is pending, execution returns to the
C main loop, a few hundred host instructions each way.

**The guest register file.** TCG keeps the guest's registers in the
`CPUX86State` struct, reached through a reserved host register. The
allocator loads them into host registers inside a TB and stores them
back at its end, before any helper call that might read them and at any
instruction that might fault, so they go through memory at every block
boundary.

**Memory: the softmmu TLB.** Every guest load or store is translated by a
*software TLB*: generated code computes an index from the guest address,
loads the entry's tag, compares it, and on a hit adds the entry's offset
and does the host load. That is about ten host instructions per guest
memory operand, with a dependent load chain in the middle. On a miss a
helper walks the guest page tables. The TLB is flushed whenever the
guest switches address spaces (a `mov cr3`), and QEMU 9.2 sizes it from
the number of entries used since the last flush.

**Helpers.** Anything the translator does not inline is a call into C:
x87 and most SSE floating point (through `softfloat`, which implements
IEEE arithmetic exactly on integers, including the 80-bit x87 format),
integer division, flags for a few instructions, I/O, and every TLB miss.
A helper call costs the register sync above plus the call.

**Self-modifying code.** A store into a page that code was translated
from takes a slow path that discards every TB overlapping the written
bytes; the next execution retranslates. Windows 9x-era software
renderers write their inner loop's operands into the instruction stream
per span, so this path can run thousands of times per frame.

**Floating point.** x87 precision is 64 bits at reset; Windows sets 53
bits at process start and Direct3D sets 24 bits for the life of a
device, and each result must be rounded to that precision, with IEEE
flags. QEMU does all of it in `softfloat` on an 80-bit representation,
and inlines only a few SSE instructions. The host FPU computes binary64
and binary32 natively with the same rounding, so in the common modes one
host instruction gives the same bits, *if* the mode and the operands are
checked.

## 3. The optimizations

Fifteen switches cover the queue's performance patches. Five are CPU
properties (`-cpu pentium3,<name>=off`): `x87-fast`, `sse-fast`,
`simd-fast`, `rep-fast`, and the opt-in `x87-pc64-as-53`. The rest are
accelerator properties (`-accel tcg,<name>=off`): `smc-same-value`,
`soft-imm`, `inline-lookup`, `tb-invalidate-fast`, `tlb-floor`,
`tls-hot-paths`, `jump-cache-keep`, `eob-chain`, `tlb-retire`, and the
opt-in `pinned-regs`. The launcher's machine form exposes all but
`pinned-regs` under "Emulation optimizations" (§3.6 says why), and
`scripts/test.sh`'s `optimizations` check proves the wiring from
checkbox to command line. A misbehaving guest is diagnosed by turning
switches off, not by bisecting patches. Each subsection names the
profile fact behind its patches; the measurements are §5 and §6.

### 3.1 Floating point on the host FPU (`x87-fast`, `sse-fast`, `simd-fast`, `x87-pc64-as-53`)

**x87 at 53 and 24 bits (patches 05, 06, 37, 45; doc 13).** At 53- or
24-bit precision with round-to-nearest, what Windows and Direct3D set, a
binary64 (or binary32) operation on operands representable at that
precision *is* the correctly rounded x87 result, so patch 05 computes it
on the host FPU. Patch 06 keeps the x87 stack as host doubles inside
generated code across instructions (the "shadow" stack), converting to
the 80-bit format only at TB exits, before helpers and on faults. Patch
37 drops the inexact-flag work once the flag is sticky (every program's
state after its first inexact operation), and patch 45 makes the shadows
binary32 at 24-bit precision, so a Direct3D game's x87 operation is one
host instruction.

**x87 at 64 bits (patches 47, 48, 49).** No host format holds a 64-bit
mantissa, so code at the default 64-bit precision (3DMark2001 SE's
Lobby, compilers that never touch the control word) got none of that.
Patch 48 keeps the stack as the x80 values and does `+ - * /` with exact
128-bit integer arithmetic, `*` and `fst m32` inline since patch 49.
Patch 47 is the one inexact switch, `x87-pc64-as-53`, off by default: it
runs 64-bit code at 53 bits, dropping the low 11 bits of every mantissa,
which no sampling of a program's operands can prove harmless. In
3DMark2001 SE the 64-bit code is the debris physics of the *high detail*
Lobby and Car Chase; low detail has none. So high detail with the switch
off and on measures what the inexact mode buys, and low detail is the
control that should not move (§6.2).

**SSE and MMX inline (patches 11, 12, 36, 39; doc 16).** SSE float
operations run on the host vector unit when MXCSR is round-to-nearest
without FTZ/DAZ, all exceptions masked and the inexact flag sticky, a
state a game reaches a microsecond after starting. A vector compare and
one branch per operation catch the cases where host and softfloat could
differ (NaNs, denormals, flags); the slow path is the old helper. Patch
12 inlines the MMX and SSE integer and permutation instructions (with a
new TCG byte-permute opcode, `tbl_vec`). Patches 36 and 39 removed two
store-to-load stalls, in 16-byte memory operands and in the lane check's
branch. On the SSE battery (`tools/sse-guest-test.py`): packed 12x,
scalar 3.6x per instruction on the Air, identical over 546,425 lines.

### 3.2 Memory: the TLB and string instructions (`tlb-floor`, `tlb-retire`, `rep-fast`)

**The TLB floor (patch 16).** QEMU resizes the software TLB at every
flush from the entries used since the previous one. Windows XP flushes at
every context switch (450 a second on an idle desktop), so the table sat
at 64–256 entries for a working set of a few hundred pages, and two live
pages sharing an index missed on every access. The minimum is now 4096.

**Retiring instead of flushing (patch 44).** Windows 98's memory manager
writes CR3 with the *same value* 2,400 times a second under 3DMark 99
(its map/unmap of one page flushes the whole TLB), each flush costing
~185 page walks to refill unchanged entries. A CR3 write now
re-validates the entries filled since the last flush against the guest
page tables and keeps those that match: 95 % of the walks gone.

**REP MOVS/STOS as a host memcpy (patch 17).** Moto Racer's blit is one
`rep movsd`, as are Windows' own `memcpy` and `memset`: 37 host
instructions per dword, ESI/EDI/ECX through `env` every iteration. The
repeat now runs as a host `memcpy`/`memset` per page run inside the
guest's TLB rules, with the architectural state at a fault reconstructed
exactly. 2.15 → 0.07 ns per element; 536 cases in
`tools/rep-guest-test.py` (widths, direction flag, page crossings,
overlaps) agree with a Python model of the instruction.

### 3.3 Code that rewrites itself (`tb-invalidate-fast`, `smc-same-value`, `soft-imm`)

**Invalidation that finds its target (patches 15, 35).** Four cuts on the
path a store into a code page takes. Two carried the cost: XP's vAPIC
TPR stubs sat below a structure the APIC writes at every interrupt, and a
range rounded down to the page start retranslated them each time; and
the per-page range of translated code was wrong for a Win9x module (code
at both ends, data between), so every data write walked the page's whole
TB list. That was 57 % of QEMU's time in 3DMark 99, invalidating
nothing.

**Same-value stores (patch 18).** 94 % of the era's self-patching stores
write the value already there (a renderer re-patching the same texture
pointer); such a store now invalidates nothing.

**Soft immediates (patch 24).** For the other 6 %, a block discarded by
writes into its own immediates is retranslated with those immediates and
displacements *loaded from the guest's code bytes*, so the guest's store
is the update and the block survives. A block still invalidated four
times goes back to constants. In Blood's starting room the translations
in a 3 s window went from 117,254 to 34.

### 3.4 Control flow (`inline-lookup`, `jump-cache-keep`, `eob-chain`)

**The jump-cache probe inline (patches 20, 38).** Every `ret`, `call *`,
`jmp *` and jump leaving its page called `helper_lookup_tb_ptr` (register
sync, flags recomputed, breakpoint check, jump-cache compare): ~70 host
instructions plus the call, 13.9 % of the vCPU's self time on 7-Zip. The
probe is now TCG ops in the block, its mode-flag half constants since
patch 38.

**The jump cache survives a TLB flush (patch 42).** A CR3 write emptied
the jump cache too, so after each of Windows 98's 2,400 flushes a second
every indirect branch paid the hash table. Entries are now kept (keyed by
physical code address, validated on use) and the cache grown.

**Chaining past non-jump block ends (patch 43).** Every `mov ds/es` in
32-bit code, `sti`, `popf`, `iret` and x87 or MXCSR control-word change
ended the block and left for the main loop: five round trips per VxD
call in Windows 98's ring-0 entry, 1.2 M main-loop entries a second in
3DMark. Only an interrupt pending while IF was clear needs the main
loop; everything else now chains.

### 3.5 Host-side overheads (`tls-hot-paths`, patches 14, 41)

**JIT write-protect toggles (patch 14).** macOS flips the MAP_JIT code
buffer between writable and executable per thread, and QEMU toggled it
before every TB run and around every patch without remembering the
state: 12 % of Super PI's vCPU thread. The state is now tracked per
thread.

**Thread-local storage (patch 19).** On macOS every `__thread` access is
a call into dyld, and the store slow path took five nested RCU read
locks per store into a code page, 8.8 % of Moto Racer's vCPU. The hot
paths read the lock counters once.

**A 13.6 KB memset per translation (patch 41).** QEMU builds with
`-ftrivial-auto-var-init=zero`, and patch 06's slow-path blocks had grown
the translator's context struct to 13.6 KB: 8.7 GB of memset in one
3DMark 99 run. The variable opts out.

### 3.6 Pinned guest registers (`pinned-regs`, patch 21, doc 18; opt-in)

`eip` and the eight general registers live in AArch64's callee-saved
x20–x28 for the life of a chain of TBs: loaded by the prologue, stored by
the epilogue, stored before a helper that may read them and reloaded
after one that may write them, which removes the loads and stores at
block boundaries. It is off by default and not offered in the machine
form (user decision: too unstable for too little gain). XP crashes with
seven or more registers pinned (§5.2, §8.1), and there is a stall at the
flags-helper call boundary; both were open when M21 dropped the patch.

## 4. Methodology

### 4.1 Two tiers of numbers

A benchmark is **reproducible** here when the same program does the same
work every run, a script starts and times it, and its output can be
checked. A game measurement is **ambiguous** in a way no harness removes:
the frame rate depends on where the camera points, on the harness's
clicks landing, on a frame cap, on which five seconds of a run a number
came from. The two kinds are reported separately; a game number carries
its protocol and was checked against a screendump of what was on screen
(§6.1).

### 4.2 The reproducible tier

Four programs whose hot paths are the ones the queue touches, each with a
fixed workload:

| Benchmark | What it exercises | Workload | Number |
|---|---|---|---|
| **nbench 2.2.3** (BYTEmark, `tools/specbench/build-guest.sh`) | ten self-timed kernels: seven integer (sort, bitfield, IDEA, Huffman, assignment, an FP *emulation* in integer code) and three x87 double (Fourier, neural net, LU decomposition) | each kernel runs until its rate is statistically stable | iterations/s per kernel; the headline is the geometric mean of the ten ratios to stock |
| **7-Zip 26.03**, `7zr b 2 -mmt1 -md=22` (public domain, [7-zip.org](https://www.7-zip.org/a/7zr.exe)) | LZMA compress and decompress, one thread, two passes at dictionary 22 (4 MB): integer, `ret`-heavy, memory-bound; the control-flow patches were profiled on it | its built-in benchmark | MIPS ratings (compress, decompress, total) |
| **Super PI mod 1.5 XS**, 1M digits (on the image) | x87 at 53-bit precision, the Windows default; patch 14 was profiled on it | the 1M run, started by keys sent from the host | CPU seconds from the job object that contains the process, so time waiting for keys does not count |
| **SSEBENCH.EXE** (ours, guest-tools ISO, doc 16) | the SSE and x87 kernels every Direct3D-era game runs (vector transform, normalise, dot products, clamps and compares, int↔float conversions), with a checksum per kernel | `-iter 20` | ns per op per kernel; the SSE score is the mean of the SSE kernels |

nbench is cross-compiled with the guest tools' flags (`-O2
-march=pentium3`, msvcrt, `-std=gnu89`); `-march=pentium3` keeps
`double` on the x87, as a 1998–2003 compiler did. The other three are
binaries as shipped.

### 4.3 Running

`tools/specbench/run.sh` boots the project's own Windows XP image (SP3,
512 MB, `-cpu pentium3`, Cirrus VGA, `snapshot=on`) once per emulator
configuration, with the benchmark ISO as D: and a fresh floppy as A:. It
knocks on the Run dialog over QMP until `GO.BAT` has copied the suite to
C: and started `SPECRUN.EXE` (`tools/specbench/specrun.c`), which runs
each command through `cmd.exe /c` in a job object, times it with
`QueryPerformanceCounter`, reads the job's CPU time, CRC-32s its standard
output, and appends one line per run to the floppy and to COM1. Super PI
is a GUI program: the runner announces each start on COM1, the host sends
the keys that pick 1M (`alt+c`, six `down`, `ret`, `ret`), and a batch
file waits for `pi_data.txt`, which Super PI writes when done. The short
programs run twice per boot and nbench once. The tables report the
**best** repetition, because macOS sometimes parks the vCPU thread on an
efficiency core for a run (a uniform ~2x). Under TCG the guest clock
follows host time, so guest milliseconds are wall milliseconds.

**Every emulator launch goes through `tools/specbench/noaslr.c`**, a
20-line launcher that turns address-space layout randomisation off, so
every launch, pristine QEMU's included, gets the same near layout
(§5.0).

Super PI's digits are CRC-checked across configurations; the other three
print their own timings, so their CRCs are recorded, not compared. One
repetition differs: `stock-pic`'s second Super PI run has CRC `550dbed7`
against `fdb79c48` everywhere else (`report.py` flags it). It is pristine
code and was not investigated; the table's 393.0 s is that repetition
(the matching one took 395.8 s).

### 4.4 Configurations

| Name | Binary | Meaning |
|---|---|---|
| `stock` | pristine QEMU v9.2.4, same configure flags, no patches | the baseline the literature compares to |
| `stock-pic` | pristine, built with our `-fPIC` flags | separates the cost of position-independent objects (ours are also linked into the embed library) from the cost of the patches |
| `off` | ours, all fifteen switches off | the unswitched patches plus the switched patches' *off paths* |
| `default` | ours, as the launcher writes a machine | what a 2ksbox user runs |
| `no-<switch>` | default with one switch off | that switch's contribution, as `default / no-<switch>` |
| `pinned` | default + `pinned-regs=on` | the opt-in register pinning |
| `pc64as53` | default + `x87-pc64-as-53=on` | the opt-in inexact mode; expected to change nothing (nothing here runs at 64-bit precision) |

### 4.5 Machine

Apple M1 (4 performance + 4 efficiency cores), 16 GB, macOS 26.6.2, Apple
clang 17 for QEMU, QEMU submodule v9.2.4. One guest at a time and
**nothing else running**: a run taken beside other work was discarded
and redone.

## 5. Results: the reproducible tier

### 5.0 First finding: where the code buffer lands decides a third of launches

The same configuration came out at 0.65x to 0.73x of itself between
launches, while the repetitions inside a launch agreed to 0.5 %. The
machine was idle, not throttled (7-Zip's host-speed calibration read
~3100 MHz in fast and slow runs) and not on an efficiency core. The
regime tracked one variable: **where `mmap(NULL)` put TCG's 1 GiB code
buffer**.

| launch | SSEBENCH SSE score (ns/op) | x87 kernel | code buffer | distance from the text |
|---|---|---|---|---|
| plain 1, 3, and the caught one | 13.00, 13.04, 13.04 | 44.7–45.1 | 0x3_0000_0000 | ~8 GiB |
| plain 2, 4 | 7.82, 7.92 | 33.7 | 0x1_0ca4…, 0x1_45dc… | 0.1–1.1 GiB |
| no ASLR 1–3 | 7.97, 7.80, 7.89 | 32–34 | 0x1_07ba…, 0x1_45b4… | 0.1–1.1 GiB |
| `stock` runs (Super PI, fast) | — | — | 0x1_074f…, 0x1_09e2… | 0.1 GiB |

The AArch64 backend emits a helper call as one `bl` within ±128 MiB of
the target, as `adrp+add+blr` within ±4 GiB, and beyond that as four
`movz/movk` and a `blr`; the same holds for every host address a TB
materialises (the indirect-jump helper, the exit constants). At 8 GiB
every helper call in every TB is the long form. So the slow profile is
the fast one scaled: the extra instructions sit in front of every helper,
and the sampler charges their stall to the helper's first instruction.
Near or far depends on how guest RAM and the libraries have fragmented
the space below the dyld shared cache by the time TCG initialises; with
ASLR off the near gap happens to be free every time on this machine.

The tables pin every launch to slide 0 (`noaslr`). The product, and
upstream QEMU on Apple Silicon, which has the same lottery, need the
buffer reserved next to the helpers at load time, before anything else
fragments the space: a constructor in `tcg/region.c` takes a `PROT_NONE`
`MAP_JIT` reservation within 2 GiB of its own image and
`alloc_code_gen_buffer_anon` uses it. A hint to `mmap` at TCG init is too
late: the near windows are partly occupied by then and macOS returns the
next free gap, 8 GiB away. The A/B against pristine QEMU, six ASLR-on
launches each:

| launch (ASLR on, SSEBENCH only) | pristine 9.2.4: SSE score, buffer | with patch 63: SSE score, buffer |
|---|---|---|
| 1 | 7.73 ns, +0.12 GiB | 7.81 ns, +0.11 GiB |
| 2 | 7.74 ns, +0.12 GiB | 7.69 ns, near |
| 3 | 7.81 ns, +0.12 GiB | 7.74 ns, +0.10 GiB |
| 4 | 7.76 ns, +1.01 GiB | 7.77 ns, +0.12 GiB |
| 5 | **12.99 ns, +8.01 GiB** | 7.73 ns, +1.12 GiB |
| 6 | **12.86 ns, +8.00 GiB** | 7.86 ns, +1.04 GiB |

Pristine went far in two launches of six here and three of five in the
earlier series. With the reservation none did, nor did six more paused
launches whose maps were read directly (1 GiB reserved in five, 512 MiB
in one, always within 1.2 GiB of the image).

Going further than "near" is not worth it. Capping the buffer at 120 MiB
directly above the image puts every block within `bl` reach of every
helper. Four launches each, ASLR on, the buffer's distance read from the
samples (107–127 MiB against 480–820 MiB):

| | SSE score (ns/op), four launches | x87 kernel (ms), four launches |
|---|---|---|
| 1 GiB reservation, `adrp+add+blr` | 8.15, 7.87, 7.75, 7.80 (mean 7.89) | 980, 907, 949, 924 (mean 940) |
| 120 MiB, every call a `bl` | 7.73, 7.74, 7.70, 7.62 (mean 7.70) | 956, 906, 1019, 945 (mean 957) |

About 2 % on the SSE helpers and nothing on the x87 ones, for a buffer an
eighth of the size: the five-instruction far form was the whole cost.

The patch is `patches/qemu/63-jit-buffer-near-helpers`. It runs before
the command line exists, so it has no switch and its A/B is a pristine
build. On a user's machine it removes a 35–45 % loss on a third of
launches rather than speeding up the median one.

### 5.1 The headline

Best of two repetitions per boot (nbench once), every launch near,
Super PI's digits identical in every row but the `stock-pic` repetition
of §4.3. Ratios are against pristine QEMU 9.2.4, higher is better (the
seconds and nanoseconds columns are inverted for the ratio).

| configuration | Super PI 1M (s CPU) | 7-Zip (MIPS) | SSEBENCH (ns/SSE op) | nbench (geomean of 10 kernels) | geomean |
|---|---|---|---|---|---|
| `stock` (pristine 9.2.4) | 393.6 | 1413 | 7.84 | 1.00 | 1.00x |
| `stock-pic` (pristine, our `-fPIC` flags) | 393.0 (1.00x) | 1439 (1.02x) | 7.67 (1.02x) | 1.00 | 1.01x |
| `off` (ours, all switches off) | 423.2 (0.93x) | 1437 (1.02x) | 8.04 (0.98x) | 0.97 | 0.97x |
| **`default` (ours, as shipped)** | **74.8 (5.26x)** | **1573 (1.11x)** | **2.30 (3.41x)** | **1.50** | **2.34x** |

Position-independent code costs nothing (`stock-pic`). The tree with
every switch off is 3 % slower than pristine, the whole cost of the
unswitched patches and the switched ones' off paths, and all that
compiling the switches out would buy back. The default is 2.3x pristine:
5.3x on x87 code, 3.4x on the SSE kernels, 1.5x on nbench's mix, 1.1x on
7-Zip's integer code.

### 5.2 Ablation: the default with one switch removed

Each row is the default minus one switch. Some rows were taken on the
build before patch 63 (the same code without the reservation, every
launch pinned near by `noaslr`), and `default` was rerun on the patch-63
build as the control (74.4 → 74.8 s, 2.36x → 2.34x). Run-to-run noise is
about ±2 %, so a difference under 3 % means nothing.

| switch off | Super PI (s) | 7-Zip (MIPS) | SSEBENCH (ns) | nbench | geomean | what moved |
|---|---|---|---|---|---|---|
| — (default) | 74.8 | 1573 | 2.30 | 1.50 | 2.34x | |
| `x87-fast` | **395.2** | 1587 | 2.42 | **1.17** | 1.44x | Super PI back to pristine (1.00x); nbench's three x87 kernels back to 1.0–1.07x |
| `sse-fast` | 74.7 | 1576 | **7.90** | 1.51 | 1.72x | the SSE kernels back to pristine (xform 1.06x, normalise 0.91x) |
| `simd-fast` | 74.5 | 1594 | 2.49 | 1.52 | 2.31x | MMX blend 4.66x → 2.45x, packed xform 6.2x → 4.6x |
| `rep-fast` | 76.0 | 1573 | 2.43 | **1.30** | 2.22x | nbench string sort 3.9x → 0.9x (its `memmove`) |
| `inline-lookup` | **81.5** | **1495** | 2.35 | 1.49 | 2.25x | Super PI −8 %, 7-Zip −5 % (every `ret` back through the helper) |
| `eob-chain` | **80.2** | 1569 | 2.35 | 1.50 | 2.28x | Super PI −7 % (its `fldcw`s leave for the main loop again) |
| `tb-invalidate-fast` | **80.5** | 1517 | 2.39 | 1.47 | 2.24x | Super PI −7 %, 7-Zip −4 % |
| `tls-hot-paths` | 75.3 | 1556 | 2.38 | 1.51 | 2.31x | within noise |
| `tlb-floor` | 75.1 | 1589 | 2.28 | 1.51 | 2.35x | within noise |
| `tlb-retire` | 74.8 | 1600 | 2.27 | 1.51 | 2.36x | within noise |
| `jump-cache-keep` | 74.5 | 1589 | 2.30 | 1.51 | 2.35x | within noise |
| `smc-same-value` | 74.8 | 1553 | 2.30 | 1.51 | 2.33x | within noise |
| `soft-imm` | 74.4 | 1547 | 2.31 | 1.51 | 2.33x | within noise |
| + `x87-pc64-as-53` (opt-in, inexact) | 74.3 | 1589 | 2.30 | **1.70** | 2.42x | nbench LU 3.0x → 5.3x, neural net 2.7x → 4.9x (§5.4) |
| + `pinned-regs` (opt-in) | — | — | — | — | — | XP crashed and rebooted during Super PI; no numbers (doc 18's open item; `run.sh <image> pinned` reproduces it) |

The six switches that show nothing here were written for games:
`smc-same-value` and `soft-imm` for self-patching renderers, `tlb-floor`,
`tlb-retire` and `jump-cache-keep` for Windows 98's 2,400 CR3 writes a
second under a game, `tls-hot-paths` for the store slow path those
renderers hit. A benchmark program does none of that; their evidence is
§6.

### 5.3 Per benchmark

The full per-kernel tables for every configuration are in
`docs/22-data/report.md`, the raw `RESULT` lines and each program's own
output in `docs/22-data/<configuration>/`. The rows that carry the story:

**nbench** (iterations/s, ratio to pristine):

| kernel | `default` | `no-x87-fast` | `no-rep-fast` | `pc64as53` | what it is |
|---|---|---|---|---|---|
| Numeric sort | 1.03x | 1.05x | 1.03x | 1.01x | integer |
| String sort | **3.93x** | 3.94x | 0.92x | 3.86x | `memmove`: the `rep movs` fast path |
| Bitfield | 1.00x | 1.01x | 1.01x | 0.98x | integer |
| FP emulation | 1.02x | 1.02x | 1.02x | 1.00x | integer (software float) |
| Fourier | **1.44x** | 1.07x | 1.18x | 1.42x | x87, transcendental-heavy (`fsin`/`fcos` stay helpers) |
| Assignment | 1.02x | 1.02x | 1.00x | 1.02x | integer |
| IDEA | 1.02x | 1.02x | 1.02x | 1.02x | integer |
| Huffman | 1.27x | 1.02x | 1.26x | 1.34x | integer with x87 in its inner loop |
| Neural net | **2.69x** | 1.02x | 2.21x | **4.92x** | x87 `exp` and multiply-accumulate |
| LU decomposition | **3.04x** | 0.99x | 2.98x | **5.35x** | x87 `+ - * /` at 64-bit precision |

**7-Zip** (MIPS): pristine 1235 compress / 1590 decompress; default 1458 /
1738 (1.18x / 1.09x). The integer, `ret`-heavy path gains from the three
control-flow patches together: without `inline-lookup` 1322 / 1668,
without `tb-invalidate-fast` 1305 / 1650, without `eob-chain` 1354 / 1716.

**SSEBENCH** (ns per op, ratio to pristine):

| kernel | `default` | `no-sse-fast` | `no-simd-fast` | `no-x87-fast` |
|---|---|---|---|---|
| packed transform (mul/add) | **6.2x** | 1.06x | 4.6x | 5.4x |
| packed normalise (`rsqrtps`) | **5.6x** | 0.91x | 4.4x | 5.3x |
| scalar chain (`ss` ops) | 1.19x | 1.00x | 1.19x | 1.13x |
| clamp + compare (min/max/cmpps) | **4.0x** | 1.01x | 3.9x | 3.8x |
| convert (`cvttss2si`/`cvtsi2ss`) | 2.85x | 0.98x | 2.85x | 2.84x |
| C transform, x87 | **11.9x** | 11.9x | 11.8x | 0.98x |
| C normalise, x87 `fsqrt`/`fdiv` | **11.5x** | 11.5x | 11.5x | 1.00x |
| denormal decay (the slow path) | **0.78x** | 0.92x | 0.78x | 0.80x |
| MMX blend | **4.7x** | 4.7x | 2.45x | 4.7x |

On the **denormal slow path** the default is 22 % *slower* than pristine
(59 vs 46 ns per op): patch 11's inline path detects a denormal result,
undoes its work and calls the helper pristine would have called
directly, on every operation of a kernel that is denormal on every
operation. Real code meets denormals briefly, in decays and fades; a
program that lived there would be better off with `sse-fast=off`. The
scalar chain gains only 1.19x because a dependent chain of
`addss`/`mulss` is latency-bound either way.

### 5.4 Reading the numbers

- **The x87 patches are the largest lever on this tier**: Super PI 5.3x,
  the x87 kernels 11.5–11.9x. nbench turned out to run at 64-bit
  precision (mingw's C runtime sets extended precision at startup; MSVC's
  sets 53 bits), so its three float kernels are the first measurement of
  patches 48/49's exact 64-bit path: LU 3.0x, neural net 2.7x, Fourier
  1.44x. The inexact `x87-pc64-as-53` takes LU and neural net to 5.3x and
  4.9x, the same bargain as 3DMark2001's high-detail physics (§6.2):
  faster, and a different low mantissa.
- **SSE inlining is worth 3.4x on the SSE kernels**: `sse-fast` carries
  the float ops, `simd-fast` the MMX and packed permutes.
- **The control-flow patches are worth 10–20 % on integer code**, the
  shape the M9 profile predicted (the `ret` lookup at 14 % of 7-Zip's
  vCPU, Super PI's x87 control-word block ends).
- **`rep-fast` is one kernel's whole gain**: string sort, the only
  `memmove`.
- **The off paths cost 3 %**, in the x87 and SSE translators' mode checks
  (Super PI 0.93x): small enough that one switchable build is the right
  product.
- **Slower than pristine**: only the denormal slow path (0.78x).

## 6. The games

A Windows 98 game is not a benchmark program. It patches its own code,
spends a quarter of its frame in x87 at 24-bit precision, flips the TLB
2,400 times a second through the VMM, and enters ring 0 five times per
VxD call. That is where the largest gains are.

### 6.1 Protocol

Every game runs on one machine, `base98-us` (Windows 98 SE, our display
driver, `-cpu pentium3`, an SB16), on the tree of §5 with patch 63 in,
each run on a fresh raw copy of the image under a bare
`qemu-system-i386`, one guest at a time, driven headless by a runner per
game (`tools/w98-3dmark.sh`, `w98-3dmark2001.sh`, `w98-blood.sh`,
`w98-moto.sh`, `w98-quake2.sh`; `docs/testing.md`, "Games and
benchmarks", has each one's protocol). The A/B is **every switch off**
against **the default**; pristine QEMU cannot be the baseline, since none
of these runs without the paravirtual adapter.

- **3DMark 99 Max** (800×600×16, triple buffer, Pentium III
  optimizations): the Benchmark clicked, a screendump every 5 s, and the
  executor's 5 s frame-rate windows placed by test from the screendumps.
  A window counts only when it lies wholly inside its test's shots (one
  spanning a loading screen reads as a false rate). The game tests run at
  a 60 Hz flip cap, so they are also taken with the vertical blank off.
- **3DMark2001 SE** (1024×768×32, DXTC, pure hardware T&L): the
  Benchmark, with the per-test frame rates 3DMark reports in its Details
  dialog, paged and screendumped. Every low-detail scene sits at the
  60 Hz cap; the high-detail Car Chase and Lobby are the inexact switch's
  benchmark (§3.1), so the run is taken a third time with
  `x87-pc64-as-53`.
- **Blood** (DOS Build engine in a Win98 DOS box, 640×480 VESA):
  `-quick -map e1m1` puts the first level up with no menu to drive. The
  game flips pages by writing the VBE display start, so QEMU's trace of
  those writes per second is its frame rate. No frame cap; the view is
  the level's first, unmoved.
- **Moto Racer 1997** (640×480): a practice race on Speed Bay with
  Direct3D off in the game's options, so its own software renderer runs
  (where the self-modifying-code patches were found); throttle held from
  the standing start, rate read from the driver's flip chain, vertical
  blank off.
- **Quake II 3.20** (640×480, `ref_soft`): `timedemo 1` on demo1, the
  game's own `frames, seconds: fps` line. A compiled software renderer
  that patches nothing: the control for the claim that the large game
  gains come from the self-modifying-code and VMM patches.

Each number was checked against a screendump taken with its window (the
3DMark tests' in-frame counters and "Now Testing" splash, Blood's and
Moto Racer's frame itself), all under `docs/22-data/games/`.

### 6.2 Results

| Workload | all switches off | default | default + `x87-pc64-as-53` | Note |
|---|---|---|---|---|
| **3DMark 99 Max**, Game 1 (race), vertical blank off | 39.3 fps | **89.2 fps** | — | windows wholly inside the test's shots (`tests.txt`); 3DMark's own counter in the shots: 43–45 / 104 |
| 3DMark 99 Max, Game 2 (first person), vertical blank off | 35.0 fps | **79.6 fps** | — | counter in the shots 26–29 / 67 (instantaneous; the window is a 5 s mean) |
| 3DMark 99 Max, 3DMarks / CPU 3DMarks, vertical blank off | 3901 / 5728 | **8686 / 16609** | — | with the vertical blank on both game tests sit at the 60 Hz cap (59.8 / 59.2 fps) and the score is 5987 / 16614 |
| **3DMark2001 SE**, Game 1 Car Chase, low / high detail | 29.5 / 1.3 fps | 59.0 / **16.9** fps | 59.7 / **24.8** fps | 3DMark's own Details figures; low detail is at the 60 Hz cap |
| 3DMark2001 SE, Game 2 Dragothic, low / high | 53.1 / 28.4 | 60.7 / **52.9** | 60.0 / **56.1** | |
| 3DMark2001 SE, Game 3 Lobby, low / high | 41.5 / 14.9 | 60.1 / **37.3** | 60.0 / **42.9** | high-detail debris physics is x87 at PC=64 (§3.1) |
| 3DMark2001 SE, Game 4 Nature | 51.5 | 64.0 | 60.1 | |
| 3DMark2001 SE, 3DMark score | 3163 | **5222** | 5476 | run to run ±3 % (a first default run scored 5072) |
| **Blood**, the crypt's first view | 4.2 fps | **127.9 fps** | — | VBE display-start writes over 80 s |
| **Moto Racer 1997**, software renderer, the race, vertical blank off | 2.9 fps | **83.5 fps** | — | page flips over 21 s of throttle; the two runs are not at the same point of the lap (the slow one covers 12 s of race in 21 s) |
| Moto Racer 1997, Direct3D (our HAL), vertical blank on | — | 59.7 fps | — | the flip cap, for the record |
| **Quake II** 3.20, software renderer, `timedemo 1` demo1 | 34.8 fps | **50.0 fps** | — | `ref_soft` mode 3; the game's `689 frames, 13.8 seconds: 50.0 fps` line; no self-modifying code, so the CPU tier's kind of gain (1.4x) |

The games gain **2.2x to 30x**: 3DMark 99's game tests 2.3x,
3DMark2001's high-detail scenes 1.9–13x, Blood 30x, Moto Racer's
software renderer 29x. On the default build 3DMark 99's game tests and
every low-detail 3DMark2001 scene sit at the 60 Hz flip cap, which is
what a user sees; the uncapped figures are headroom. **The inexact PC=64
switch** (patch 47) is worth +47 % in the scene that is all debris
physics (Car Chase, high detail), +15 % in the Lobby, +6 % in Dragothic;
the low-detail controls stay at the cap.

### 6.3 What the two tiers say together

The tiers agree on direction, and Quake II, the one game whose renderer
is a program of the benchmarks' kind, gains the benchmarks' 1.4x. They
disagree on scale because three patch groups are invisible to a
fixed-workload benchmark: self-modifying code (§3.3) never happens in a
compiled benchmark; the CR3 storm and the ring-0 round trips (§3.2,
§3.4) belong to Windows 98's VMM under a game; x87 at 24-bit precision
(patch 45) is a Direct3D device's state. The six switches that show
nothing in §5.2 are the difference between 4 fps and 128 in Blood,
between 3 and 84 in Moto Racer. A benchmark number for this tree is a
lower bound on what a user sees. Symmetrically, the literature's SPEC
numbers for hardware-MMU designs (§7) come from workloads that never
touch the paths those designs make slower (page-table churn).

### 6.4 Threats to validity

- **One host.** Everything is one M1 Air on one macOS. The games on the
  project's x86-64 machine are still to be taken.
- **The games' baseline is "every switch off", not pristine QEMU**, which
  runs none of them. The off paths are 3 % slower than pristine (§5.1),
  so the games' ratios overstate the gain over upstream by about that.
- **A game's frame rate depends on the camera.** Each runner fixes what
  it can (3DMark's tests are fixed animations, Blood's is the level's
  first view, Quake II's a demo); Moto Racer's is a throttle held from
  the standing start, and the slow run covers less of the lap.
- **The tables are the near-buffer regime** (§5.0). A QEMU without the
  reservation is there about two launches in three; the rest run at
  0.55–0.9x of these numbers on helper-heavy code, pristine and ours
  alike.
- **The guest compiler is not the era's.** MSVC 6 and Watcom emitted more
  x87, fewer SSE and different call shapes than GCC 16 at
  `-march=pentium3`. The direction of every effect should carry over, the
  magnitudes will not. The games are the era's binaries.
- **Small workloads.** A minute or two per program; only 7-Zip's
  dictionary has a working set beyond the caches.
- **Two repetitions, minimum reported.** The minimum is the right
  estimator for the efficiency-core hazard but hides variance; the raw
  `results.txt` files keep both. The games are one run each (3DMark2001's
  score moves ±3 % between two default runs).
- **Stock QEMU's firmware is unpatched** (ours has the BIOS date stamp
  and the swapped VGA BIOS); neither touches a running XP.
- **The CRC check covers Super PI's digits** and the appendix suite's
  outputs; nbench, 7-Zip and SSEBENCH check themselves (a checksum per
  SSEBENCH kernel; 7-Zip CRCs every block and aborts on a mismatch). The
  games check nothing but the screen.
- **Super PI is timed by CPU time**, not its own stopwatch; the two agree
  to within its startup.

## 7. Related work

The literature was surveyed after the patches were built from profiles
(by Claude, the AI assistant that did the implementation work), so where
a patch and a paper coincide that is convergence, not citation.
`docs/23-dbt-literature.md` has the full list with links and a ranking.

- **Hardware-MMU-backed guest memory.** ESPT (Chang et al., VEE 2014)
  and HSPT (Wang et al., VEE 2015, 1.98x on QEMU) embed a shadow page
  table in the translator's address space; Captive (Spink, Wagstaff,
  Franke; TACO 2016, ATC 2019) runs the translator in ring 0 inside a KVM
  VM with the guest MMU on nested paging (2.5x average, 5.88x peak over
  QEMU on SPEC CPU2006); Rodzevich et al. (ISPRAS 2024) do it for our
  pairing, AMD64 on AArch64; Poletaev and Dovgalyuk (ISP RAS 2025) inside
  QEMU with an mmap view. Our Hypervisor.framework probe measured
  1.4–2.6x per load (`tools/hvf-el1/`); patches 16 and 44 are cheaper
  cuts at the same cost; §8.2 prices the design.
- **Indirect branches.** SPIRE (Jia et al., VEE 2013), MAMBO-X64's
  return-address prediction (D'Antras et al., PLDI 2017) and Tiaozhuan's
  full address mapping (Li et al., TACO 2024; +3.9 % average, 19.4 % peak
  on SPEC with an x86 guest) go further than patch 20; §8.1 tried them.
- **Global register allocation.** Zurstraßen et al. (DATE 2025, up to
  1.4x over block-local) and Batuzov (ISP RAS) in QEMU; patch 21 is the
  same idea.
- **Floating point on the host.** Cota's hardfloat (QEMU, 2018: use the
  host once the inexact flag is sticky) is the rule patches 06 and 11
  extend to x87 and to values kept in host format across instructions;
  Zurstraßen et al. (RAPIDO 2025) vectorise the same checks for RISC-V.
- **Self-modifying code.** Transmeta's translations verify their own
  source bytes on entry (Dehnert et al., CGO 2003); patch 24's blocks
  read the bytes instead.
- **Interrupt checks.** Niu, Zhang and Li (SYSTOR 2022) remove the
  per-block interrupt test; patch 43 removes the block ends that forced
  the main loop.
- **On arXiv.** Learned translation rules in system-mode QEMU (2402.09688,
  1.36x), function offload to native code (2512.00487), IR-less
  translation (2501.03427).

## 8. What the remaining ideas are worth

§7 leaves two kinds of candidate: techniques the queue has not tried, and
the one design it parked for size. Both were priced on the same tier.
Doc 23's last section and the M9 track doc's "Gauging the gain" have the
full account; the diffs, scripts and runs are under
`docs/22-data/spikes/` and `docs/22-data/hwmmu/`.

### 8.1 The techniques, each tried

Each was implemented behind its own `-accel tcg` switch on one build,
exact (every battery of the guest test stage passes; Super PI's digits
identical), and measured against a `default` run of the unchanged binary
taken the same morning. Noise on this tier is ±2 %.

| candidate | Super PI (s) | 7-Zip (MIPS) | SSEBENCH (ns) | verdict |
|---|---|---|---|---|
| control (the tree as committed) | 75.2 | 1553 | 2.37 | |
| jump cache indexed by the pc itself (Tiaozhuan's full address mapping: 32 GiB reserved, populated on demand) | 73.5 | 1588 | 2.30 | within noise |
| the exit-request check at back edges and indirect branches only (Niu et al.) | 74.3 | 1583 | 2.29 | within noise |
| return prediction: a return-address ring, `call` through a host `bl`, `ret` through a host `ret` (MAMBO-X64), on top of both | 75.5 | 1574 | 2.45 | a loss, 2–6 % |
| every SSE result check removed (inexact; the ceiling of any cheaper check, RAPIDO) | | | 2.17 | ≤ 6 % of the SSE score, none of it in the packed kernels |
| pinned registers capped at seven (the DATE 2025 cross-check of patch 21) | XP rebooted in Super PI | | | the crash is the pinned path, not the eighth register |

On an out-of-order host the control-flow patches already took what
mattered: the hash the jump table removes was not on the critical path
of a probe that ends in five dependent loads, and the per-block
interrupt check is an L1 load the core never waits for. Return
prediction loses because the ring push at every call and the compare at
every `ret` cost more than they save; the M1's indirect predictor
already got most `br` targets right. One dependency surfaced: patch 34
delivers the PIT's overdue interrupt on the `in` instruction and relies
on the next block's check to take it, so a block ended by an I/O
instruction keeps the check.

### 8.2 The hardware-MMU design, priced without building it

The design (the M9 track doc) runs TCG's output inside a
Hypervisor.framework VM whose stage-1 page tables mirror the x86 guest's,
so a guest load is one host load instead of the softmmu chain: §7's
Captive / ESPT / HSPT family, 2–2.5x in their SPEC numbers. A probe had
priced its primitives (1.4–2.6x per load), and the workload gain had
been inferred from the profile's 43 % of samples on the chain. Two
measurements replaced the inference:

- **a memory census of each workload** (`tools/hwmmu/census.c`, a TCG
  plugin): instructions, loads, stores, distinct 4 KiB pages touched per
  second and per window of 65,536 accesses (the nested TLB holds 3072),
  each access's page reuse distance, and first touches and first writes
  (a mirror fill and a dirty upgrade each);
- **workload-shaped kernels in the probe**: an independent load with
  four or twelve ALU ops behind it, and a load-plus-store pair, as
  today's softmmu sequence and as the mirrored access, at working sets
  from 64 KiB to 32 MiB.

| workload | acc / insn | stores | pages per 64K-access window, mean / max | windows over 3072 | accesses re-touching a page within 64 accesses |
|---|---|---|---|---|---|
| Super PI 1M | 0.55 | 38 % | 70 / 1,600 | 0 of 449,891 | 97 % |
| 7-Zip | 0.34 | 34 % | 240 / 1,150 | 0 of 286,139 | 97 % |
| nbench | 0.29 | 34 % | 8 / 926 | 0 of 570,340 | |
| Quake II timedemo (Win98, software renderer) | 0.35 | 45 % | 52 / 334 | 0 of 605,108 | |
| Blood (Win98 DOS box) | 0.32 | 16 % | 94 / 367 | 0 of 242,234 | |

With ALU work to hide behind, the chain costs 0.3–0.6 ns per access
rather than the 2–5 ns of a dependent chase, and a store gains more (a
plain `str` against a chain of its own). Charging each workload's access
rate those differences, each access on the kernel row its reuse distance
puts it on:

| workload | M accesses / s | projected gain |
|---|---|---|
| 7-Zip | 483 | 1.20x |
| Super PI 1M | 392 | 1.16x |
| Quake II timedemo | 372 | 1.16x |
| Blood | 257 | 1.10x |
| nbench, SSEBENCH | 146–188 | 1.05–1.07x |

The design's one risk, random working sets beyond the nested TLB's
12 MiB (~21 ns per miss in the probe), does not occur: over 2.3 million
windows on six workloads, none touched more than 1,600 distinct pages.
The projection leaves out what else the mirror would change (TLB refills
at 40 k/s and flushes at 500/s, XP's 3.9 % idle in CR3 switches, a
Windows 98 game's 2,400 CR3 writes a second, whose largest costs patches
42 and 44 already took) and cannot separate dependent-address accesses,
so 7-Zip's number is a floor. The literature's 2–2.5x does not transfer
because these programs re-touch their pages within a few dozen accesses:
the chain they pay is the throughput of nine overlapped instructions,
not the latency of a dependent walk.

**Decision.** A fifth is not worth a freestanding build of the vCPU
core, a fault-driven rewrite of `cputlb.c` and a mailbox protocol between
the VM and the device model. Abandoned for now; the census, kernels and
projection stay in the tree.

## 9. Conclusion

The floating-point patches carry the bulk of the 2.3x on benchmarks; the
switches that show nothing there decide the games (2.2x to 30x). For an upstream
reviewer the most useful result is patch 63: a third of macOS/arm64
launches, pristine or patched, lose 35–45 % to a far code buffer. What is
left to gain generically on these workloads is a fifth at most (§8), and
the decision was to stop here.

Open, in order: the games of §6 on the project's x86-64 machine; the
x86-64 form of the placement question there, where the far form is the
always case; patch 21's crash, reproducible by `tools/specbench/run.sh
<image> pinned`, set aside; the denormal slow path (§5.3); and the "all
off plus one switch" family for a reader lifting a single patch.

## Appendix A. The SPEC-CPU2006-derived suite (measured once, not the headline)

SPEC CPU2006 is what the literature reports, it is licensed, and SPEC
retired it in 2018, so `SPEC=1 tools/specbench/build-guest.sh` builds
what it was made of: the open-source ancestors of five CINT2006
benchmarks (bzip2 1.0.8 ← 401.bzip2, GNU Go 3.8 ← 445.gobmk, HMMER 2.3.2
← 456.hmmer, Sjeng Free 11.2 ← 458.sjeng, libquantum 1.1.1 ←
462.libquantum), cross-compiled with the same flags, with fixed inputs and
SPEC's own small fixes (a pinned seed, no learning file, no stdin
polling), every output CRC-checked. On the Air, seconds, best of two:

| config | bzip2 -9 | bzip2 -d ×3 | shor | hmmer | gnugo | sjeng | geomean vs stock |
|---|---|---|---|---|---|---|---|
| stock 9.2.4 | 10.2 | 6.1 | 105.8 | 27.7 | 17.2 | 15.5 | 1.00x |
| ours, all off | 10.6 | 8.3 | 110.6 | 29.8 | 23.5 | 17.2 | 0.88x |
| ours, default | 9.9 | 5.5 | 103.0 | 27.3 | 13.2 | 14.9 | **1.07x** |
| default, `x87-fast` off | 9.7 | 5.2 | 106.5 | 27.5 | 21.5 | 14.6 | 1.00x |
| default, `sse-fast` off | 9.7 | 5.9 | 102.9 | 27.4 | 13.2 | 14.7 | 1.06x |

Every configuration produced identical output. Compiled integer code
gains a uniform few percent (the TLB floor, the inline lookup, the
chained block ends), GNU Go 1.30x because its influence functions use
floating point, and the rest of the queue never runs. The suite answers
what an upstream reviewer would see on their own benchmark, and its
`off` row was the first evidence that the switched patches' off paths are
slower than upstream's code (0.88x; GNU Go 0.73x, the x87 path at
`x87-fast=off` slower than stock's).

## Appendix B. Reproducing

```sh
scripts/build.sh                                   # our QEMU (build/qemu)
# pristine baseline: a worktree of the submodule at v9.2.4, same configure flags
git -C qemu worktree add --detach build/qemu-stock-src v9.2.4
(cd build/qemu-stock && ../qemu-stock-src/configure --target-list=i386-softmmu \
   --disable-werror <the --disable flags of scripts/configure-qemu.sh> && ninja qemu-system-i386)
tools/specbench/build-guest.sh                     # the suite → build/specbench/sb.iso (SPEC=1 adds appendix A's)
tools/specbench/run.sh ~/vms/winxp.qcow2 all       # ~2 h; build/specbench/runs/
tools/specbench/report.py build/specbench/runs --md
# the games (base98-us; each runner's env in docs/testing.md): the default,
# then the same with every switch off, then the inexact switch where it applies
FRESH=1 TABLET=0 IMG=<base98-us disk> TDM_DIR='\PROGRA~1\3DMARK~1' DDFLAGS=32768 tools/w98-3dmark.sh 99-def-nv
FRESH=1 tools/w98-3dmark2001.sh 2001-def; FRESH=1 CPU=pentium3,x87-pc64-as-53=on tools/w98-3dmark2001.sh 2001-pc64
tools/w98-blood.sh blood-def; SOFT=1 DDFLAGS=32768 tools/w98-moto.sh moto-soft-nv; tools/w98-quake2.sh q2-def
# every switch off: CPU="pentium3,x87-fast=off,sse-fast=off,simd-fast=off,rep-fast=off"
#   QEMU_TCG_OPTS="smc-same-value=off,soft-imm=off,inline-lookup=off,tb-invalidate-fast=off,tlb-floor=off,tls-hot-paths=off,jump-cache-keep=off,eob-chain=off,tlb-retire=off"
```

## Appendix C. Raw result lines

The raw lines of every configuration (`RESULT` lines with wall and CPU
milliseconds, CRC and exit code per repetition, and each program's own
output) are under `docs/22-data/<configuration>/`, the report as
`docs/22-data/report.md`, the A/B of patch 63 under
`docs/22-data/runs-ab3/`, the 120 MiB experiment under
`docs/22-data/runs-exp120/`. The `pinned` directory has the screendump of
the rebooting guest. The games are under `docs/22-data/games/<run>/`: the
rate lines, the per-test placement (`tests.txt`), 3DMark's score and
Details screendumps, Blood's per-second counts, the game frames the
numbers were checked against, and Quake II's console log. §8's material
is under `docs/22-data/spikes/` (the spike diff, the scripts that produce
it, every run's result lines, the rebooted `pinned-7` guest) and
`docs/22-data/hwmmu/` (the census of every workload, the reuse-distance
census, the probe's clean run). The profiler samples (one per
configuration, inside Super PI) are not committed: they are in
`build/specbench/samples/` on the Air.
