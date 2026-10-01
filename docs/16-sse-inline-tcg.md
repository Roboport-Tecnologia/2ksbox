# 16. SSE and MMX inline in TCG

How SSE float, MMX and SSE integer instructions run as inline host code
under TCG instead of helper calls: patch 11 (SSE/SSE2 float), patch 12
(MMX / SSE integer and permutes), and patches 36 and 39, which removed
two stalls. Doc 13 is the x87 counterpart, whose slow-block machinery
this shares; doc 22 measures the queue; `docs/tracks/m8-tcg-fastpaths.md`
holds the track record.

Stock QEMU runs nearly every SSE instruction as a helper:
`helper_addps_xmm` loops over four lanes of `float32_add`. Once the
inexact flag is sticky that is softfloat's hardfloat path, a host
operation wrapped in classification checks inside a call frame with
three pointer arguments. Direct3D-era games use SSE for the work that
runs every frame: D3DX vector and matrix routines are packed SSE,
compiled scalar math and float-to-int conversions are `ss` instructions,
and era blitters, mixers and codecs are MMX.

Files: `target/i386/tcg/sse-fast.c.inc` (instruction logic, slow
blocks, the packed vector path), `sse-fast-lane.c.inc` (scalar lanes in
general registers, included for 32 and 64 bits), `simd-fast.c.inc`
(patch 12), TCG opcodes in `include/tcg/tcg-opc.h` and both backends.

## The mode: MXCSR admissible and PE already sticky

A TB is translated with `TB_FLAG_SSE_FAST` (bit 31) when

- `env->sse_fast_mode` is set: MXCSR has RC = nearest, FTZ and DAZ off,
  all six exceptions masked, and the CPU property `sse-fast` is on
  (default; `-cpu pentium3,sse-fast=off` is the control). Every
  `cpu_set_mxcsr` caller keeps it current through
  `update_mxcsr_status()`;
- the precision (inexact) flag is already set in `env->sse_status`.

The second condition keeps the inline code flag-exact without the
per-lane residual the x87 path computes while PE is clear. With PE
sticky, an admissible host operation can only raise PE, which is
invisible, so the code only has to verify that nothing else would have
been raised: one unsigned compare per lane on the result (on the
operands, for compares). That is exactly the gate of softfloat's own
hardfloat path (`can_use_fpu`), so the helper path and this one agree on
when the host FPU is acceptable. A thread's MXCSR gets PE within its
first few float operations and never loses it (Windows saves and
restores MXCSR, flags included, with `fxsave`/`fxrstor`).

Consequences for the translator:

- `ldmxcsr`, `fxrstor` and `xrstor` end the TB: they can change MXCSR
  or clear the flags, and a chained successor must not carry a stale TB
  flag.
- A TB translated without the flag emits, after each helper that may
  raise PE, a check at the end of the instruction (`sses_helper_done`):
  if the fast mode applies now, exit to the next instruction. Without it
  a loop translated before its first inexact result would keep running
  the helpers through its direct TB chain. The check must follow the
  register and flags write-back; an earlier version exited before it and
  `cvttss2si` lost its EAX.
- A runtime guard at a fast TB's first inlined instruction
  (`sses_guard`: mode byte and flag word) re-executes from a correctly
  translated block if the state does not match. With the exits above it
  cannot fire; it costs four instructions per TB.
- `info registers` prints the slow-path counters `SSE-fast slow paths:
  guard= handover= helper= cvt/comis=`. A helper or cvt/comis count that
  grows by millions means the workload is on the slow path (an
  out-of-range `cvttss2si` loop once cost 2× the helper this way).

## Fast conditions

Softfloat with DAZ/FTZ off treats denormal operands as exact values,
but since QEMU 10.1 (`57df511180`) it raises DE when it consumes one.
So every instruction below except rcp/rsqrt (whose helpers restore the
flags) and the int32 conversions (no DE, as on the hardware) also needs
no denormal operand. Beyond that, what must be excluded is everything
that would raise a flag other than PE, or where QEMU's result differs
from the host's (NaN payload rules):

| Instructions | Fast when | Why that is enough |
|---|---|---|
| add, sub, sqrt (ps/pd/ss/sd) | result exponent field not all ones | no NaN operand, no infinity, no overflow; a tiny add/sub result is exact (both operands are multiples of the smallest denormal) and sqrt cannot underflow |
| mul, div, cvtsd2ss | result exponent field in [2, max−1], or an exact zero because a factor / the dividend is zero | excludes NaN, inf, overflow, division by zero and any underflow whether tininess is detected before or after rounding (a result that rounded up into field 1 is tiny before rounding) |
| rcp, rsqrt (ps/ss) | normal result | the helpers restore the flags, so only the value matters |
| min, max, cmp (8 predicates), comis, ucomis | no NaN operand | NaN would need QEMU's NaN selection and IE; the compare itself depends on the host (below) |
| cvt(t)ss2si, cvt(t)sd2si | \|x\| < 2^31 − 1/2 | the host conversion is exact for both roundings; softfloat returns 0x80000000 with IE otherwise |
| cvtsi2ss, cvtsi2sd | always | only PE |
| cvtss2sd | result not NaN/inf | an SNaN input raises IE |

