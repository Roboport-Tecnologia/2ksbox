# 13. x87 on the host FPU: the shadow stack in TCG

How x87 floating point runs under TCG without softfloat for the common
case: patch 05 (helpers on the host FPU), patch 06 (the stack kept as
host values inside generated code), and the patches that extend it to
every precision a Windows program uses: 37 (sticky inexact), 45
(binary32 at PC=24), 47–49 and 67 (PC=64). SSE and MMX are in doc 16,
benchmark and game measurements of the whole queue in doc 22, the track
record in `docs/tracks/m8-tcg-fastpaths.md`, each patch's row in
`patches/qemu/README.md`.

## The modes

Stock QEMU runs every x87 instruction as a helper call into softfloat's
80-bit arithmetic. The fast paths apply when RC = nearest, the precision
exception is masked and the CPU property `x87-fast` is on (default;
`-cpu pentium3,x87-fast=off` is the control). The precision control then
picks one of four modes, a 2-bit TB flag (`TB_FLAG_X87_MODE`, bits
29–30, from `env->x87_fast_mode`):

| Mode | PC | Who runs there | Shadow format |
|---|---|---|---|
| 0 | any, with RC ≠ nearest, PE unmasked or `x87-fast=off` | MSVC's `_ftol` (truncation), briefly | none: softfloat helpers |
| 1 | 53 bits | Windows' default for a process | binary64 (patch 06) |
| 2 | 24 bits | Direct3D sets it at `CreateDevice` (unless the app asks to preserve the FPU), so a 3D game's whole frame | binary32 (patch 45) |
| 3 | 64 bits | the x87's reset default: code that never sets the control word, mingw's C runtime, 3DMark2001's physics | the x80 itself (patch 48) |

`-cpu …,x87-pc64-as-53=on` (patch 47, off by default, **not exact**)
maps PC=64 to mode 1 instead.

## Why helpers were slow (patch 05)

Patch 05 computes an operation on the host FPU whenever the operands and
precision make the host's binary64/binary32 result the correctly rounded
x87 result, and falls back to softfloat otherwise. That halved the cost,
but on a 7-op DOS loop (`fld m64`, `fmul m64`, `fadd m64`, `fstp m64`,
`fld m64`, `fdiv m64`, `fistp m32`) on x86-64, helpers still took 98 %:

| Symbol | Share |
|---|---|
| `x87_binop` (the host-FPU body incl. checks and TwoSum) | 33 % |
| `merge_exception_flags` | 11 % |
| individual `helper_f*` (call frame, flag save, FT0/ST0 round trip) | 45 % |
| translated code (TLB lookups, FIP/FDP stores, calls) | ~2 % |

Inlining the body into the helpers gave 10 %: the cost is the call per
op, every value passing through `env->ft0` / `env->fpregs[]` as floatx80,
and the flag save and merge. Patch 06 keeps the stack in host registers
across instructions instead.

## Float opcodes in TCG

TCG had no float opcodes, but the allocator handles vector registers
(gvec), `tcg_out_mov` moves i64 values between general and vector
registers in both backends, and `tcg_out_ld/st` accept an i64 temp in a
vector register. So a scalar float op is an ordinary opcode on i64 temps
whose operand constraint is the vector class:

- `include/tcg/tcg-opc.h`: `add/sub/mul/div_f64`, `fmsub_f64` (a·b − c,
  fused), `sqrt_f64`, `cvt_f64_f32`, `cvt_f32_f64`, offered where the
  backend defines `TCG_TARGET_F64` (`tcg_f64_supported()`). On QEMU 11.1
  each backend's half is a `TCGOutOp` descriptor in `tcg/tcg.c`'s
  scheme (`TCGOutOpBinary`, `TCGOutOpUnary`, and `TCGOutOpTernary` for
  `fmsub_f64`; `patches/qemu/README.md`, "06").
- x86-64: `vaddsd/vsubsd/vmulsd/vdivsd/vfmsub213sd/vsqrtsd/vcvtsd2ss/vcvtss2sd`,
  constraints `C_O1_I2(x, x, x)` etc.; requires AVX + FMA3
  (`CPUINFO_FMA`).
- aarch64: `fadd/fsub/fmul/fdiv/fnmsub/fsqrt/fcvt` (scalar D, `fcvt` both
  ways), `C_O1_I2(w, w, w)` etc., plus `tcg_out_movi` into a V register.
