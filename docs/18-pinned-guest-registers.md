# 18. Pinned guest registers: the x86 register file in host registers across chained TBs

The design of patch `21-pinned-regs`, which keeps the eight 32-bit GPRs
and `eip` in aarch64 callee-saved registers for the life of a chain of
TBs. **It is parked.** It is off by default and the machine form does
not offer it (user decision: too unstable for too little gain), so a
bundle that still says it is on never reaches the command line. **The
patch was not ported to QEMU 11.1 and is gone from the queue** (track
M21, user decision 2026-10-01: 10.1's TCG backend rewrite made the port
a rewrite), so this doc is the design on 9.2.4 and the tools below ran
on that tree. Profiles, results and the open crash are in
the M9 track (`docs/tracks/m9-tcg-aarch64.md`, "Open"); doc 22 §3.6 and
§8.1 place it among the other patches.

## Why

TCG keeps every global (the eight GPRs, `eip`, the flag words, the
segment bases) in memory at every basic-block boundary and before every
op that may fault, and reloads it on first use after. A `ret` TB loads
`esp` and `eip`, adds, stores both; a loop with a memory access stores
its modified registers before each `qemu_ld`/`qemu_st` and reloads them
after each conditional branch. x86 hosts have no registers to spare for
more; aarch64 has nine free callee-saved registers (x20–x28, x19 being
`env`), and QEMU's allocator already prefers them.

## The contract

A **pinned global** is a `TEMP_GLOBAL` with a fixed host register R(g)
chosen once at init. Invariant P, *the register is authoritative*: in
generated code R(g) holds g's current value everywhere except between
the store before a helper call and the reload after it. The `env` slot
may be stale. `mem_coherent` tracks within a TB whether the slot is
current (conservatively 0 at TB entry and at every label, since a branch
may have skipped the store), so a global stored for one helper call and
not modified since is not stored again for the next.

| boundary | what happens |
|---|---|
| prologue (`tcg_qemu_tb_exec` entry) | after `x19 = env`: `ldr` every pinned global from its `env` slot |
| epilogue (`tb_ret_addr`, reached by `exit_tb` and by `goto_ptr` to the epilogue) | `str` every pinned global to `env`, before the callee-saved registers are restored |
| `goto_tb`, `goto_ptr` to another TB | nothing: the values stay in their registers, which is the point |
| helper call that may read globals (no `TCG_CALL_NO_RWG`) | `str` the pinned globals whose slot is not known coherent |
| helper call that may write globals (no `TCG_CALL_NO_WG`) | `ldr` every pinned global after the call (the helper may have written `env->regs[]` or `env->eip` in C: `div`, `iret`, `sysenter`, …), then assign the call's own outputs |
| `qemu_ld` / `qemu_st` slow path (TLB miss, MMIO, watchpoint, a store into a page holding code) | the stub stores every pinned global before calling the memory helper, through one `bl` to a shared thunk after the epilogue (inline stores grew the code buffer 25 %); nothing is reloaded (the helper returns with the registers intact, or never returns because of a fault or an invalidation of the running TB, and the main loop finds `env` current) |
| longjmp out of a helper (`cpu_loop_exit`) | covered by the two helper rows: by TCG's existing contract a helper that can raise is not `NO_RWG`, so the store preceded it |

So the `env` slot is current whenever C code can look at it, and
generated code never reads it between TBs. The main loop, interrupts,
exceptions, `cpu_restore_state`, the monitor and gdb are unchanged: they
all run between `tcg_qemu_tb_exec` calls or after a longjmp, which is
after a store.

Exceptions stay precise. The i386 translator orders each instruction's
memory accesses before its register writes so that the globals synced
before a `qemu_ld`/`qemu_st` are the pre-instruction values; the
slow-path stub stores the same values.

## The register allocator

`tcg/tcg.c`, guided by P. A pinned temp always has `val_type ==
TEMP_VAL_REG` with `reg == R(g)`; `reg_to_temp[R(g)]` points at it from
`tcg_reg_alloc_start` on; R(g) is in `reserved_regs`, so `tcg_reg_alloc`
never hands it to anything else.