The denormal check is left out for a register already known to hold
none at that point of the TB (`sses_clean`, per register in four views:
binary32 or binary64, all lanes or lane 0): one that passed the check
earlier in the TB (a failure leaves the TB), or the result of mul, div,
sqrt, rcp, rsqrt, min, max, a compare or a conversion, none of which
can produce a denormal on the fast path. Any other write to an xmm
register forgets every register, except a `movaps`-family register move,
which copies the bits. In a loop body most operands are such registers,
so a typical instruction checks one operand or none.

Everything else takes the instruction's slow block: the unmodified
helper sequence out of line, then a TB exit to the next instruction, as
doc 13's x87 shadows do (the two share the slow-block array and
emitter). All lanes are computed and checked before anything is stored,
so the helper re-runs on intact operands. Legacy SSE encodings only; VEX
forms take the helpers. A TB with more than 94 inlined x87 + SSE
instructions falls back to helpers for the rest.

## Two code shapes

**Packed** (`ps`, `pd`): the vector unit. New TCG opcodes `fadd_vec`,
`fsub_vec`, `fmul_vec`, `fdiv_vec`, `fsqrt_vec` (vece MO_32/MO_64;
aarch64 `fadd.4s` etc., x86-64 `vaddps`/`vaddpd`), with the checks built
from TCG's integer vector ops (`shli`, `cmp`, `and`, `add`, `andc`,
`sari`, `bitsel`). The checks compare |x| (the sign bit masked off, so
never negative) with signed compares, which both hosts have natively:
an unsigned vector compare costs x86-64 without AVX-512 three or four
extra instructions (a `pmaxud` + `pcmpeqd` + inversion, or a bias on
both sides for 64-bit lanes), which on 11.1 with the DE checks made the
packed bench 1.8x slower than on 9.2. The lane mask is all-ones or all-zeros per lane and
the branch tests it with `vec_allsign_i32` (patch 39: `vpmovmskb` + `xor`
on x86-64, `cmlt #0` / `uminv` / `umov` / `eor` on aarch64). Before that
op the mask went through `env->sses_scratch` and two 64-bit loads, a
store-to-load dependency in front of every SSE op's branch; that is
still the fallback on a backend without it. A packed `mulps` is ~25 host
instructions on the M1 including constant materialization (~100 with
four scalar lanes in general registers, ~130 for the helper).

**Scalar** (`ss`, `sd`, the conversions, comis): general-register lanes
with the scalar opcodes `add_f32` .. `sqrt_f32` (i32 temps living in
vector registers, as doc 13's `add_f64` on i64), `cvt_i32_f32/f64`,
`cvt(t)_f32/f64_i32` (aarch64 `scvtf`, `fcvtns`, `fcvtzs`; x86-64
`vcvtsi2ss`, `vcvtss2si`, `vcvttss2si` and the sd forms). A scalar
`mulss` is ~20 host instructions; the vector shape lost for scalars to
the memory round trips it needs for lane 0.

**Memory operands (patch 36).** A 16-byte operand used to be loaded with
`qemu_ld_i128`, two 8-byte loads on a guest without AVX (every era CPU
model), and written into `env` with two 8-byte stores, so the inline
op's 16-byte vector load could not be store-forwarded and waited for
both. The pair is now assembled in the vector unit (each half dup'ed,
merged with `SIMD_TBL_MASK64LO`) and written with one `st_vec`, with the
same guest access, alignment check and fault. It is behind `sse-fast`.

**min / max / cmp** on aarch64 are an integer compare of total-order
keys: a 16–18-op transform (fold −0 into +0, flip negative magnitudes)
so `cmp_vec` sees a monotonic order. The x86-64 backend uses native
instructions instead (below).

## Patch 12: the integer and permutation instructions

Between the inlined `mulps`/`addps` of a D3DX vertex transform sit four
`shufps`, and the MMX loops of the era are all `punpck*`, `pack*`,
`pmulhw`, `pmaddwd`, `pavg*`, `psadbw`, shifts by a register count and
`pshufw`, each a helper call, plus a helper call for the MMX entry
(`fpstt`/`fptags` reset) before every one. Patch 12 (property
`simd-fast`) translates them inline:

- the permutes (`shufps`, `shufpd`, all `unpck*`, `pshufw`) through a
  new TCG vector opcode `tbl_vec`, a byte table lookup with zeroing
  (`tbl` on aarch64, `vpshufb` on x86-64) whose index vectors are
  precomputed per instruction and immediate in `env->simd_tbl` at CPU
  init (a two-source permute is two lookups and an OR, one lookup when
  both sources are the same register);
- on 64-bit halves in integer registers: the saturating packs
  (`smax`/`smin`/`umin` per lane), `pmulhw`/`pmulhuw`/`pmaddwd`, the
  average identity `(a | b) - ((a ^ b) >> 1)` lane-masked, `psadbw` as
  the vector unit's byte `umax - umin` plus a SWAR sum;
- gvec scalar shifts with the over-width count masked to zero
  (sign-filled for `psra*`);
- the MMX entry as three stores.

Integer ops have no flags or modes, so exactness is by construction.
Legacy encodings only (VEX forms, ymm, `pmuludq`, SSSE3 stay on
helpers); hosts without `tbl_vec` use a bit-spreading / lane-store
fallback kept in the file.

Two traps:

- **Anything that feeds the vector path must produce its result as one
  vector store.** Shuffles written as four 32-bit lane stores made the
  next instruction's 128-bit load of that register stall on the M1 (no
  store-to-load forwarding across several smaller stores), and the
  transform kernel got *slower* than with the helper. That is why
  `tbl_vec` exists; patch 36 is the same lesson.