- Other hosts and TCI: no `TCG_TARGET_F64`, and the translator emits
  helper calls as before.

Mode 2 (patch 45) uses the binary32 scalar opcodes on i32 temps,
`add_f32` … `sqrt_f32`, that patch 11 added for SSE (doc 16).

## The shadow stack (`target/i386/tcg/x87-shadow.c.inc`)

**Mode checks.** Everything that can change the control word (`fldcw`,
`fldenv`, `frstor`, `fninit`, `fnsave`, `fxrstor`, `xrstor`) ends the
TB. A one-load runtime guard at each TB's first inlined instruction
exits to that instruction if the mode does not match, and the lookup
then finds the right variant; `info registers` counts these exits
(`x87-fast guard exits`).

**Stack model.** The translator addresses the stack relative to the top
at the TB's first x87 instruction (a runtime `entry_top` loaded once):
relative slot r is physical register `(entry_top + r) & 7`, and `ST(i)`
is slot `(delta + i) & 7` where `delta` counts pushes and pops. This
costs no TB-flag bits and no TB variants per stack depth. Each slot is
statically MEM (only `fpregs[]` holds it), CLEAN (shadow equals memory)
or DIRTY (shadow newer). The shadows are TCG globals backed by `env`:
`cpu_x87_sd[]` (binary64, mode 1), `cpu_x87_ss[]` (binary32, mode 2),
`cpu_x87_xl[]` / `cpu_x87_xh[]` (x80 mantissa and sign | exponent,
mode 3). `env->fpstt` and `env->fptags` stay current in memory (one
store per push/pop), so helpers and the unwinder always see the real
stack layout.

**Invariants.** No DIRTY slot at any TB exit or before any helper that
may read `fpregs[]`. Helpers that may write `fpregs[]` or move the top
(all non-inlined x87 instructions, MMX entry, `fxrstor`/`xrstor`) get a
boundary: flush, forget shadows, reset the frame. Exit hooks live in
`gen_eob`, `gen_jmp_rel`, `gen_exception`, `gen_interrupt` and the
`hlt`/`pause`/`mwait`/`vmrun`/`icebp`/`rdpmc` sites; their flush code
leaves the static state untouched, so conditional exits inside a TB stay
correct.

**Faults mid-TB.** A guest load or store can fault with DIRTY shadows.
TCG already syncs globals to memory before every `qemu_ld/st`, so the
shadow values are in `env`; a third `TARGET_INSN_START_EXTRA_WORDS` word
(dirty mask + delta) lets `x86_restore_state_to_opc()` convert them back
into `fpregs[]` during unwind, for whichever mode the TB was translated
in. Inlined memory instructions do their guest access before any
architectural change, so the faulting instruction's word describes
exactly the state to repair.