- **Outputs** go into R(g) directly (`add eax, ebx` is one `add w20, w20,
  w21`; `deposit eax, eax, t`, a byte or word write, is one `bfi` in
  place). When the op's constraints forbid it (an output aliased to an
  input that is not the same global, a "new register" output whose R(g)
  is also an input, a paired output), the op writes a scratch register
  and one `mov` follows. A `movi` into a pinned global materializes the
  constant in R(g) at once (a memory-only global would stay
  `TEMP_VAL_CONST` until used).
- **Inputs** never give their register to an output: the "dead input
  reuses its register" shortcuts of `tcg_reg_alloc_op` (aliased and
  paired forms) and `tcg_reg_alloc_mov` are closed for pinned temps, as
  for `TEMP_FIXED`.
- **Dead and sync.** `temp_dead` is a no-op for a pinned temp;
  `temp_sync` stores from R(g) and sets `mem_coherent`. A `DEAD_ARG` on a
  pinned *output* does not skip the register write
  (`tcg_reg_alloc_mov`'s store-only path is closed): a memory-only write
  would leave a stale register that a later slow-path stub stores back
  over the slot.
- **Calls.** `tcg_reg_alloc_call` stores the non-coherent pinned globals
  unless `NO_RWG`, and reloads all of them after the call unless `NO_WG`
  (before assigning the call's outputs; a pinned output gets the return
  register moved into R(g)).
- **Block ends.** `save_globals` / `sync_globals` / `tcg_reg_alloc_bb_end`
  / `tcg_reg_alloc_cbranch` accept a pinned temp in its register (they
  assert memory for the others) and emit nothing.
- **Memory accesses.** With every callee-saved register pinned, TCG's
  rule "free every caller-saved register before a `qemu_ld/st`" would
  spill every temp live across a memory access, so the aarch64 slow path
  saves and restores the live caller-saved registers itself
  (`TCG_TARGET_LDST_SAVES_LIVE`).

## Liveness and coalescing

`liveness_pass_1` decides where memory globals are synced; for pinned
globals the memory slot only matters at helper calls:

- Ops with `TCG_OPF_SIDE_EFFECTS` (`qemu_ld`/`qemu_st`) no longer mark
  pinned globals `TS_MEM`: the slow-path stub covers the fault case.
- At basic-block ends and TB exits (`la_bb_end`, `la_func_end`, the
  conditional-branch sync) a pinned global is *live in its register*
  rather than dead-in-memory, so a definition just before a `goto_tb`
  or a label is kept and lands in R(g).
- Calls keep their rules: a helper that may write globals kills them
  (`TS_DEAD | TS_MEM`: the last definition before the call is stored,
  and still written to R(g)); a helper that may read them syncs them.

**Coalescing is what makes pinning pay.** The i386 translator computes
every result into a temp and moves it into the register global. For a
memory global that move is the store it needs; for a pinned one it is an
extra `mov`, and without coalescing pinning gained nothing (7-Zip's own
CPU-frequency loop halved). `la_coalesce_pinned_mov` renames the temp's
definition to the global and drops the `mov` when nothing in between
reads or writes the global, reads the temp, may fault or ends the block.
The optimizer's copy propagation takes a pinned global as the canonical
copy of a value (`cmp_better_copy`): reading the register is free, and a
load's address then needs no copy.

## Init order and API

The prologue and epilogue need the list of pinned globals with their
`env` offsets, but the target creates its globals in
`TCGCPUOps::initialize` (i386: `tcg_x86_init`) at the first CPU's
realize, after `tcg_init_machine` emitted the prologue. The patch moves
the system-mode `tcg_prologue_init()` call from `tcg_init_machine` to
`tcg_exec_realizefn`, right after `initialize()` (user-mode already calls
it after `cpu_create`); no code is generated and no vCPU thread exists
in between.

`tcg_global_pin_i32/i64(TCGv)` in `tcg.h`, called by the target after
`tcg_global_mem_new`, takes the next non-reserved register of the
backend's `tcg_target_pinned_regs[]` (user-mode's guest base takes x28)
and returns false when the list is exhausted or empty, so target code is
portable. aarch64 lists x20–x28; the x86-64 backend lists nothing (rbx,
rbp, r12, r13, r15 would be free). i386 pins, in order: `eip`, `eax`,
`ecx`, `edx`, `ebx`, `esp`, `ebp`, `esi`, `edi`. Not pinned:
`cc_dst`/`cc_src`/`cc_src2`/`cc_op` (no registers left; still synced
before each memory op when modified) and the segment bases (read-only in
practice, loaded once per use).

