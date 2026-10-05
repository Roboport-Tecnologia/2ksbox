# 23. The parked optimizations against the literature

What the dynamic-binary-translation literature has on the TCG work this
project parked or could not do, and, in the last section, each ranked
candidate built as a spike and measured on doc 22's tier. The next
session on TCG performance starts here instead of searching again; doc
22 §7 is the short form. The queue itself is in `patches/qemu/README.md`,
docs 13, 16 and 18 and the M8/M9 track docs.

The survey came after the patches, which Claude, the AI assistant that
did the implementation work, built from profiles of the workloads. Where
a patch matches a paper, the two arrived there independently.

arXiv carries almost nothing on the subject: a full-text search for
"dynamic binary translation" returns nine papers, three of them relevant.
The field publishes at VEE, CGO, TACO, ASPLOS, PLDI, DATE and ISP RAS, so
most entries are venue papers; the arXiv ones have their own section.

## What we parked, and who has worked on it

### 1. The softmmu TLB chain

The four-load TLB lookup is ~43 % of generated-code samples on integer
code (7-Zip). The M9 track designed and probed the structural fix: run
TCG's output inside a Hypervisor.framework VM at EL1 with the guest's x86
page tables mirrored into stage 1, so a guest load is a host load. The
probe measured every primitive on the Air (1.4–2.6x per load; helpers
must run inside the VM; the one risk is the nested-TLB penalty on working
sets beyond ~12 MiB) and priced it at "4–8 weeks".
The literature has built that design four times:

- **Captive.** Spink, Wagstaff, Franke, *Hardware-Accelerated
  Cross-Architecture Full-System Virtualization*, [TACO 2016](https://dl.acm.org/doi/10.1145/2996798);
  the retargetable version *A Retargetable System-Level DBT Hypervisor*,
  [USENIX ATC 2019](https://www.usenix.org/system/files/atc19-spink.pdf).
  The DBT runs in ring 0 inside a KVM VM, the guest MMU is mapped onto
  nested paging, device I/O and interrupts cross the VM boundary. ARM
  guest on x86 host: 2.5x average, 5.88x peak over QEMU on SPEC CPU2006.
  The closest relative of our design, including the device/interrupt
  half our probe left as a mailbox protocol.
- **Rodzevich, Batuzov, Koltunov, Cheremnov, Shlyapin**, *Efficient MMU
  Emulation in Case of Cross-ISA Dynamic Binary Translation*,
  [ISPRAS 2024](https://ieeexplore.ieee.org/document/10899135/): an
  AMD64 guest on an AArch64 host with shadow page tables built for the
  Arm MMU. The only paper with our ISA pairing.
- **Poletaev, Dovgalyuk, Teys, Kostin**, *Hardware acceleration of Qemu
  MMU for aarch64 on x86-64 full system emulation*,
  [Proc. ISP RAS 37:6 (2025)](https://www.mathnet.ru/php/archive.phtml?wshow=paper&jrnid=tisp&paperid=1089&option_lang=eng):
  inside QEMU, an extra mmap view of the guest virtual address space so
  translated loads use a fixed offset instead of the TLB lookup; the
  abstract's speedup figure is truncated at "271" (read: up to ~2.7x).
- **ESPT.** Chang, Wu et al., *Efficient Memory Virtualization for
  Cross-ISA System Mode Emulation*, [VEE 2014](https://dl.acm.org/doi/10.1145/2576195.2576201)
  (a kernel module). **HSPT.** Wang et al., *Practical Implementation
  and Efficient Management of Embedded Shadow Page Tables for Cross-ISA
  System Virtual Machines*, [VEE 2015](https://www.semanticscholar.org/paper/HSPT:-Practical-Implementation-and-Efficient-of-for-Wang-Li/fa8e9614381ed39e6b978c691aa81e5365984a20)
  (mmap only, 1.98x average on QEMU, ARM on x86-64).

Their numbers agree with our probe's per-load gain. The user-space mmap
variants are closed to us on the Air: macOS maps at 16 KiB and the
guest's pages are 4 KiB; only the VM has the 4 KiB granule (the probe
measured `TGran4=0`). The per-load gain does not become a workload gain
here: a memory census of each workload times the probe's workload-shaped kernels
projects 1.1–1.2x (7-Zip 1.2x, Super PI and Quake II 1.16x, Blood 1.1x,
the FP kernels 1.05x), and the nested-TLB risk occurs on none of
them (doc 22 §8.2; the M9 track's "Gauging the gain"). **Abandoned for
now** (user decision): not worth the port's complexity. Read Captive and
the 2024 ISPRAS paper before reopening it.

The cheaper cuts we took: patch 16 (TLB floor) and patch 44 (retire
instead of flush). Also relevant: Tong, Koju, Kawahito,
Moshovos, *Optimizing Memory Translation Emulation in Full System
Emulators*, [TACO 2015](https://dl.acm.org/doi/10.1145/2686034): SoftTLB
resizing, a victim TLB (which QEMU adopted), helper threads to flush.

### 2. Indirect branches past patch 20

Patch 20 inlines QEMU's jump-cache probe; what remains is a hash, a
compare and conflict misses (11.7 M hash-table lookups per 10 s in
3DMark 99 with the stock 4096 entries, 2.5 M after patch 42 grew the
cache to 65,536).

- **Tiaozhuan.** Li, Guo, Lan, Xue, Han, Niu, Zhang, *A General and
  Efficient Indirect Branch Optimization for Binary Translation*,
  [TACO 2024](https://dl.acm.org/doi/10.1145/3703355): a table indexed
  directly by guest PC in a large reserved address space ("full address
  mapping"), so a lookup is 1–2 host instructions, plus
  "exception-assisted branch elimination" for the correctness check.
  In LATX (x86 guest on LoongArch): +3.9 % average, +19.4 % peak on SPEC
  CPU2006. For a 32-bit guest the table is 32 GiB of lazily populated
  virtual address space.
- **MAMBO-X64.** D'Antras, Gorgovan, Garside, Luján, *Low Overhead
  Dynamic Binary Translation on ARM*, [PLDI 2017](https://research.manchester.ac.uk/en/publications/low-overhead-dynamic-binary-translation-on-arm/),
  and *Optimizing Indirect Branches in Dynamic Binary Translators*,
  [TACO 2016](https://dl.acm.org/doi/10.1145/2866573): translated
  call/ret pairs that push host addresses so the hardware return-address
  predictor works; `ret` is most of our indirect branches.
- **SPIRE.** Jia et al., *SPC-Indexed Indirect Branch Redirecting*,
  [VEE 2013](https://dl.acm.org/doi/10.1145/2517326.2451516): trampolines
  in the source code space.

Generic, exact, and on top of patch 20: the cheapest candidate.

### 3. Pinned guest registers (patch 21, parked; dropped in M21)

- **Zurstraßen, Bosbach, Reimann, Leupers**, *Static Global Register
  Allocation for Dynamic Binary Translators*,
  [DATE 2025](https://ieeexplore.ieee.org/document/10993060/): static
  guest-to-host register mappings, up to 1.4x over block-local allocation
  (RISC-V on ARM64). Doc 18's idea, with a gain the same size as
  ours (7-Zip decompress +16 %).
- **Batuzov**, *Global register allocation during dynamic binary
  translation*, [Proc. ISP RAS](https://ispranproceedings.elpub.ru/jour/article/view/178?locale=en_US):
  per-block pre/post conditions in QEMU, 29.6 % on a synthetic example.
- *Low-Compilation-Cost Register Allocation in LLVM-Based Binary
  Translation*, [EuroSys 2026](https://dl.acm.org/doi/abs/10.1145/3767295.3803591).

### 4. Cheap IEEE flag checks (the deferred `fp-relaxed` mode)

- **Cota**, hardfloat, [QEMU 2018](https://patchew.org/QEMU/20181124235553.17371-1-cota@braap.org/):
  use the host FPU when the inexact flag is already sticky. Patches 06
  and 11 apply and extend that rule.
- **Zurstraßen, Bosbach, Jünger, Leupers**, *Rapid RISC-V Floating Point
  Vector Simulation*, [RAPIDO 2025](https://dl.acm.org/doi/10.1145/3721848.3721853):
  the same corner cases as patch 11's per-op checks; add and sub cannot
  underflow, so that check is dead; the checks vectorised. May trim doc
  16's "~25 instructions per packed check" without giving up exactness.

### 5. x87 at 64-bit precision, exact

Patches 48/49 keep the x80 as the shadow and do `*` and `fst m32`
inline; add, sub and div are still 128-bit integer helpers.

- **Zhang, Aiken**, *High-Performance Branch-Free Algorithms for
  Extended-Precision Floating-Point Arithmetic*,
  [SC '25](https://dl.acm.org/doi/10.1145/3712285.3759876): branch-free
  double-word add, sub, mul, div and sqrt on floating-point expansions,
  with machine-verified error bounds, 11.7x over QD. An x80 mantissa
  fits a pair of doubles exactly, so this is a possible FPU-side route
  for the remaining helpers; the final rounding to 64 bits still needs
  its own argument. Speculative.

### 6. Self-modifying code residue

Patch 24 refuses stores on the second page of a page-straddling block,
and the M9 track's open items want two-page blocks linkable.

- **Dehnert et al.**, *The Transmeta Code Morphing Software*,
  [CGO 2003](http://www.xsim.com/papers/transmeta-code-morphong-software.dehnert-cgo03.pdf):
  self-checking translations that verify their own source bytes on entry
  instead of being invalidated on write, plus fine-grain write protection
  of mixed code/data pages. The exact technique.

### 7. Interrupt checks per block

- **Niu, Zhang, Li**, *Eliminate the overhead of interrupt checking in
  full-system dynamic binary translator*, [SYSTOR 2022](https://dl.acm.org/doi/10.1145/3534056.3534939).
  Patch 43 took the larger part of that cost (the block ends that forced
  the main loop).

### 8. Memory barriers on the Arm host

- **Risotto.** Gouicem et al., *A Dynamic Binary Translator for Weak
  Memory Model Architectures*, [ASPLOS 2023](https://dl.acm.org/doi/10.1145/3567955.3567962):
  verified fence placement for x86 on Arm, +6.7 %. On the M1 doc 16's
  memory-operand bench saw no cost from the barriers and 7-Zip 2–10 %
  (single runs); `-smp 1,maxcpus=1` drops them, and a new default needs
  an audit (M9 track, "Open").

## The arXiv-native papers

- [2402.09688](https://arxiv.org/abs/2402.09688), *A System-Level
  Dynamic Binary Translator using Automatically-Learned Translation
  Rules* (2024): learned rules inside system-mode QEMU, 1.36x over QEMU
  6.1 on SPEC CINT2006. The only arXiv paper about speeding up
  full-system QEMU. Rules learned from a modern compiler's patterns may
  not fit MSVC-6-era code.
- [2512.00487](https://arxiv.org/abs/2512.00487), *Partial
  Cross-Compilation and Mixed Execution for Accelerating DBT* (2025):
  offloading whole functions to native host code, up to 13x. User-mode,
  and what our guest DLLs already do for Direct3D and Glide.
- [2501.03427](https://arxiv.org/abs/2501.03427), *Boosting
  Cross-Architectural Emulation Performance by Foregoing the
  Intermediate Representation Model* (2025): a 35x claim from a proof of
  concept. Not credible for full-system work.
- [2605.08419](https://arxiv.org/abs/2605.08419), Elevator: static
  x86-64 → AArch64 translation (2026). Wrong shape for us.
- [2009.03846](https://arxiv.org/abs/2009.03846), x86 ↔ ARM
  concurrency mappings (2020); [1905.06825](https://arxiv.org/abs/1905.06825),
  TLB simulation on QEMU (2019).

Nothing, on arXiv or elsewhere, addresses the Voodoo 2's per-dword MMIO
trap; doc 21 §9's RAM-backed FIFO window is our answer to it.

## Ranking, by fit and evidence

1. The hardware-MMU papers (they de-risk the parked design and share our
   ISA pairing).
2. Tiaozhuan plus the return-address trick (cheap, exact, on top of
   patch 20).
3. The DATE 2025 register paper as a cross-check for patch 21's open
   items.
4. The RAPIDO 2025 flag-check paper for doc 16's follow-ups.
5. SC '25 double-word arithmetic for x87 PC=64 (speculative).

## Spikes: each candidate tried, one at a time

Each ranked item was built as a spike (user request) and measured on doc
22's tier (`tools/specbench/run.sh`, the XP image, every launch through
`noaslr`): one build with every spike behind its own `-accel tcg`
switch, a fresh `default` run of the unchanged binary the same morning
as the control, and the guest stage of `scripts/test.sh` on every build
(the DOS x87 / rep / SMC / SSE batteries, the PIT clock, MIDI, ATAPI,
the pad; not the Voodoo checks). Super PI's digits are identical in
every row. The diff, the scripts that produce it and every run's result
lines are under `docs/22-data/spikes/`; none of it is in the patch
queue. Noise on this tier is about ±2 % (doc 22 §5.2) and the control
came out 1–2 % under doc 22's row, so a difference under 3 % is nothing.

| spike | switch | Super PI 1M (s) | 7-Zip (MIPS) | SSEBENCH (ns) | nbench | verdict |
|---|---|---|---|---|---|---|
| control, the tree as committed | — | 75.2 | 1553 | 2.37 | 1.00 | doc 22's `default`, remeasured |
| **A** Tiaozhuan's full address mapping: the jump cache as a table indexed by the pc itself | `jump-table` | 73.5 | 1588 | 2.30 | 1.01 | ≤ 2 %, within noise |
| **B** SYSTOR 2022, the exit-request check at back edges and indirect branches only | `irq-check-backedge` | 74.3 | 1583 | 2.29 | 1.01 | ≤ 2 %, within noise |
| A + B | both | 73.9 | 1583 | 2.30 | 1.01 | as above |
| **D** MAMBO-X64's return prediction: a return-address ring, `call` through a host `bl`, `ret` through a host `ret` | `ras`, on top of A + B | 75.5 | 1574 | 2.45 | 1.01 | a loss: −2 % Super PI, −6 % SSEBENCH |
| **C** RAPIDO 2025, the ceiling: every SSE result check removed (inexact, `SSES_NOCHECK=1`) | — | — | — | 2.17 | — | the checks cost ≤ 6 % of the SSE score (convert 16 %, scalar chain 8 %, packed ops 0–1 %); nothing exact can take more |
| **E** DATE 2025 / patch 21, pinned registers, capped at seven (`QEMU_TCG_PIN_MAX=7`) | `pinned-regs` | XP rebooted during Super PI | | | | the crash is not the eighth register: it reproduces at seven (`pinned-7/reboot.png`) |

- **A, the pc-indexed jump table** (§2, Tiaozhuan). 32 GiB of address
  space reserved with `MAP_NORESERVE`, an entry per 32-bit pc holding the
  TB pointer in its low 48 bits and patch 42's generation above them
  (16 bits; the wrap re-maps the table), materialised a host page at a
  time. A single-page TLB flush re-maps the page's 32 KiB rather than
  writing it, so an `invlpg` of a data page does not touch it; a TB flush
  re-maps the whole table. The probe's address chain goes from eight
  dependent operations (the hash) to three, with no conflict misses, and
  the compare (cs_base / flags / cflags / generation) is unchanged, so it
  is exact. The tier does not see it: 7-Zip's indirect-branch working set
  already fit the 65,536-entry cache after patch 42, and the hash was not
  on the critical path of a chain that ends in five dependent loads. Its
  case, if any, is a Windows 98 game with thousands of virtual-call
  targets, which doc 22 §6 would have to measure.
- **B, the check at back edges** (§7). A TB not under icount and not in
  an interrupt shadow skips the `icount_decr` test at its start; the
  target emits it before every backward direct jump and every indirect
  branch (every cycle in the block graph contains one), and before the
  jump that ends a TB an I/O instruction ended: patch 34 delivers the
  PIT's overdue edge on the `in` itself and counts on the next block
  start to take it (the PIT check failed on the first build without
  this). Exact; interrupt latency in straight-line code goes from "next
  block" to "next back edge or return". It saves two host instructions
  per block, an L1 load never on an out-of-order core's critical path:
  nothing to measure. The paper's gains were on an in-order LoongArch.
- **D, return prediction** (§2, MAMBO-X64). Two TCG ops: `call_tb` (a
  goto_tb reached through `bl` to a three-instruction stub in the
  caller's TB, which stores the host return address into a ring entry
  beside the guest return address and the calling TB) and `goto_ret` (a
  goto_ptr emitted as `ret`). A 64-entry ring lives in the jump cache;
  the `call` side is in `gen_jmp_rel` (32-bit code, same-page
  continuation, never for `call $+5`); the `ret` side runs before the
  ordinary probe: pop, compare the popped address with the target and
  the calling TB's cs_base / flags / cflags with the current ones (the
  landing is a `goto_tb` to the continuation and must be exact), `ret` on
  a hit, fall through to the probe on a miss. Exact, and slower: the ring
  push at every call (eight instructions, two stores) plus the branch at
  every `ret` cost more than the host `ret`'s prediction saves, so the
  M1's indirect predictor was already right about most `br` targets. The
  ring's hit rate is unmeasured; a counter would decide whether a second
  iteration (the ring in the CPU's negative-offset state, no push for
  leaf calls) is worth having. Parked with that note.
- **C, cheaper IEEE checks** (§4). The paper's point that add and sub
  cannot underflow was already true of patch 11, whose add and sub check
  only "exponent field not all ones", two operations. Removing every
  check (inexact, an experiment knob only) bounds what any trimming could
  buy: 6 % of the SSE score, all in the scalar and convert kernels, none
  in the packed ones games run. Closed.
- **E, pinned registers** (§3). The DATE 2025 paper confirms patch 21's
  gain (+16 % on 7-Zip decompress); the crash had been blamed on the
  eighth pinned register. It is not: capped at seven, XP rebooted in
  Super PI's first repetition as in doc 22's nine-register run. The bug
  is in the pinned path; the next step was to catch the reboot (`-d int`)
  and bisect over the allocator changes, until M21 dropped patch 21
  rather than port it to QEMU 11.1.
- **Not spiked.** §1, the hardware MMU: weeks by the probe's own
  estimate, and its user-space variants are closed on macOS (16 KiB
  pages). §5, SC '25 double-word arithmetic for x87 at 64 bits: its
  ceiling is already measured (`x87-pc64-as-53` is the same kernels with
  the 64-bit rounding removed: LU 3.0x → 5.3x, neural net 2.7x → 4.9x,
  doc 22 §5.3), and rounding a 106-bit double-double to a 64-bit
  mantissa exactly on halfway cases is the whole work. §6, Transmeta's
  self-checking translations: only a Windows 98 renderer exercises it,
  and the games tier is a day of runs. §8: already measured, and a change
  of default rather than a technique.

**Conclusion.** On this tier the literature's remaining generic ideas buy
nothing measurable: the control-flow patches already took the part of
each that mattered on an out-of-order host, and the remaining large
lever, the softmmu TLB chain (§1), costs weeks. Two cheap measurements
could change that: a Windows 98 title's virtual-call working set (for A)
and the ring's hit rate (for D).