**Fast path.** Shadows hold only zero or normal values inside the mode's
window (±900 binary exponent in mode 1, the binary32 range in mode 2,
the x87's own 1..0x7ffd in mode 3), so operands need no checks and
results are checked once (window; underflow for mul/div; divisor
nonzero). The fast path is branchless except for the branches to the
slow block and has no labels, so TCG keeps its globals (and the shadows)
in registers across instructions.

**Inexact flag (patch 37).** While PE is still clear, the inexact flag
comes from a TwoSum/FMA residual ORed into `fpus`. PE is sticky and
masked in inline mode, so once it is set none of that is observable.
`TB_FLAG_X87_PE` (TB-flag bit 2) marks a TB translated with PE already
set, which computes no residuals; the guard re-checks `fpus.PE` at run
time (a chained TB is not looked up again), and `fclex` / `fninit` /
`fldenv` / `frstor` / `fnsave` turn the sticky variant off for the rest
of their TB. Every program is sticky after its first inexact operation.
Patch 20's inline TB lookup builds the TB flags as TCG ops and must
carry this bit too: without it a chained exit misses, which cost a
quarter of 3DMark 99's first-person frame rate in epilogue exits. The
same goes for any future x87 TB flag.

**Slow blocks.** Every instruction records one out-of-line block
(`X87SlowBlock`: static stack state at instruction start, pc/cc state,
operands as TB temps). At `tb_stop` the blocks are emitted after the
normal exit: write back all dirty shadows, run the unmodified helper
sequence (including the store and pop for `fist`/`fst`), update
FIP/FDP, exit the TB to the next instruction. A hot loop that hits a
slow case (NaN/inf/denormal operands, values outside the window, 64-bit
mantissas from `fldpi`/`fsin` in modes 1–2, `fst m32` overflow, `fist`
out of range) pays a helper plus a TB exit per occurrence. The
slow-block array (96 entries, shared with doc 16's SSE path) limits a TB
to 94 inlined x87 + SSE instructions; the rest fall back to helpers.

**Deliberate deviation.** A popped register keeps its previous
`fpregs[]` content instead of the popped value. Materializing every
popped slot would cost ~20 host instructions per `fstp`, and only a read
of an empty register can observe it (QEMU never faults on that; real
hardware answers with an indefinite NaN). `fxsave` images of empty
registers can differ from patch 05's; `fxsave`/`fxrstor` round trips
(context switches) are unaffected.

## Coverage and fallbacks

Inlined: `fld/fst/fstp m32/m64`, `fild/fist/fistp m16/m32`, all six
arithmetic ops with m32/m64/st(i) operands including the popping and
reversed forms, `fld/fst/fstp st(i)`, `fxch`, `fcmovcc`, `fld1`,
`fldz`, `fchs`, `fabs`, `fsqrt`, `frndint`, `fcom/fcomp/fcompp`,
`fucom/fucomp/fucompp`, `fcomi/fucomi(p)`, `ftst`, `ffree(p)`,
`fincstp/fdecstp`, `fnstsw ax` (mode 3: all but `fsqrt` and `frndint`).
Helpers with a boundary: `fld/fstp m80`, `fild/fistp m64`, `fisttp`,
`fbld/fbstp`, transcendentals, `fscale`, `fprem`, `fxtract`, `fxam`, the
other constants, `fnsave/frstor`, `fnstenv/fldenv`. No boundary needed:
`fnstcw`, `fnstsw m16`, `fnclex`, `fwait`.

## PC=24 mode

Mode 2 is exact for 24-bit precision in two shapes:

- **PE sticky (the steady state, patch 45).** The shadows are binary32
  (`cpu_x87_ss[]`) and an operation is the host's `addss` / `mulss` /
  `divss` / `sqrtss` plus one range check: while the result has a
  binary32 exponent, the correctly rounded 24-bit result is exactly the
  x87's PC=24 result. The rest takes the slow path: overflow, underflow
  and the lowest binade, where a value just under 2^-126 rounds up into
  binary32's normal range but not with the x87's wider exponent.
  `fld m32` is the operand's bits after the zero-or-normal check,
  `fst m32` the shadow's bits with no check and no flag; `fist` /
  `frndint` / `fst m64` widen once and use the binary64 code.
- **PE still to be decided.** Operands are widened and the binary64
  arithmetic runs, each result rounded to 24 bits by `x87s_round24`
  (`cvt_f32_f64` and back). For 24-bit operands that double rounding
  equals the correctly rounded 24-bit result of +, −, ×, ÷ and sqrt
  (53 ≥ 2·24+2). PE is the residual test ORed with "rounding changed the
  bits".

At this precision softfloat rounds m64 operands to 24 bits and raises PE
(`float64_to_floatx80`; real hardware never rounds loads, and patch 05
already defers to softfloat there), so `fld/fxxx/fcom m64` round the
operand the same way inline. Integer loads stay exact in both paths: an
`fild m32` whose value needs more than 24 bits takes a slow block
(`X87S_SLOW_FILD`); `fild m16` always fits.

## PC=64 as 53 bits

Patch 47, the one inexact switch: `x87-pc64-as-53=on` makes
`update_fp_status` map PC=11b to `floatx80_precision_d`. Softfloat then
rounds at 53 bits, `x87_fast_prec` returns the double path and
`x87_fast_mode` becomes 1, while `fnstcw` still returns the guest's own
word. The result loses the low 11 bits of a 64-bit mantissa that a real
x87 would compute. Sampling a program's operands cannot tell which
operations would survive that (two 53-bit operands make a 106-bit
product, and whether its last bits matter is decided later, by a
comparison or an accumulation). Hence off by default, and "not exact" in
the launcher's label.

The workload behind it is 3DMark2001 SE's Lobby at high detail, whose
debris physics alternates `FCW=033f` (PC=64) with Direct3D's `003f`
(PC=24) in the same code pages; before patch 48, x87 helpers and
softfloat's 80-bit path took over half the vCPU.

## PC=64 inline, exact

Patch 48 gives PC=64 its own mode instead of rounding it away. A
double-double (106-bit) shadow was rejected: it still has to fall back
wherever a result sits too close to a 64-bit rounding tie. Integer
arithmetic on the mantissa has no such case and keeps the x87's own
15-bit exponent.

**Mode 3.** Set by `update_fp_status` at PC=64 with RC nearest, PM
masked, `x87-fast` on and `x87-pc64-as-53` off. The shadows are the x80
values (`cpu_x87_xl[]` mantissa as i64, `cpu_x87_xh[]` sign | exponent
as i32, backed by `env->x87_xl/xh[]`), holding zero or a normal like
every mode's shadows (`x87s_x80_check` on a reload). Reload and
materialization are copies; `fld m64` / `m32` are exact conversions of
any zero or normal (softfloat does not round loads at PC=64); `fild`
normalizes the integer inline (`clz`); `fchs`, `fabs`, `fxch`, `fcmov`,
`fld st` / `fst st`, the constants and every compare are integer
operations (a compare orders two 128-bit signed keys, exponent:mantissa
negated for a negative, so −0 is +0).

**Arithmetic.**

- `fmul` is inline (patch 49): the mantissas' 128-bit product
  (`mulu2_i64`; `mul` + `umulh` on aarch64), normalized by its top bit,
  then `x87s_pack_x80`, which is `x87f_pack_x` as TCG ops: the
  pre-rounding exponent checked to 1..0x7ffd, nearest-even on the 64 bits
  below, the carry out of 2^64, PE, a zero operand's signed zero.
  `fst m32` is inline too: the top 24 mantissa bits rounded by the 40
  below, one range check after the carry. Both are label-free apart from
  the slow branch.
- `+ − /` call `helper_x87x_arith`, declared `TCG_CALL_NO_RWG_SE`: it
  reads and writes no guest state, so the shadows stay in host registers
  across the call and it needs no boundary. It returns an i128 holding
  the result mantissa and its sign | exponent with an "inexact" bit (PE,
  unless the TB is PE-sticky) and an "ok" bit, clear when softfloat must
  decide (a pre-rounding exponent outside 1..0x7ffd, i.e. overflow or
  tininess; a zero divisor; an operand that is not zero or normal),
  which branches to the slow block. A label-free inline add would have to
  compute both the add and the subtract and select one, about a hundred
  ops, not clearly cheaper than the call.
- `fst m64` and `fist m16/m32` are helpers of the same shape
  (`x87x_to_f64` / `_int`), rounding nearest-even to the destination and
  refusing what would be out of range.
- Patch 67 fits the helper to the Windows x64 ABI: the two
  sign | exponent words travel as one 64-bit argument (a fifth argument
  goes on the stack behind 32 bytes of shadow space there), and division
  is one `divq` through `X87F_UDIV128` (`udiv_qrnnd`) instead of libgcc's
  `__udivti3`, whose 128-bit operands go through memory on Windows.

The ordinary helpers' PC=64 arithmetic takes the same integer path
(`x87f_binop_x`; `x87_fast_prec` returns `X87F_PREC_X`), so a PC=64
block that is not inlined (single-stepped, past the instruction limit, a
slow block) is faster too. The unwinder copies `x87_xl/xh[]` back for a
mode 3 TB.

## Verification

- `tools/x87-guest-test.py` (the `x87-guest` check): a DOS battery of 68
  instruction sequences (multi-instruction chains, `fcmov`, compares,
  integer and float conversions) over 44² operand pairs and seven
  control words (PC=53/24/64 nearest; truncate, down and up; PE
  unmasked, where the inline path must stay off). Each case runs twice,
  after `fninit` and with PE set first, so the sticky variants run too
  (`fninit` before every case had hidden them). PC=64 adds operands for
  exact 64-bit ties, a cancellation and full mantissas. Result:
  **906,713 lines identical** with `x87-fast` on and off, on aarch64,
  x86-64 and the Windows build under Wine. A 64-instruction block must
  stay one TB, and the script warns when the on/off bench ratio is under
  1.5x (a stale binary with the fast path inactive once passed).
- `tools/x87-fast-test.c`: patch 05's host path against a real x87 (x86-64
  host oracle).
- `tools/x87-unwind-test.asm` (linux-user, not on macOS): faults with two
  dirty shadows outstanding; the FPU state saved in the signal frame must
  equal the helper path's.

## Performance

The 7-op m64 loop above, x86-64 (Ryzen 7 5700X), ns per x87 op:

| Configuration | ns/op | vs softfloat |
|---|---|---|
| softfloat (`x87-fast=off`) | 21.6 | 1.0× |
| patch 05 (helpers on the host FPU) | 10.6 | 2.0× |
| ops inline, x80 round trip per instruction (prototype) | 6.9 | 3.1× |
| **patch 06: shadow doubles across instructions** | **2.9** | **7.4×** |

With patch 06, 93 % of the time is in translated code. `fmul m64`'s
fast path is ~45 host instructions: TLB lookup 9, operand window check
5, multiply + result check with the underflow test folded in 9, FMA
residual + PE 9 (none once PE is sticky), FIP/FDP 7, one sync store of
the shadow global.

The bench loops set PE first. 20 M iterations of seven ops: PC=53 0.33 s
on the Ryzen, 0.38 s on the Air; the single-precision loop at PC=24
(`X87BEN2S`, the shape of Direct3D code) 0.33 s and 0.49 s; the m64 loop
at PC=24, which widens every store and narrows every load, 0.44 s and
0.71 s.

Guest workloads:

- **Super PI 1M in XP on the Air**: 9:49 on softfloat, 6:33 with patch
  05, 1:57 with patch 06, faster than the reference rig's real
  Pentium 4 1.7 GHz (2:02; `reference/benchmarks/README.md`).
- **3DMark 99** (patches 37 and 45 on the Win98 machine): CPU 3DMarks
  13549 → 14690 → 16899. The per-test numbers are in the M9 track and
  doc 22 §6.
- **3DMark2001 SE's Lobby, high detail** (a raw copy of `base98-us`,
  23 five-second windows, no trace):

  | Build | fps (worst window) |
  |---|---|
  | PC=64 on helpers | 35.2 (18.8) |
  | patch 48, mode 3 | 39.7 (23.8) |
  | patch 49, `fmul` / `fst m32` inline | 44.2 (29.4) |
  | patch 47's inexact switch | 50.3 (35.2) |

  In mode 3 before patch 49, softfloat was gone from the profile
  (`parts128_*` 0.1 % from ~20 %) and the arithmetic helpers were ~15 %.