- **`decode->immediate` is sign-extended** (0xB1 arrives as −79), so
  table indices must mask it.

## The x86-64 backend

The codegen was written on aarch64. On x86-64 it was bit-identical but
slower in places: x86-64 lacks aarch64's cheap bitfield insert
(`BFI`/`SBFX`), so the SWAR packs reached only 3–3.9×. Where the host
speaks the guest's ISA, four groups of generic TCG vector opcodes now
emit native instructions. They are x86-64 only (the `TCG_TARGET_HAS_*`
macros are 0 on aarch64, where the portable code stays):

| Opcodes | x86-64 instructions | Gate |
|---|---|---|
| `mulsh_vec` / `muluh_vec` | `PMULHW` / `PMULHUW` | `TCG_TARGET_HAS_mulh_vec` |
| `ssnarrow_vec` / `usnarrow_vec` | `PACKSSWB` / `PACKUSWB` / `PACKSSDW` | `TCG_TARGET_HAS_pack_vec` |
| `fmin_vec` / `fmax_vec` | `VMINPS` / `VMAXPS` | `TCG_TARGET_HAS_fp_minmax_vec`, `have_avx1` like `fadd_vec` |
| `fcmp_vec` | `VCMPPS`; the third argument is the guest's own CMPPS predicate, 0–7 | `TCG_TARGET_HAS_fp_cmp_vec`, `have_avx1` |

- **min / max / cmp without the total-order key.** The native
  instructions already implement the guest's −0/+0 tie-break and NaN
  operand selection, so the guard needs only a 6-op NaN-presence check
  (`sses_vnan_ok`) instead of the 16–18-op key.
- **MMX-width packs.** The low 64 bits of a 128-bit `PACKSSWB` come from
  all eight words of its first operand, not a 4+4 split like the 64-bit
  MMX form, so the two 4-word operands are concatenated into one
  register and narrowed against itself, keeping the low half. The
  concatenation is `dup_i64_vec` + `bitsel_vec` against a lane mask
  (`SIMD_TBL_MASK64LO` in `env->simd_tbl`); going through
  `env->sses_scratch` (two GP stores reloaded as one vector load) hit the
  forwarding stall again.
- **`psadbw`** uses the register-only `umax_vec`/`umin_vec`/`sub_vec`.
  The memory-based `tcg_gen_gvec_*` forms are built for long vectors and
  made `psadbw` slower than the helper it replaced.

Effect on the DOS bench (below): packed chain 8.1× → 10.0×, MMX chain
1.4× → 2.1×, the isolated clamp+cmp kernel 6.5× (the Air gets
8.5–8.7× on the key path).

## Verification

`tools/sse-guest-test.py` (the `sse-guest` check): a DOS program enables
SSE in real mode (CR4.OSFXSR) and runs its instruction sequences: all of
the table above, memory-operand forms, `cmpps` with every predicate, a
4-op packed chain, a 7-op scalar chain with a conversion round trip, a
mixed x87/SSE block that exercises dirty x87 shadows across an SSE
slow-block exit, a 120-instruction block that overflows the slow-block
array, the SSE2 double forms under `-cpu pentium3,+sse2`, and patch 12's
integer battery (MMX and XMM forms, memory operands, self-operands,
chains, a mixed x87/MMX sequence). Each runs over every pair of 47
single and 33 double edge-case values (zeros, denormals, min/max, 2^31
boundaries, infinities, quiet and signalling NaNs), under MXCSR 1FA0
(inline mode), 1F80 (flags clear: the hand-over path) and 3FA0 (round
down: helpers), printing every lane and MXCSR. **546,425 result lines
identical** with the switches on and off, on aarch64 and x86-64. The x87
battery (doc 13) must pass too, since the slow blocks are shared.

