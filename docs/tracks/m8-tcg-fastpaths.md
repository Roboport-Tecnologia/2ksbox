# Track M8: CPU fast paths in TCG (docs 13 and 16)

This track made emulated floating-point code fast under TCG: the x87 fast
path and shadow-double translator (patches 05/06, doc 13), SSE float inline
(patch 11, doc 16), MMX/SSE integer and permutes inline (patch 12, doc 16),
and the TCG float and vector opcodes they add to both backends. It was
merged to `main` on 2026-09-04, bit-exact on aarch64 (the Air) and x86-64.
M21 ported the four patches to QEMU 11.1 (2026-10-01), the backend half
rewritten for its `TCGOutOp` backends, all batteries still bit-exact.
This doc keeps scope, test loop and open items; designs and measurements
are in docs 13 and 16, each patch's row in `patches/qemu/README.md`. Doc 13
also covers the later x87 patches from other work: 37 (PE sticky), 45
(binary32 at PC=24) and 47–49 (PC=64).

## Scope and files

- QEMU patches `05-x87-fast`, `06-x87-inline-tcg`, `11-sse-inline-tcg` and
  `12-simd-inline-tcg`. They touch:
  - `target/i386/tcg/`: `x87-fast.h`, `x87-shadow.c.inc`,
    `sse-fast.c.inc`, `sse-fast-lane.c.inc`, `simd-fast.c.inc`;
  - the new opcodes: `include/tcg/tcg-opc.h`, `tcg/tcg.c`,
    `tcg/tcg-op*.c`, `tcg/aarch64/`, `tcg/x86_64/` (`tcg/i386/` before
    QEMU 10);
  - hooks in `translate.c`, `emit.c.inc`, `decode-new.c.inc`,
    `tcg-cpu.c`, `fpu_helper.c`, `cpu.h`, `cpu.c`.

  SSE and SIMD were 07/08 on the track branch; old commit messages use
  those numbers.
- CPU properties `x87-fast`, `sse-fast` and `simd-fast` (all on), each with
  a switch in the launcher's optimizations list.
- Tests: `tools/x87-fast-test.c` (`x87-fast` check),
  `tools/x87-guest-test.py` and `tools/sse-guest-test.py` (`x87-guest`,
  `sse-guest`), `tools/x87-unwind-test.asm`.
- Benchmarks: `guest-tools/src/ssebench.c` (`SSEBENCH.EXE` on the ISO);
  `tools/xp-ssebench.sh`, which runs it in XP per `-cpu` config and works
  on macOS; the x87/SSE sections of `reference/benchmarks/README.md` with
  the rig row.

## Test loop

```sh
scripts/build.sh                    # re-applies the queue after a patch edit
python3 tools/x87-guest-test.py     # the slow blocks are shared by x87 and SSE:
python3 tools/sse-guest-test.py     #   run both after any change to either
scripts/test.sh all                 # before every commit
tools/xp-ssebench.sh ~/vms/winxp.qcow2   # default, sse-fast=off, both off
```

- **Pass criterion.** Each battery's fast-path-on and fast-path-off outputs
  must be identical (last counts: 382,251 lines x87, 546,425 SSE). Tool
  detail is in `docs/testing.md`.
- **Slow-path counters.** `info registers` prints `SSE-fast slow paths:
  guard= handover= helper= cvt/comis=`. Counters growing by millions mean
  the workload runs on the slow path.
- **Editing a patch.** Follow the recipe in `patches/qemu/README.md`.
  Patch 12 edits the same TCG files after 11, so changing 11 means
  regenerating 12.

## What stayed open

- **A real-workload number.** A Direct3D title in XP with and without
  `x87-fast=off,sse-fast=off`, the first end-to-end number for 06, 11 and
  12 together. M4 lists the same item.
- **The rest of clamp+cmp** (43 % of the rig on x86-64, 34 % on the Air).
  First inline `movmskps`/`movmskpd`, still on the helper; then cut the
  per-operand TLB cost of memory operands.
- **Cheaper packed checks.** Hoist the vector constants, which each packed
  op re-materializes at two instructions each on aarch64. A vector-to-scalar
  move opcode would remove the `env->sses_scratch` round trip from the
  scalar path.
- **Still on helpers:** `cvtps2dq`/`cvttps2dq`/`cvtdq2ps`,
  `cvtpi2ps`/`cvtps2pi`, `pmuludq`, and SSE3 `haddps`/`addsubps`, SSSE3
  and VEX forms, which era code does not use.
- **The x87 C transform** (16–18 % of the rig), limited by the shadow
  translator's throughput on cheap ops. Improving it is a doc 13 redesign,
  not started.