- **Patch 67**, a DOS kernel at PC=64 (`fild`, `fld m32`, `fdivp`,
  `fsqrt`, `fistp`) on the Windows build: 92.5 → 86.0 ns a pass (the
  vCPU had spent 10.8 % in `helper_x87x_arith` and 4.3 % in
  `__udivti3`, against 4.6 % and 1.75 % on Linux); unchanged on Linux.

## Bring-up on the Air

Patch 06 was the first code to keep an i64 temp in a vector register,
and two aarch64 backend paths it reached were wrong. Both were host-side
illegal instructions: SIGILL in the vCPU thread, `cpu_tb_exec` at the
top of the macOS crash report, whose exception code is the instruction
word and so identifies the emitter:

1. `0x4e003c19` = `UMOV x25, v0.d[0]` with a zero element-size field:
   `tcg_out_mov` vector→general passed 0 for imm5; now `4 << type`.
2. A TCG constant fed to an f64 op (`fld1` then `fmulp`): the
   `tcg_out_movi`-into-a-V-register path used `tcg_out_dupi_vec`; it is
   now a movi into `TCG_REG_TMP0` then `INS Vd.D[0], Xtmp`.

Both encodings were checked against llvm-mc. For a new emitter: take the
instruction word from the crash report, or run `qemu-system-i386 … -d
out_asm -D log` on the failing test and read the TB.

## Follow-ups

- `-cpu pentium3,x87-fast=off` is the fallback if a Windows guest
  misbehaves; doc 22 §3 lists the other switches.
- Per-op cost (~45 host instructions in mode 1) can still drop: defer
  FIP/FCS to flush points with the pc in the insn_start word (~4), keep
  FDP eager. The x87 C transform in `SSEBENCH.EXE` runs at 16–18 % of
  the rig; its limit is how many cheap ops the shadow translator issues,
  which needs a redesign rather than a tweak.
- Mode 3: `+ − /` inline as TCG ops (`add2`/`sub2`, `clz` exist on both
  backends) if a profile says the calls dominate; `fsqrt` / `frndint`
  inline.
- aarch64 binary32 at PC=24 trails PC=53 (0.49 s against 0.38 s on the
  Air, equal on the Ryzen); not profiled (M9 track, "Open").
- A Direct3D title in XP with and without `x87-fast=off` as an
  end-to-end number for patches 06, 11 and 12 together (M8 track).