Switches:

- `-accel tcg,pinned-regs=on|off` (the `tcg_pin_globals` flag, read at
  machine init; the prologue is emitted once, so it cannot change at
  run time). Off is the oracle: allocator, liveness, slow path and
  prologue run as upstream.
- `QEMU_TCG_OPTS=pinned-regs=on` runs the DOS batteries and the game
  runners with it.
- `QEMU_TCG_PIN_MAX=n` caps the number of pinned globals.

## What it costs

- One `ldr` per pinned global at every entry from the main loop and one
  `str` at every exit to it (9 + 9), now unconditional where the first
  and last TB of a chain used to pay piecemeal. A chain that returns to
  the main loop every TB (interrupt storms, `-d nochain`,
  `one-insn-per-tb`) breaks even; every chained TB after the first gains.
- A helper call that is not `NO_RWG` stores up to 9 registers (once per
  TB unless they are modified again); one that is not `NO_WG` reloads 9.
  Helper calls are ~5 % of the hot instructions on 7-Zip (`div`, `mul`
  with flags, `cpuid`, `rdtsc`, port I/O, the x87 and SSE cases that
  leave the inline paths) and each already costs a hundred cycles.
- 9 stores in every `qemu_ld`/`qemu_st` slow path (a TLB miss: ~1 % of
  accesses after patch 16).
- Nine fewer registers for TCG temps: x0–x15 remain (x16/x17 are the
  backend's scratch, x18 the platform register). The only long-lived
  values are the globals, which are pinned, so losing callee-saved temps
  costs spills only around calls.

## Measured

7-Zip's benchmark: compress +3 %, decompress +15–16 % over off. Pinning
removes instructions, not latency: the "41 % of generated-code samples
on register loads" that motivated it was mostly the sampler's skid off
the softmmu TLB chain. With the loads gone the samples moved to the ALU
work after them and the chain's share stayed at 42 %.

## Open

- **The crash.** XP reboots or bugchecks with registers pinned: seen
  with eight pinned, with all nine in doc 22's `pinned` configuration
  (during Super PI) and with seven in doc 23's spike E, so it is the
  pinned path, not a particular register. It reproduced with
  `tools/specbench/run.sh <image> pinned` on the 9.2.4 tree.
- **A stall at the flags-helper call boundary.** `helper_cc_compute_c`
  at 4 % of the vCPU instead of 1 %, most samples on its first
  instruction.
- The M9 track lists both with the follow-ups (cheaper stores/reloads
  around helper calls, the x86-64 register list).

## Test and oracle

- `scripts/test.sh all` runs the DOS batteries (x87, rep, SMC, SSE, each
  comparing its own on/off pair) and the XP guest stage unpinned, the
  default. On the 9.2.4 tree, `QEMU_TCG_OPTS=pinned-regs=on` on the DOS
  tools ran the same batteries pinned, an A/B of the pinning alone.
- 7-Zip's benchmark verifies every decompressed block against a CRC, so
  a run to completion checks a few billion instructions. It passes; XP
  under Super PI does not (above).
- The `optimizations` check (`docs/testing.md`) requires that
  `pinned-regs` never reaches the command line from a bundle.