## Performance

`SSEBENCH.COM` (in the same test run): register-only kernels, 40 M
iterations, BIOS ticks. On the M1 Air after patch 11:

| Kernel | helpers | inline | ratio |
|---|---|---|---|
| packed: mul, add, mul, add, div, max, min, sub (8 ops) | 4.56 s | 0.38 s | **12×** |
| scalar: mul, add, div, sqrt, add, comiss+branch, cvttss2si (7 ops) | 2.80 s | 0.77 s | **3.6×** |

That is ~1.2 ns per packed op inline (bound by the latency of the
dependent NEON chain; the checks overlap in the out-of-order window)
against ~14 ns for the helper, and ~2.7 ns per scalar op against ~10 ns,
where the scalar path pays the general-register moves of its checks.
Memory-operand forms add the TLB lookup on both paths (the same kernel
with memory operands showed only 1.3×), so a real loop sees less than
the ratio.

Register-only ratios over the helpers with both patches, per host:

| Host | x87 (PC=53 / PC=24) | packed SSE | scalar SSE | MMX chain |
|---|---|---|---|---|
| x86-64 (Ryzen 7 5700X) | 8.1× / 6.7× | 7.5× | 3.9× | 2.1× |
| the Air | 10.6× / 5.7× | 13.8× | 3.4–4.1× | 3.6–3.7× |

`guest-tools/src/ssebench.c` (`SSEBENCH.EXE` on the guest-tools ISO) is
the Win32 counterpart for the reference rig and the guests: D3DX-shaped
kernels (packed transform, packed normalize with `rsqrtps`, a scalar
chain with `comiss`, clamp + `cmpps` (the "clamp+cmp" kernel) and
`cvttss2si`/`cvtsi2ss`), the transform and normalize again in plain C
pinned to x87 at PC=53 (doc 13's path), an MMX blend, and a
denormal-decay kernel that shows the slow-path cost. It prints ns per op
and a mean "SSE score". `tools/xp-ssebench.sh` runs it in XP headlessly
per `-cpu` configuration and prints the slow-path counters around the
run.

Against the rig (the rig's time ÷ ours, Air / x86-64 box), with every
`check` value identical to the rig's: normalize, scalar chain, convert,
C normalize and MMX blend land at 87–130 %, the packed transform at
61 % / 49 %, and the two outliers are clamp+cmp (34 % / 43 %) and the
x87 C transform (18 % / 16 %). `reference/benchmarks/README.md` has the full
per-kernel table, the rig's row and the measurement pitfalls. Clamp+cmp
is not register-only: each iteration does an aligned 16-byte load and
store (two softmmu TLB lookups) and a `movmskps`, which still goes
through `helper_movmskps_xmm`. On XP on the Air, `sse-fast=on` against
`off`: packed transform 7.4×, normalize 6.3×, scalar chain 3.3×,
clamp+cmp 4.4×, convert 3.2×; the denormal kernel runs at 0.6× of the
helper, the price of a check that fails on every operation (doc 22 §5.3
measures the same). Doc 22 §5 has these kernels against pristine QEMU.

## Follow-ups

- **The rest of clamp+cmp.** Inline `movmskps`/`movmskpd` (a sign-bit
  gather), then reduce the per-operand TLB cost of memory operands.
- **Cheaper packed checks.** The checks re-materialize their vector
  constants per instruction (two instructions each on aarch64); a
  constant pool or hoisting in the backend would trim ~6 of the ~25.
  RAPIDO 2025's trimming (doc 23 §4) is bounded: removing every check
  outright buys ≤ 6 % of the SSE score, none of it in the packed
  kernels.
- **The scalar path's checks** move values between vector and general
  registers (`fmov`/`ins`/`umov`, ~4 cycles each on Apple cores);
  keeping lane 0 in place would need a vector-to-scalar move opcode in
  TCG.
- **Still on helpers.** `cvtps2dq`, `cvttps2dq`, `cvtdq2ps`,
  `cvtpi2ps`/`cvtps2pi`, `pmuludq`; SSE3 `haddps`/`addsubps`, SSSE3 and
  VEX forms have no era relevance. (`movlhps`/`movhlps`/`pshufd`/
  `pshuflw`/`pshufhw` were already inline upstream.)
- **Barriers.** Every guest memory access carries a `dmb` because the
  pc machine's `max_cpus` is above 1 (`-smp 1,maxcpus=1` turns them
  off). The memory-operand bench showed no difference on the M1; 7-Zip
  read 2–10 % faster without them. Dropping them by default needs an
  audit (M9 track, "Open").
