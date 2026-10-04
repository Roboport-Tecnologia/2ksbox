# QEMU patch queue

Every change 2ksbox makes to QEMU. The tree is the pinned submodule
`qemu/` at v11.1.2, plus qemu-3dfx's OpenGL device (`third_party/qemu-3dfx`)
wired in by our port of its patch (`patches/qemu-3dfx/`: qemu-3dfx stops
at 9.2), plus the patches in this directory. Every row was written on 9.2;
where 11.1 changed a patch's shape, the row says so. Each row's **Drop**
was re-checked against pristine 11.1.2 (2026-10-04, track M21): none is
met yet. Rows 11 and 24 carry 11.1 numbers; the rest, and doc 22, are
9.2's. The larger patches are designed in the numbered docs
(x87 doc 13, SSE doc 16, pinned registers doc 18, CD-ROM doc 17, music
doc 20, Voodoo 2 doc 21); doc 22 measures the TCG patches as a whole.
The test tools named here are in `docs/testing.md`.

## How prepare builds the tree

`scripts/prepare-qemu.sh` redoes everything on each run;
`scripts/build.sh` skips it while its inputs hash the same (`-f` forces it).

1. **Overlays.** Prepare rsyncs in qemu-3dfx's `hw/3dfx` (copied for
   `sign_commit`, never built) and `hw/mesa`;
   `embed/` → `qemu/embed/`; `d3dpt/hw/` → `hw/d3dpt/` with the protocol
   headers; `voodoo/` → `hw/voodoo/`; `libsynth/qemu/` → `hw/audio/`
   (`opl3.c`, `mpu401.c`); `libdisc/qemu/` → `block/cdimage.c` and
   `include/block/`; `gamepad/qemu/` → `hw/usb/dev-gamepad.c` and
   `hw/input/gameport.c`; `tpm/qemu/` → `backends/tpm/tpm_libtpms.c`. Our device code lives in these overlays and is
   edited in the repo; a patch only wires it into QEMU's build and machines.
2. **Restore.** Every tracked file any patch touches is checked out
   pristine, and every file a patch creates is deleted.
3. **Apply.** Our port of qemu-3dfx's patch,
   `patches/qemu-3dfx/00-qemu111x-mesa.patch`, then this queue
   in filename order with `git apply`. A patch that does not apply stops
   the run and prints why. Most patches carry a paragraph of what and why
   above their first diff header, which `git apply` ignores.
4. **Blobs**, which a patch cannot carry. The legacy BIOS date in every
   `pc-bios/bios*.bin` is restored from git and stamped from SeaBIOS's
   06/23/99 to `12/31/99`: Windows 98 setup installs ACPI, and so
   enumerates the PCI bus at all, only when the date is at least its
   `ACPICheckDate` of 12/01/99 (doc 06; the `bios-date` check). Then the
   VGA BIOS built from `patches/seabios/` (VBE 4F09h),
   `firmware/vgabios-{stdvga,cirrus}.bin`, is copied over QEMU's own.
5. **Sign.** qemu-3dfx's `sign_commit` stamps its commit into `hw/3dfx`
   and `hw/mesa`. The guest wrappers must come from the same commit,
   which `guest-tools/build-wrappers.sh` ensures.

**Never `git checkout` files inside `qemu/` by hand between prepare runs**,
and never rely on "already applied" heuristics: a partial tree once
silently lost the 3dfx meson hunk (`unknown type 'glidept'`).

## Editing or adding a patch

A patch is a git-format diff that forward-applies to the tree the patches
before it produce.

1. Run `prepare-qemu.sh`, copy the files you will change (the "pre" tree),
   edit them in `qemu/`.
2. Diff with `git diff --no-prefix --no-index a b` from two copies laid
   out as `a/<path>` and `b/<path>`; a new file needs a `--- /dev/null`
   header. Put a paragraph of what and why above the first header.
3. Prove it from pristine. Run `prepare-qemu.sh` twice (both runs must
   apply everything), then build. A reverse check against an edited tree
   proves nothing.

**When a later patch touches the same files.** Editing patch N can shift
the context a later patch M needs, and `git apply` gives no partial
credit. Patches 11 and 12 share their TCG files, so every change to 11
regenerates 12. The recipe:

1. Take M out of the queue too, and save its payload (the files as M
   leaves them).
2. Regenerate N against the tree the earlier patches produce. Before
   diffing, `git -C qemu checkout --` the files only N touches (prepare
   restores only files some *present* patch lists), and make sure no file
   N creates is in the "pre" tree.
3. Put M's payload back by hand on top and diff it against the N-only tree
   to regenerate M.

The same can be done away from `qemu/` when N's edit does not disturb
M's hunks: in a scratch directory holding only the files N touches
(`git -C qemu show HEAD:<file>`), `git apply -p1 --include=<file>...`
the patches before N, copy that tree, apply N, apply your edit, diff the
two copies for the new N, then `git apply --check` each later patch on
the result. Prepare twice afterwards is still the proof.

Files from an overlay (`block/cdimage.c`, `include/block/cdimage.h`,
`include/block/libdisc.h`, `hw/3dfx`, `hw/d3dpt`, …) are edited in the
repo, never in a patch. The exception is 86Box's vendored Voodoo sources,
which stay verbatim in `voodoo/86box/`: a fix to them is a patch on the
overlay (`hw/voodoo/86box/`, patches 64, 71 and 72), applied after the
rsync.

## The switches

Every optimization has a run-time off switch, so one run diagnoses a
guest that computes a wrong answer, with no bisect. The launcher's
machine form offers fourteen as "Emulation optimizations" (doc 07; the
`optimizations` check). A machine that changed nothing emits no
property, so its command line also runs on a stock QEMU.

| Switch | Where | Patches it gates | Default |
|---|---|---|---|
| `x87-fast` | `-cpu` | 05, 06, 37, 45, 48, 49 | on |
| `sse-fast` | `-cpu` | 11, 36, 39 | on |
| `simd-fast` | `-cpu` | 12 | on |
| `rep-fast` | `-cpu` | 17 | on |
| `x87-pc64-as-53` | `-cpu` | 47 (inexact) | **off** |
| `tb-invalidate-fast` | `-accel tcg` | 15, 35 | on |
| `tlb-floor` | `-accel tcg` | 16 | on |
| `smc-same-value` | `-accel tcg` | 18 | on |
| `tls-hot-paths` | `-accel tcg` | 19 | on |
| `inline-lookup` | `-accel tcg` | 20, 38 | on |
| `soft-imm` | `-accel tcg` | 24 | on |
| `jump-cache-keep` | `-accel tcg` | 42 | on |
| `eob-chain` | `-accel tcg` | 43 | on |
| `tlb-retire` | `-accel tcg` | 44 | on |
| `pinned-regs` | `-accel tcg` | 21 | **off**, not offered |

Accelerator properties are spelled `-accel tcg,<prop>=off` (not
`-machine accel=`); `QEMU_TCG_OPTS=<prop>=off` passes them to the DOS
batteries. The launcher does not offer `pinned-regs` (user decision: too
unstable for too little gain), and a bundle that still turns it on never
reaches the command line. Patches 14, 41, 63 and 67 have no switch
because they change only cost, not behaviour. Device-level A/Bs:
`-global isa-pit.overdue-irq=off` (34), `-global isa-pit.reinject=off`
(65), `-device voodoo2,recompiler=off` (64).

## The patches

Numbers 03 and 57–59 are unused (57–59 are kept for CD-ROM backend
work, doc 17). Two files share the number 20.

### 00-3dfx-darwin-contextalpha
qemu-3dfx's shared code uses `GL_CONTEXTALPHA`, defined only under
`CONFIG_LINUX`, which broke the Darwin build. **Drop:** upstream qemu-3dfx
fixes it.

### 01-upstream-i386-lss-tb-exit-fix
**Dropped in M21** (QEMU 11.1). Upstream since 10.1 (`0f1d6606c2`).

### 02-3dfx-sdl-optional
**Dropped in M21** (QEMU 11.1). Folded into the qemu-3dfx port, which has no SDL2 requirement.

### 04-3dfx-graceful-no-display
With no 3D provider registered, `MGLCreateContext` / `MGLMakeCurrent`
refuse the context instead of taking the VM down: the backend's half of
patch 30's contract, and what a standalone `qemu-system-i386` hits (on
macOS patch 70's backend refuses before asking). **Drop:** never.

### 05-x87-fast
x87 arithmetic on the host FPU when the guest runs at 53- or 24-bit
precision with round-to-nearest (Windows' default, Direct3D's setting).
Bit-exact against softfloat; everything else falls back. Super PI 1M
on the M1 Air 9:49 → 6:33. **Switch:** `x87-fast`. **Test:**
`tools/x87-fast-test.c` (host oracle), `tools/x87-guest-test.py`.
**Drop:** upstream grows a floatx80 hardfloat path.

### 06-x87-inline-tcg
The x87 stack kept as host doubles across instructions inside TCG at
PC=53 and PC=24 (doc 13). Eight scalar binary64 TCG opcodes (x86-64
VEX+FMA3, aarch64); conversion to x80 only at TB exits, before helpers
and on faults (unwind repair through a third `insn_start` word). Anything
unusual runs the helper out of line and exits the TB. Bit-exact except
for empty registers after a pop. DOS loop 21.6 (softfloat) / 10.6 (patch
05) / 2.9 ns per op; XP Super PI 1M on the Air 9:49 → 1:57. **Switch:**
`x87-fast`. **Test:** `tools/x87-guest-test.py`. **Drop:** upstream float
ops in TCG, or an upstream rewrite of the x87 translator. **On 11.1:** the eight ops are `TCGOutOp` descriptors (`TCGOutOpBinary`, `TCGOutOpUnary`, and `TCGOutOpTernary` for fmsub, in `tcg/tcg.c`) behind `TCG_TARGET_F64`, x86-64 gated at run time on AVX + FMA; the shadow has its own scratch temps (`x87s_t32`/`x87s_t64`, 11.0 removed `tmp2_i32`/`tmp1_i64`), the third `insn_start` word carries its state, and FXCH and `fst st(i)` mirror 11.1.2's tag-word and C1 fixes. It carries 07's and 09's hunks on the shadow.

### 07-upstream-x87-helper-fixes
**Dropped in M21** (QEMU 11.1). Upstream since 11.0 and 11.1.1 (`cf10af6c70`, `1621cc4971`). Its hunk on patch 06's inline compare moves into 06.

### 08-upstream-i386-decoder-fixes
**Dropped in M21** (QEMU 11.1). All seven fixes upstream by 11.1.1.

### 09-upstream-i386-rep-string
**Dropped in M21** (QEMU 11.1). Upstream since 10.0. Its `x87-shadow.c.inc` hunk moves into 06.

### 10-embed-api
Meson builds `shared_library('qemu-embed-<target>')` per system target
from the existing static library plus `embed/libqemu_embed.c`,
`embedaudio.c`, `embedfx.c` and `mglcntx_embed.c` (epoxy and gbm when
found), with an ld64 export list (`embed/libqemu_embed.symbols`) because
QEMU's plugin `-exported_symbols_list` hides everything else on macOS. On
Windows it also compiles `mglcntx_mingw.c`'s WGL half into the emulators
alone (patch 31). It links the target's stubs archive (`target_stubs`)
as the emulators do; without it the aarch64 library on macOS leaves
KVM's symbols undefined. The API is doc 11. **Drop:** an upstream embed API.

### 11-sse-inline-tcg
SSE/SSE2 float arithmetic inline on the host FPU (doc 16) when MXCSR is
round-to-nearest, no FTZ/DAZ, all exceptions masked and PE already sticky
(TB flag bit 31). Packed ops run on the vector unit (new TCG `fadd/fsub/
fmul/fdiv/fsqrt_vec`, `fmin/fmax_vec`, `fcmp_vec`, which map straight to
`VMINPS`/`VCMPPS` on x86-64), scalar ops in general registers. NaN, inf,
overflow, underflow and divide-by-zero take the helper out of line, and
`ldmxcsr`/`fxrstor`/`xrstor` end the TB. Packed 7.5–12×, scalar 3.4–3.9×.
**Switch:** `sse-fast`. **Test:** `tools/sse-guest-test.py` (546,425
lines identical on/off). **Drop:** upstream float ops in TCG. **On 11.1:** a denormal operand sends the instruction to its slow block, since 10.1's softfloat raises DE on one (`57df511180`; the SSE battery's MXCSR caught it), checked only for registers not already known clean in the TB, and the checks use signed compares (doc 16), which keeps the SSE bench at 9.2's times; the scalar ops are `TCGOutOp` descriptors as in 06, the vector ones stay on the vector path; `float_exception_flags` is a bit-field since 11.1, read as the struct's first 16 bits.

### 12-simd-inline-tcg
MMX and SSE integer and permutation instructions inline (doc 16):
shuffles, unpacks, packs, `pmulhw`, `pmaddwd`, `pavg`, `psadbw`, shifts
by register, `pshufw`, and the MMX entry as three stores. New TCG vector
ops `tbl_vec` (byte table lookup), `mulsh/muluh_vec` and
`ssnarrow/usnarrow_vec` (x86-64 only; aarch64 keeps a SWAR fallback).
Legacy encodings only. MMX chain 4.0× on aarch64, 2.1× on x86-64.
**Switch:** `simd-fast`. **Test:** `tools/sse-guest-test.py`. **Drop:**
upstream gvec permutes and narrowing ops. Regenerate it after any change
to 11 (shared files).

### 13-perfmap-darwin
`-perfmap` and `tcg/perf.c` on every host, not Linux only (`/tmp/perf-
<pid>.map`, one line per translated guest instruction).
`tools/tcg-profile.sh` maps macOS `sample` hits through it. `-jitdump`
stays Linux-only (it needs `mmap` and `flockfile`); on Windows it says it
is unavailable. **Drop:** upstream drops the `CONFIG_LINUX` gate.

### 14-jit-wx-state
macOS flips the `MAP_JIT` buffer between writable and executable per
thread; QEMU made that call before every TB run and around every patch or
translation (12 % of Super PI's vCPU thread). A per-thread state makes
the call only on a change (Super PI 1M 1:36 → 1:25). The state starts
*unknown* on purpose: the main thread is in execute mode when
`tcg_prologue_init` asks for write, a vCPU thread is not, and a wrong
initial state faults on the prologue store and spins at 100 % with QMP
never answering. **Drop:**
upstream tracks the state.

### 15-tb-invalidate-fast
Four cuts to TB invalidation on guest writes:

- The DMA path no longer rounds the first page's range down to the page
  start, which made every XP guest, idle too, retranslate the vAPIC ROM's
  TPR stubs thousands of times a second (they sit below the state the
  APIC writes per interrupt). 11.1.2 still has it.
- A per-page code map (patch 35) lets a write that misses every TB skip
  the page collection and list walk.
- A write that cannot hit a TB shared with a neighbouring page runs under
  the page's own lock.
- No whole jump-cache flush per invalidated `CF_PCREL` TB.

**Switch:** `tb-invalidate-fast` (patch 29). **Test:**
`tools/smc-guest-test.py`. **Drop:** upstream clamps the first page's
range and keeps per-page code ranges.

### 16-tlb-floor
`CPU_TLB_DYN_MIN_BITS`/`DEFAULT_BITS` 6/8 → 12. The direct-mapped
softmmu TLB is resized at every flush from the entries used since the
last; at XP's 450 flushes a second it sat at 64–256 entries where live
pages collide (Moto Racer: 6 million victim swaps a second, slow
path 48 % of the vCPU → 1.3 %). Cost: 128 KiB per mmu index, cleared per
flush. **Switch:** `tlb-floor` (patch 29, read per resize). **Drop:**
upstream's resize policy counts conflicts, or a set-associative TLB.

### 17-rep-fast
REP MOVS / STOS as a host `memcpy`/`memmove`/`memset` per page run. With
at least 8 elements left, `helper_rep_movs_fast`/`_stos_fast` take the
run that stays inside the current source and destination pages, probe
each once without faulting (filling the TLB, marking dirty and
invalidating TBs as the stores would; MMIO, watchpoints and unmapped pages
are refused), copy, and return the count done. Anything else falls into
the per-element loop, capped at 15 iterations per entry. The helper
writes nothing to `env`, so a longjmp out of the probe restarts the
instruction cleanly. Not emitted under TF, single-step or icount.
MOVSD/STOSD 2.1 → 0.07 ns per element. **Switch:** `rep-fast`.
**Test:** `tools/rep-guest-test.py` (536 cases against a Python model).
**Drop:** upstream grows a rep fast path.

### 18-smc-same-value
A store that leaves a code page's bytes unchanged invalidates no TB. The
store slow path compares the bytes with memory before `notdirty_write`
and skips the invalidation when equal (dirty bits still set; 16-byte,
probe and atomic stores still invalidate). Exact by construction.
Era software renderers patch their span loops' immediates per span, and
94 % of Moto Racer's ~700,000 code-page stores a second rewrote the value
already there; its race went 7.3 → 21.7 fps. **Switch:**
`smc-same-value`. **Test:** `tools/smc-guest-test.py`. **Drop:** upstream
takes it (worth sending). **On 11.1:** a not-dirty store can land on an
MMIO page, whose `haddr` is no host pointer, so the comparison refuses
`TLB_MMIO` pages (a Win98 boot segfaulted in it once in three).

### 19-tls-hot-paths
Thread-local reads off the TCG hot paths, which on macOS are calls into
dyld's `_tlv_get_addr` (8.8 % of Moto Racer's vCPU → 2.5 %).
`notdirty_write` uses `_rcu_locked` dirty-bitmap helpers instead of five
nested RCU lock pairs per store to a code page (`cpu_exec` already holds
the read section; `cpu_exec_step_atomic` now takes one too), and the
`tcg.c` allocators read `tcg_ctx` once and pass it down. The remaining
2.5 % is left on purpose: the `tcgv_*_arg` read in each `tcg_gen_op*`
wrapper (removing it needs a context-taking twin of every `tcg_gen_opN`),
`cpu_tb_exec`'s per-thread JIT state (a per-CPU one is wrong under
round-robin with several vCPUs), and `tcg-op-ldst.c`. **Switch:**
`tls-hot-paths` (patch 29), the RCU half only; the `tcg_ctx` half has no
reachable behaviour. **Drop:** the RCU part is worth sending upstream;
the `tcg_ctx` part matters only where TLS is a call. **On 11.1:** the dirty-bitmap API is out of line in `system/physmem.c`, where the `_rcu_locked` twins now live (`include/system/physmem.h`).

### 20-embed-audio
Registers the player's `embed` audiodev: the QAPI enum/union entry,
`audio_template.h`'s per-direction case, and `audio/audio.c`'s
`audio_create_pdos` case (without it a NULL pdo segfaults), and since
11.1 the `audio_get_pdo_out`/`_in` cases (without them, `abort()`). The
backend itself is the QOM type `audio-embed` in `embed/embedaudio.c`,
which is how 11.1 finds `-audiodev embed`. **Drop:** with 10.

### 20-inline-lookup
The jump-cache probe of indirect branches (`ret`, `call *`, `jmp *`, a
jump leaving its page) as TCG ops instead of a call to
`helper_lookup_tb_ptr` (~70 host instructions, 13.9 % of 7-Zip's vCPU).
`translator_lookup_and_goto_ptr` computes pc, cs_base and flags the way
`x86_get_tb_cpu_state()` (`target/i386/tcg/tcg-cpu.c`) does and folds every mismatch (pc, cs_base,
flags, cflags, breakpoints, single-step) into one word **branch-free**,
since a `brcond` ends a TCG block and spills every temp. One `goto_ptr`
takes the TB or the epilogue, where the main loop's lookup fills the
cache. These stay on the helper: `CF_NO_GOTO_PTR`, exec/nochain logging,
`one-insn-per-tb`, 32-bit hosts, the x86-64 target. 7-Zip compress +12 %,
decompress +7 %. Any new TB flag must be built here too (patch 37).
**Switch:** `inline-lookup`. **Drop:** upstream grows a generic inline
probe with a per-target state hook. **On 11.1:** `translator.c` is built once per mode, so the probe addresses `CPUState` as `offsetof(CPUState, f) - sizeof(CPUState)` and tests `singlestep_flags & SSTEP_ENABLE`.

### 21-pinned-regs
**Dropped in M21** (QEMU 11.1). Not ported (user decision, 2026-10-01): it was off and unoffered, and 10.1's TCG backend rewrite would make the port a rewrite. Doc 18 keeps the design.

### 22-upstream-apic-reset-cpuid
**A Win98 guest that restarts freezes on its first frame.** Win98 turns
its local APIC off through `IA32_APIC_BASE`, which rightly clears
`CPUID.01H:EDX.APIC`. `apic_reset_common()` restores the enable bit but
not the feature bit, so the next POST finds no APIC, SeaBIOS skips
`smp_setup()` and never sets LINT0 to ExtINT, and every i8259 interrupt
is dropped at the masked LVT0: a spin at 100 % behind a blinking caret
(`info pic`: `irr` set, `isr=00`; `info lapic`: `LVT0 masked`). The fix
records the CPU model's APIC bit at realize and restores it on reset,
leaving `-cpu …,-apic` alone. Reproduces on stock QEMU 11.1.0; `apic_reset_common()` is unchanged in
11.1.2. **Test:**
`tools/win98-reboot-test.sh`. **Drop:** upstream takes it (worth sending).

### 23-upstream-dsound-option
`--disable-dsound` was a no-op: the guard (unchanged in 11.1.2) `if not
get_option('dsound').auto() or …` is true for *disabled* too, so every
Windows build compiled `dsoundaudio.c` and linked `-lole32 -ldxguid`.
Disabled now skips the block. **Test:** the
`no-optionals` check (QAPI's `AUDIODEV_DRIVER_DSOUND` absent). **Drop:**
upstream fixes the guard.

### 24-soft-immediates
A block whose own code the guest keeps patching reads those operands from
the code bytes at run time. `accel/tcg/tb-softimm.c` counts, per physical
address (hashed multiplicatively), the writes that throw a block away.
Past four, the next translation emits its immediates and displacements as
host loads of the guest's code bytes, so the guest's store *is* the
update; a write inside the byte ranges read that way invalidates
nothing. A block thrown away four times anyway goes back to constants. Covered: group-1 ALU ops, MOV, IMUL3, memory
displacements, and the imm8 count of every shift and rotate (not RCL/RCR
on 8/16-bit, not SHLD/SHRD). Jump targets, ports and SSE lane selectors
stay constants. An emitter that picks a shortcut from the immediate's
value must not take it for a field read at run time: 11.1's ADC and SBB
turn an immediate 0 into "add the carry" and drop the operand, so a block
translated while the field was 0 ignored every later patch (Moto Racer's
race drew wrong on 11.1; `soft_imm_op()` turns the shortcut off). Needs a little-endian host with unaligned loads and a
single vCPU; covers only the block's first page. Moto Racer's race
41 → 58 fps (TB invalidations 36,500/s → 1/s); Blood's corridor
9.4 → 131 fps. Each page remembers up to eight byte ranges a write was
absorbed in, emptied whenever its TB list changes, so a repeated write
skips the walk over every block on the page (with Win98's lazy FPU
switching up to three TB-flags copies of each, CR0.TS/MP): Blood spent
30-40 % of QEMU in that walk, ~118 M TB visits a second, and went 119 →
164-182 fps on 11.1 with the cache. **Switch:** `soft-imm`. **Test:** `tools/smc-guest-test.py`,
which also requires four cases' fields to have been *absorbed*: a right
answer does not prove the block survived its patches (a `pc >> 2` hash
once let two blocks two bytes apart share a counter and compute right
while never absorbing). **Drop:** upstream invalidates and retranslates;
worth proposing once a second guest confirms it.

### 25-upstream-sb16-reset-irq
QEMU's `sb16` raised IRQ 5 in three places no driver could lower it
again, and on the edge-triggered PIC a held line swallows every later
interrupt. A DSP reset during auto-init DMA pulsed it (latched unowned),
and a silence block (DSP 0x80) raised it without the status bit the
driver's read clears. A reset now also cancels a pending silence block. Symptom: Duke
Nukem 3D's SETUP played its sound test once, then "Playback failed,
possibly due to an invalid or conflicting IRQ" (doc 20 §5.2). **Test:**
the `sb16-irq` check. **Drop:** upstream fixes it.

### 26-usb-gamepad
Builds `hw/usb/dev-gamepad.c` (overlay), a USB HID gamepad with two
sticks as X/Y and Z/Rz, an 8-way hat with a null state, twelve buttons
and a six-byte report. QEMU has no gamepad or axis input event. It is its
own file because every machine already has a `usb-tablet` on
`dev-hid.c`. Built under `CONFIG_USB_HID`. The host drives
it with **absolute state**, not events (`usb_gamepad_set_state()` from the
embed shim, embed API v8 `qemu_embed_pad_state`), so the next update
corrects a dropped one; a second instance is refused. XP, 98 SE and Me use
their in-box HID stack (98 SE asks for its source files once). Doc
`docs/tracks/m13-gamepads.md`. **Test:** the `pad`, `pad-guest-xp` and
`pad-guest-98` checks, `tools/hid-descriptor-check.py`. **Drop:** never.

### 27-gameport
Builds `hw/input/gameport.c` (overlay), the analog joystick port at
0x200–0x207 QEMU never had, DOS's only way to a controller. A write arms
four RC one-shots (`t = 24.2 µs + 0.011 × R µs` over a 0–100 kΩ pot); a
read compares deadlines on `QEMU_CLOCK_VIRTUAL`, so there is no timer.
Its own `CONFIG_GAMEPORT`, a standalone ISA device independent of the
sound card. Fed from `qemu_embed_pad_state`; the d-pad drives the first
stick's axes to their ends, a DOS game's only way to read one. Win9x
games read the USB pad through DirectInput and winmm, so no one installs
"Standard Game Port".
**Test:** the `pad` and `pad-guest` checks (`tools/pad-guest-test.py`).
**Drop:** never.

### 28-upstream-vga-chain4-dirty
`vga_mem_writeb`'s chain-4 branch stores at `(addr << 2) | plane` but
marks `addr` dirty after doubleword-mode shifting. Every write marks the
first quarter of VRAM, and in mode 13h nothing below scanline 51 is
redrawn. Only the Cirrus routes chain-4 writes through this function
(`-vga std` maps a RAM alias): Duke Nukem 3D at 320×200 was wrong on
`-vga cirrus`, clean on std. **Test:**
`tools/vga-dirty-guest-test.py`. **Drop:** upstream fixes it.

### 29-optimization-switches
Gives patches 15, 16 and 19 their switches (`tb-invalidate-fast`,
`tlb-floor`, `tls-hot-paths`), so "every optimization off" really is;
without them a fault was once blamed away from these three. Under `tls-hot-paths=off` both RCU
branches must set the same dirty bits, which makes the off branch patch
19's oracle. **Test:** the `optimizations` check. **Drop:** never; it is
what makes the queue bisectable.

### 30-3dfx-ui-vtable
qemu-3dfx's eleven UI entry points (`mesa_*`, `glide_*`) dispatch through
a `QemuFxUiOps` table (`ui/fxui.c`) any frontend can register; the embed
library registers its window-less provider (`embed/embedfx.c`). With no
provider, contexts are refused and the VM keeps running. SDL's half is
gone with `--disable-sdl`. The `glide_*` entries are unreferenced, since
the qemu-3dfx port builds no `hw/3dfx`; they stay so this patch keeps
applying as one piece. **Drop:** upstream qemu-3dfx grows a provider
seam.

### 31-mesa-ctx-weak
`hw/mesa/mglcntx_linux.c`'s exports are weak, so `embed/mglcntx_embed.c`
overrides them inside libqemu-embed while `qemu-system-i386` keeps the
native backend (GLX on Linux; patch 70's refusing backend on macOS). The
embed backend is EGL surfaceless + pbuffer on Linux, a drawable-less CGL
context with an FBO on macOS. A COFF weak external is not an ELF weak
definition, so on Windows `mglcntx_mingw.c` is split instead: its WGL
backend sits behind `MESAGL_WGL_BACKEND` and patch 10 compiles it a
second time into the emulators alone. **Drop:** per-consumer backend
selection in meson.

### 32-mesa-setfunc
`MesaGLSetFunc(fenum, fn)` swaps one guest-dispatch entry; the macOS
embed backend redirects `glBindFramebuffer(…, 0)` to its stand-in FBO.
**Drop:** upstream exposes the table.

### 34-pit-overdue-irq
**A DOS game's clock ran at twice real time.** `irq_timer` raises the
IRQ 0 edge at counter 0's wrap a main-loop wakeup late, and a guest
reading the PIT in a tight loop sees the counter wrapped with the tick
not yet counted. DOS Quake's `Sys_FloatTime` counts that as a whole
period twice (`QCLOCK.COM`: every tick a 55 ms backward step, 200 %).
Every PIT port access now first delivers overdue transitions in order,
and the `IN`/`OUT` ends its TB so the interrupt is taken before the next
instruction. **Switch:** `-global isa-pit.overdue-irq=off`. **Test:** the
`pit-guest` check. **Drop:** upstream delivers the edge on access.

### 35-tb-code-map
Patch 15's per-page byte range of code becomes a 64-bit map, one bit per
64-byte chunk, set for every chunk a TB touches. A Win9x module is code
at both ends and data between, so the range made every data write walk
the page's TB list (twice, with patch 24): 57 % of QEMU in 3DMark 99,
invalidating nothing. A write to a chunk with no bit now returns first. 3DMark 99 3334 → 5894. **Switch:**
`tb-invalidate-fast`. **Test:** `tools/smc-guest-test.py`, the DOS
batteries. **Drop:** upstream keeps a per-page code map.

### 36-sse-load-vector
A 16-byte SSE memory operand was loaded and stored to `env` as two
8-byte halves, so the next vector load could not be store-forwarded and
stalled. The pair is now assembled in the vector
unit (patch 12's `SIMD_TBL_MASK64LO`) and written with one `st_vec`, with
the same access, alignment check and fault. CPU 3DMarks +16 %.
**Switch:** `sse-fast` (with `TCG_TARGET_HAS_v128`). **Test:**
`tools/sse-guest-test.py`. **Drop:** upstream assembles an i128 into a
vector.

### 37-x87-pe-sticky
A TB translated with PE already sticky (`TB_FLAG_X87_PE`, bit 2) drops the
residual computation and the `fpus` update per op. The guard re-checks
PE at run time (a chained TB is not looked up again), and `fclex`,
`fninit`, `fldenv`, `frstor` and `fnsave` leave sticky mode for the rest
of their TB. Patch 20's inline lookup must carry the bit; without it,
epilogue exits cost a quarter of the frame rate. **Switch:** `x87-fast`.
**Test:** `tools/x87-guest-test.py`. **Drop:** upstream has no x87 shadow
path.

### 38-lookup-known-flags
Patch 20's inline lookup rebuilt the x87 mode, SSE mode and x87 PE bits
from `env` on every indirect jump (~22 of ~70 host instructions). Each
can only change at an instruction that ends the TB, so the leaving TB's
own bits are a constant OR. A bit set during the TB is emitted as 0,
which picks the exact variant until the next full lookup: slower, never
wrong. hflags and eflags are still loaded. CPU 3DMarks +5 %. **Switch:**
`inline-lookup`. **Drop:** with 20.

### 39-vec-allsign
New TCG op `vec_allsign_i32` (zero iff every byte of a vector has its top
bit set): `vpmovmskb` + `xor` on x86-64, `cmlt`/`uminv`/`umov`/`eor` on
aarch64. Patch 11's per-op lane check becomes that op and a `brcond`
instead of a round trip of the mask through `env->sses_scratch`; a
backend without the op keeps the round trip. **Switch:** `sse-fast`.
**Drop:** upstream grows a vector-test op.

### 40-d3dpt-device
Adds the `hw/d3dpt` meson subdir. The overlay carries `d3dpt_vga.c`, the
`d3dpt-vga` PCI adapter of the XP and 9x display drivers (docs 15 and
19): a stdvga core, a register BAR and 128 MiB of VRAM whose top 64 MiB
is the Direct3D command window, and `d3dpt_exec_load.c`, which opens the
executor library. Until M16 step 7 (2026-09-27) the patch also put a
SysBus Direct3D device on the pc machine for the retired guest DLLs
(doc 14); patch 74's context changed with it. **Drop:** never.

### 41-disas-context-uninit
QEMU builds with `-ftrivial-auto-var-init=zero`, and
`x86_translate_code`'s `DisasContext` is ~13.6 KB since patch 06's
slow blocks: a memset per translation (8.7 GB in one 3DMark 99
run). The patch opts out (`__attribute__((uninitialized))`); the
translator initialises what it reads and `x87s_new_slow` clears each slow
block it hands out. **Drop:** never while
06's slow blocks live in `DisasContext` (upstream's is small, so it has
no reason to mark it; 11.1's `QEMU_UNINITIALIZED` could replace the
attribute).

### 42-jump-cache-keep
A TLB flush no longer empties the jump cache. An entry carries the
cache's generation in the pc word's high half, a flush bumps the
generation, and `tb_lookup()` re-validates a stale entry against the pc's
current mapping and re-stamps it. The cache is 65,536 entries instead of
4,096. Win98's VMM writes the same CR3 2,400 times a second. The
generation bump returns early on a CPU with no jump cache, as upstream's
clear does: without TCG (qtest, KVM) `loadvm`'s `tlb_flush` still lands
there, and the i386 QEMU crashed on it (2026-10-01, the `tpm-qtest`
check on a Mac). **Switch:** `jump-cache-keep`. **Drop:** upstream keys its jump cache by
physical page. **On 11.1:** whether the cache carries a generation is a run-time `target_long_bits() <= 32` (`TARGET_LONG_BITS` is poisoned in code built once per mode): `qemu-system-i386` keeps it, `qemu-system-x86_64` the clear.

### 43-eob-chain
A block ending without a jump (`mov ds/es`, `sti`, `mov ss`, `popf`,
`iret`, `sysenter`, an x87 or MXCSR control-word change) always went back
to the main loop, five round trips per VxD call on Win98. It now chains
through the inline lookup when `cpu->interrupt_request` is zero, and a
block ending on a control-word change rebuilds the lookup's mode bits
from `env` (patch 38's constants are wrong there; MSVC's `_ftol` hit
that). Main-loop entries 12.3 M → 5.7 M per 10 s. **Switch:**
`eob-chain`. **Drop:** upstream chains these.

### 44-tlb-retire
A CR3 write no longer forgets every translation. `tlb_flush_retiring()`
moves the entries filled since the last flush into a per-mmu-index
retired table and clears only those. A miss probes the retired table and
reuses an entry when the target says it still holds
(`TCGCPUOps.tlb_retired_reusable`; on i386 CR3, mode, A20, SMM, PKRU/PKRS
and every page-table entry the walk read are unchanged). Any other flush
drops the tables, `invlpg` its page. 95 % of Win98's refills are reused;
within noise on the Ryzen, kept for hosts where a walk is not cheap. The
filled-slot list must be `uint32_t`: a `uint16_t` wrapped past 65,536
entries and killed Win98 a few seconds into `SETUP.EXE`, a different
victim each time. A flush for any other reason must drop the table's
*contents*, not only its flag: the next retiring flush set the flag back
and revived every older entry, and Windows XP's VESA mode change on the
inbox VGA driver (a VGA window topology flush, then the int10 call's own
`mov cr3`) ended black or in an empty text mode (2026-09-24;
`tools/xp-driver-test.sh <image> vesa`). `info jit` prints refills
and reuses. **Switch:** `tlb-retire`. **Drop:** upstream's TLB keeps
state across CR3 writes. **On 11.1:** the hook is in `include/accel/tcg/cpu-ops.h`, the page-walk record in `target/i386/tcg/system/excp_helper.c`, and `info jit`'s counters in `accel/tcg/tcg-stats.c`.

### 45-x87-prec24-f32
At PC=24 (Direct3D's setting, a 3D game's whole frame) the shadows are
binary32 in their own globals (`cpu_x87_ss[]`), and with PE sticky an op
is one `addss`/`mulss`/`divss`/`sqrtss` plus a range check, because
correctly rounded binary32 is the x87's PC=24 result while the exponent
fits. Overflow, underflow and the lowest binade (where binary32 rounds up
into it and the x87's wider exponent does not) take the slow path; with
PE undecided the binary64 path runs. CPU 3DMarks 16295 → 16899. The
battery sweeps every control word a second time with PE set, because
`fninit` before every case had kept the sticky variants from ever running.
**Switch:** `x87-fast`. **Test:** `tools/x87-guest-test.py`. **Drop:**
upstream has no x87 shadow path.

### 46-darwin-strchrnul
**Dropped in M21** (QEMU 11.1). Upstream since 10.1 (`a5b30be534`: the check includes `<string.h>`, and our `-Werror=unguarded-availability-new` makes it honour the deployment target). To confirm on the Air in step 4: no `HAVE_STRCHRNUL` in `config-host.h` at the macOS 12 floor.

### 47-x87-pc64-as-53
**The one inexact switch, off by default.** Code at PC=64 has no host
type with a 64-bit mantissa and paid a softfloat helper per op.
`x87-pc64-as-53=on` makes `update_fp_status` treat PC=64 as PC=53, so
patches 05 and 06 apply. `fnstcw` still returns the guest's word; results
differ from an x87 in the mantissa's last 11 bits, and the launcher
labels it "not exact" (doc 13). 3DMark2001 SE's Lobby 35.2 → 50.3 fps.
**Switch:** `x87-pc64-as-53`. **Test:** the `optimizations` check.
**Drop:** never.

### 48-x87-pc64-inline
x87 at PC=64 inline and exact (doc 13). The fourth x87 mode keeps the
stack as the x80 values themselves (mantissas in i64 globals, sign and
exponent in i32 ones), so loads, stores, `fild` and compares are inline
and `+ − × ÷` call pure helpers (`TCG_CALL_NO_RWG_SE`) doing 128-bit
integer arithmetic rounded nearest-even, with overflow, tininess,
denormals and NaNs sent to the slow block. `fsqrt`/`frndint` are not
inlined in this mode. Lobby 35.2 → 39.7 fps. **Switch:** `x87-fast`.
**Test:** `tools/x87-guest-test.py` (PC=64 control words, exact ties).
**Drop:** upstream has no x87 shadow path.

### 49-x87-pc64-inline-mul
Patch 48's `fmul` (a `mulu2_i64` product, normalized, rounded and packed
as TCG ops) and `fst m32` inline, with no helper call. Add, subtract,
divide, `fst m64` and `fist` stay calls (an inline add would compute both
add and subtract to stay label-free). Lobby 39.7 → 44.2 fps. **Switch:**
`x87-fast`. **Test:** `tools/x87-guest-test.py`. **Drop:** upstream has
no x87 shadow path.

### 50-cdimage-block-driver
Builds the `cdimage` block driver (doc 17 §5.2): meson option
`libdisc_dir` (where `liblibdisc.a` is; `configure-qemu.sh` passes
`target/release`), the `libdisc` dependency with the staticlib's
per-platform link libraries (an `if/elif`, since meson forbids chained
ternaries), `CONFIG_CDIMAGE`, and `block/cdimage.c` (overlay). `-cdrom
x.cue`/`.ccd` probe to `cdimage`; a plain `.iso` stays on `raw`.
Snapshots and migration with a cdimage medium are unsupported. **Drop:**
never, or an upstream cdimage.

### 51-atapi-disc-model
`hw/ide/atapi.c` asks `cdimage_disc()` on every command; NULL is the
stock path byte for byte (doc 17 §5.3–5.4). With a disc model: verified
READ(10/12), READ CD / READ CD MSF over the full MMC-3 field table, TOC,
SUB-CHANNEL, DISC INFORMATION, GET CONFIGURATION, mode pages 2A and 0E,
MODE SELECT(10), and CD-DA through `-device ide-cd,audiodev=<id>` (PLAY
AUDIO, PAUSE/RESUME, STOP PLAY/SCAN and the stop half of START STOP UNIT,
which is how XP's `mcicda` stops), or a position at 75 sectors/s without
one; INQUIRY from `model=`. New IDE fields are not migrated.
`CDIMAGE_TRACE=1` logs packets, replies and sense. On 11.1 the PIO
read path reads a whole DRQ burst at once (`1e4ab5af46`); the model
fills the burst's sectors synchronously and shares the completion half
(`cd_read_sector_done`), and CD-DA opens its voice on the drive's
`AudioBackend`. **Test:**
`tools/atapi-guest-test.py`, `tools/xp-cdimage-test.sh`. **Drop:** never.

### 52-atapi-disc-shelf
A vendor ATAPI opcode (0xD0) on `ide-cd` that lists the host's disc shelf
and loads or ejects from it, so the guest's `CDSHELF` swaps discs without
the launcher (doc 07). The CD-ROM drive is the one device DOS, Win98 and
XP can all send a raw command to, so no guest driver is needed.
`shelf=<file>` is a `<label>\t<path>` line file the launcher writes; a
drive without it answers ILLEGAL REQUEST. The opcode is `CONDDATA`
because LOAD/EJECT through SPTI or ASPI leave the byte count at zero. A
disc the host cannot open is refused with 02/3A up front. The medium
change runs from a bottom half, so the tray moves after the command
returns, as on a real drive. Which entry is in the drive is read off the
medium itself on every listing (`cdimage_medium_path`, by inode), so the
bundle's boot disc and a disc the launcher inserted over QMP are marked
like one the guest loaded. Protocol `cdshelf/cdshelf_proto.h` (bump
`CDSHELF_PROTO_VERSION` on change). **Test:** `tools/atapi-guest-test.py`,
`tools/cdshelf-guest-test.sh`. **Drop:** never.

### 53-atapi-dvd-profile
A cdimage medium longer than an 80-minute CD (`CD_MAX_SECTORS`) reports
as a DVD-ROM (current profile, feature `0x001f`, mode page 2A's DVD read
bit), because past 99:59:74 an MSF has no address to give. With a CD in
the tray the bytes are unchanged. For folder discs (doc 17 §2.1).
**Test:** `BIG=1 tools/dirdisc-guest-test.sh`. **Drop:** never.

### 54-atapi-audio-seek-stop
**On Win9x a seek is the stop.** `mcicda` sends one PLAY AUDIO MSF and two
SEEKs for a whole play/pause/stop session, so a SEEK now ends playback
(data reads do not: Win98's CDFS re-reads the volume descriptors
throughout a play). Position replies no longer fall back to the last data
sector read after a stop. Symptom: on Win98 the disc played on behind a
stopped MCI (doc 17 §5.4). **Test:** `tools/cdaudio-guest-test.sh`.
**Drop:** never.

### 55-atapi-audio-read-error
A host I/O error (`LIBDISC_EIO`) on a CD audio sector plays 2352 bytes
of silence and reads on, as a real drive does. Before, one transient
failure (an image on a network share) stopped the music with status 0x14
until the game asked for another track. A data sector in the range or a vanished medium still
stops, with a warning. **Test:** the `atapi-read-error` check
(`tools/read-error-inject.c`). **Drop:** never.

### 56-atapi-medium-type
The mode parameter header's medium type (byte 2 of every MODE SENSE
reply) says what the drive holds: 01h data, 02h audio, 03h both, from the
libdisc track list; 01h on a plain image; 70h with no medium; 71h with the
tray open. Upstream writes 70h always, which MMC-1 defines as "door
closed, no disc present". Windows and Linux never read it; OAKCDROM.SYS
and UIDE turn it into bit 11 of the MSCDEX device status, "no disc in
drive", and Mortal Kombat 3 checks that bit before it starts, so on a DOS
boot it refused the disc it had just read its directory from (2026-09-24;
the same disc passed in a Win98 DOS box, where CDFS answers). **Test:**
`tools/atapi-guest-test.py` (page 2A's byte 2 is 03h on the mixed test
disc). **Drop:** if upstream reports the medium.

### 60-opl3-mpu401-devices
Builds the music devices (doc 20 §1, §5): meson option `libsynth_dir`,
the `libsynth` dependency, `CONFIG_LIBSYNTH`, and `hw/audio/opl3.c` +
`mpu401.c` (overlay). `opl3` is a YMF262 at 0x388 and, with `sbbase=`, at
a Sound Blaster's 2x0–2x3 and 2x8/2x9, its timers on the virtual clock
(AdLib detection reads them). `mpu401` is UART mode with `synth=gm|mt32`
and **no interrupt unless `irq=` asks** (doc 20 §5.1: IRQ 2/9 is the ACPI
SCI on PIIX4, and an unacknowledgeable ACK triple-faulted Win98). Both
print a 5 s activity line; neither has vmstate. **Test:** the `libsynth`
and `music` checks. **Drop:** never, or an upstream MPU-401.

### 61-sb16-mixer-volumes
QEMU's `sb16` stored the CT1745 mixer's volumes and applied none, so
Windows' sliders did nothing and effects over CD music clipped. Master ×
voice now scales the SB16's voice (reapplied after `audio_be_open_out`), the
SB Pro registers mirror both ways, and a small registry in the audio core
(`audio_mixin_attach`/`_detach`/`_set_volume`, in `audio/audio-be.c`;
each entry keeps its voice's `AudioBackend`, which 11.1's volume call
needs) lets `opl3` and `ide-cd`'s CD audio take the FM and CD levels and output switches. Reset is 0 dB
with CD on, so a DOS game that never programs the mixer sounds as
before; a machine without an SB16 plays every input at unity. **Test:**
the `sb-mixer` check; `CDVOL=` in `tools/audio-glitch-test.py cd`.
**Drop:** an upstream sb16 mixer.

### 62-voodoo2-device
`subdir('hw/voodoo')` and nothing else. `-device voodoo2` (doc 21) is the
overlay: 86Box's Voodoo emulation verbatim (commit in `voodoo/86box/UPSTREAM`),
a shim of 86Box's platform headers, and a QEMU PCI
device. **Test:** the `voodoo-guest*` checks. **Drop:** never, or when
86Box grows a QEMU device.

### 63-jit-buffer-near-helpers
On macOS the TCG code buffer is reserved within 2 GiB of QEMU's image
when the image loads (a constructor in `tcg/region.c`, before `main()`
and guest RAM; 1 GiB, else 512 or 256 MiB), so every helper call is a
near `BL` or `ADRP` sequence. It had landed 8 GiB away in about a third
of launches, helper-heavy code running 35–45 % slower with the same
binary (doc 22 §5.0); an mmap hint at TCG init is too late. Darwin only. `QEMU_JIT_DEBUG=1` prints every try. No switch (it runs before the
command line); the A/B is a pristine build. **Drop:** upstream places the
buffer near the text.

### 64-voodoo2-dither-sub-recompilers
86Box's x86-64 and ARM64 rasterizer code generators now subtract the
dither from a blend read-back under `fbzMode` bit 19, as its interpreter
does; without it a colour blended onto itself drifted on every pass
(doc 21 §9). An overlay patch. **Switch:** `-device
voodoo2,dither-sub=off` turns it off on both paths; `recompiler=off` is
the A/B. **Test:** the `voodoo-guest` check's dither phase. **Drop:**
upstream 86Box fixes it and `scripts/sync-86box-voodoo.sh` brings it in.

### 65-pit-reinject
**A 1 kHz guest clock ran at the rate of the host's wakeups**, 6 % slow on
a Windows host's 15.6 ms timer, which played MIDI slow. Transitions that
came due while the main loop slept were raised back to back, which the
edge-triggered 8259 sees as one interrupt. Now an edge that finds IRQ 0
already requested is owed, and `pic_intack` raises one owed tick as a
fresh edge after the ISR is entered, paced to at most one per half period
(a burst on every acknowledge nests under Win98's timer handler and took
the VMM down after a long vCPU stall; a pacing timer merges with the
regular edge). The debt is capped at a quarter second of ticks, a new
count drops it, and IRQ 0 masked at the 8259 owes nothing. **Switch:**
`-global isa-pit.reinject=off`. **Test:** the `pit-guest` check's rate
phase (`tools/wait-granularity.c` rounds waits to 15.6 ms). **Drop:**
upstream reinjects coalesced PIT ticks.

### 66-passthrough-hides-cursor
While another card has the monitor (`graphic_hw_passthrough`: the Voodoo
2, the 3D frontend), the 2D adapter's hardware cursor is reported hidden.
`qemu_console_set_mouse` (9.2's `dpy_mouse_set`) publishes through `dpy_mouse_publish`, which answers
hidden in pass-through and republishes when that changes, so Windows'
arrow no longer draws over a full-screen Glide game. Trace event
`dpy_mouse_publish`. **Test:** the `voodoo-guest-d3dpt` check. **Drop:**
upstream has pass-through.

### 67-x87x-arith-call-shape
Patch 48's PC=64 helper takes four arguments (the two sign|exponent
words as one) and divides with `udiv_qrnnd` instead of `__udivti3`.
Under the Windows x64 ABI a fifth argument and 128-bit operands go
through memory, and the helper cost 10.8 % of the vCPU there against
4.6 % on Linux. **Test:** `tools/x87-guest-test.py`. **Drop:** with 48.

### 68-windows-clang
**Dropped in M21** (QEMU 11.1). Upstream since 10.0 (`8f5a4cfc7e` removed `gcc_struct`, so clang has nothing to refuse). To confirm in step 4 with the Windows cross build and its checks.

### 69-mkvenv-file-uri
**Dropped in M21** (QEMU 11.1). Upstream since 11.0 (`587f4a1805` passes a plain path to `--find-links`). To confirm in step 4 in MSYS2.

### 70-mesa-darwin-no-xquartz
**The macOS build needs no XQuartz.** qemu-3dfx's Mesa backend on macOS
was GLX on XQuartz, unused since SDL went.
On Darwin `hw/mesa/mglcntx_linux.c` is now a weak backend that asks the
provider hook (so the "no 3D provider" warning still appears), answers
the window-ready poll and the pixel-format queries, and refuses the
context. It has the same symbols as the GLX one (checked with `nm`) and
OpenGL.framework's `dllname`. Linux is unchanged. **Drop:** upstream
qemu-3dfx drops GLX on Darwin.

### 71-voodoo2-packet3-packed-color
A command-FIFO triangle packet's packed colour word (bit 28) is read
whenever either the RGB or the alpha parameter is named; 86Box took it
only under the RGB bit. Glide sends a vertex with iterated alpha over a
constant colour with the packed bit set and RGB clear, so that vertex
left a word unread and every later header was read mid-parameters.
Fixed in both consumers in `vid_voodoo_fifo.c` and in
`voodoo/voodoo2.c`'s packet walk, which warns once when it meets one. An
overlay patch (doc 21). **Drop:** upstream 86Box fixes it.

### 72-voodoo2-fifo-order
86Box's FIFO thread runs its memory FIFO (LFB and texture writes) and the
command ring in the order the guest wrote them. Each entry carries the
ring's write pointer at queue time (`cmdfifo_mark`) and runs once the
ring has been consumed that far, and the ring loop yields when the
memory FIFO's head is due, with no wait anywhere. Draining one and then
the other made a HUD written through the LFB flash (after the swap) or
vanish under the world geometry (before it) (doc 21 §13). An overlay
patch. **Test:** the ordering phase of
`tools/voodoo-guest-test.py` exercises it, but only a game under TCG,
where the rasterizer is behind, tells the two orders apart. **Drop:**
upstream 86Box fixes it.

### 73-vga-vram-prebacked
`vga_common_init` accepts a `vram` region its owner has already backed
and allocates one only when it has no size (a wrong size is refused).
`d3dpt-vga` backs it from the Wine executor's shared file
(`memory_region_init_ram_from_fd`) when the executor runs in another
process (ADR-018, doc 14), so the guest's VRAM and command window are the
bytes that process maps. No change for any other VGA. **Drop:** never.

### 74-no-glidept
**Dropped in M21** (QEMU 11.1). Folded into the qemu-3dfx port, which carries only the OpenGL half.

### 75-tpm-libtpms
The libtpms TPM backend, `-tpmdev libtpms,id=…,state=<file>`: a TPM 2.0
inside QEMU's process for Windows 11 (track M20), where QEMU's own
`emulator` backend talks to swtpm in a second process. The backend is
ours (`tpm/qemu/tpm_libtpms.c`, overlaid); the patch adds the `libtpms`
meson feature (`--enable-libtpms`; it asks pkg-config for libcrypto by
name too, since libtpms's `.pc` names only `-ltpms` and ours is static),
`CONFIG_TPM_LIBTPMS`, the `libtpms` `TpmType` with its `state` option in
QAPI, and `info tpm`'s line for it. The TPM's permanent state is the one
file, replaced atomically; snapshots carry the permanent and volatile
state, and `loadvm` writes the snapshot's permanent state back to the
file. libtpms and libcrypto come static and hidden from
`scripts/build-deps.sh` on Linux and macOS (`QEMU_DEPS=system` leaves
the feature on auto). Everything is behind `CONFIG_TPM`, which QEMU
refuses on a Windows host (still in 11.1.2, `have_tpm`), so the Windows build has no TPM yet.
**Test:** `tools/tpm-qtest.py` (the `tpm-qtest` host check): a fresh
TPM, a restart on the same file and a savevm / loadvm round trip
through `tpm-crb`'s registers under qtest; `tools/win11-spike.py boot`
with `TPM=libtpms` for Windows 11. **Drop:** never (upstream QEMU has no
in-process TPM). **On 11.1:** the backend includes `system/` headers (11.1
renamed `sysemu/`) and its `class_init` takes `const void *`; the patch
applies with offsets. `tpm-qtest` passes on 11.1 (Linux, 2026-10-02).

### 76-hvf-arm-macos12
Arm HVF on the macOS 12 floor (track M20 step 4). QEMU's Arm
Hypervisor.framework accelerator (9.2 and 11.1.2) sizes the VM's IPA space with the VM
configuration calls macOS 13 added, unguarded, so `aarch64-softmmu` (the
Windows 11 on Arm target, built on Arm hosts) failed the floor's
`-Werror=unguarded-availability-new`. Each call now runs under
`__builtin_available(macOS 13, *)`; on 12 the VM is made with no
configuration (a 36-bit IPA space) and one that needs more is refused.
**Test:** the build on the floor; a Windows 11 on Arm boot under HVF.
The macOS 12 path is unrun (no macOS 12 host with HVF here).
**Drop:** when the floor is macOS 13 or later, or upstream guards it.
**On 11.1:** `hvf_arch_vm_create` also sets up nested virtualization and
the in-kernel GIC (both macOS 15); the configuration path moved whole into
`hvf_arm_vm_create_config` (marked macOS 13), and before 13 a VM asking
for either is refused too. Compiles at the macOS 12 floor on the Air
(2026-10-04).

### 77-arm-target-no-era-devices
The era's devices stay out of a target with no ISA bus (track M20 step
4). `aarch64-softmmu`, Windows 11 on Arm's QEMU on an Arm host, is the
first target of ours that is not a PC: OPL3 and the MPU-401 (patch 60)
now also need `CONFIG_ISA_BUS`, and libqemu-embed compiles
`embed/mglcntx_embed.c` (the `hw/mesa` backend) and `embed/embedfx.c`
(its UI provider, registered under `TARGET_I386`) only for the x86
targets, where `hw/mesa` is built. `embed/libqemu_embed.c` itself calls
the gameport only under `CONFIG_GAMEPORT`. **Test:** the Arm build links;
the x86 targets are unchanged (`scripts/test.sh host`). **Drop:** with
patches 60 and 10, or when they say the same.

### 78-wav-header-live
QEMU's `wav` audiodev stores the RIFF and data lengths after every write.
Since 11.0 every open voice holds a reference on its audio backend and
devices are not unrealized at exit, so `audio_cleanup()` never finalizes
a backend a device uses and `wav_fini_out()` never patches the header:
every capture had both lengths 0, which readers take as empty (the
`music` check read "silent", `sb-mixer` "not a WAVE file"). Stock
`adlib` shows it too. **Test:** the `music` and `sb-mixer` checks.
**Drop:** upstream finalizes audio backends at exit, or patches the
header as it goes.

### 79-guestfwd-unix
`guestfwd=tcp:<addr>:<port>-unix:<path>`: each TCP connection the guest
opens to addr:port becomes a new connection to the host's Unix socket,
through libslirp's `slirp_add_unix()` (track M23, doc 24 §2.1). It is how
the player's SMB server, libsmb, sees a Windows guest's `\\10.0.2.4\…`
as ordinary per-connection streams; a chardev target carries one
connection for the machine's life, and `cmd:` spawns a process per
connection. libslirp 4.9.5 has it on Unix only, so on Windows the rule
is refused until a `patches/deps/libslirp` patch turns on `AF_UNIX` there.
**Test:** `tools/win11-spike.py` with `SMB=` (docs/testing.md).
**Drop:** upstream QEMU gains a Unix target for `guestfwd`.

### 80-win32-foreign-thread-exit
On Windows, the thread-exit notifiers of a thread QEMU did not create run
when that thread exits, not at process exit. `qemu_thread_atexit_add()`
took any such thread for the process's main thread and queued its
notifiers (defer-call's, fdmon-poll's, the log's, the coroutine pool's,
all `__thread` variables) for `atexit`. libqemu-embed runs the main loop
on a thread the player creates, which ends after `qemu_embed_destroy()`,
so at exit the list was walked through freed TLS: a segfault in
`notifier_list_notify()` from the DLL's onexit table in about one exit
in twelve, with either player toolchain (track M22, doc 11). A foreign
thread's list now runs from a fiber-local storage callback; the main
thread, noted by a constructor, keeps `atexit`. POSIX already uses a
per-thread key destructor. **Test:** the player through
`PLAYER_QMP_EXEC='{"execute":"quit"}'` exits 0 every time (72 of 72 across
the GNU and MSVC winit players and `player-mitsuami`, 2026-10-03; about
one in twelve crashed before), and `test.sh`'s `companions-env`.
**Drop:** upstream gives foreign threads a per-thread exit list on
Windows.

### 81-vga-blank-surface-size
`vga_draw_blank()` blanks `last_scr_width` x `last_scr_height` of the
console's surface and trusted an allocated one to be that size. On a
Win98 boot through the embed library it was not: gdb at the fault showed
`last_scr` 720x400 (text) over an allocated 640x400 surface, and the last
row's `memset` ran 320 bytes past it. On Windows an allocated surface is
a file mapping that ends exactly there, so it faulted in msvcrt (the
`strncpy+1108` frame) in three boots of six with the MSVC-built player
(track M22); elsewhere the write lands silently on what follows. An
allocated surface of another size is now replaced, as a shared one
always was. How `last_scr` and the surface part is not yet known.
**Test:** six boots of `base98-br` on each player, none ending early.
**Drop:** upstream checks the surface's size in `vga_draw_blank()`.

### 82-hvf-unaligned-section
HVF tried to unmap a memory section that is not page aligned, and HVF
aborts on that (track M21, step 4 on the Mac). 11.1's `hvf_set_phys_mem()`
dropped 9.2's slot list and turns such a section into `hv_vm_unmap()`,
which refuses a range it never mapped (`HV_BAD_ARGUMENT`, an abort in
`assert_hvf_ok`). `tpm-tis-device`'s `tpm-ppi` RAM region is 1 KiB,
under Apple Silicon's 16 KiB page, so Windows 11 on Arm with its TPM
could not start on the Mac at all (found by M23; `win11-spike.py`'s
`TPM_PPI=off` was the workaround). Such a section is now left alone: it
was never mapped, and its accesses trap as before. **Test:** `ARCH=aarch64 tools/win11-spike.py boot`
with the PPI on (no `TPM_PPI=off`): Windows 11 on Arm at its desktop in
36.2 s and powered off clean on the Air (2026-10-04); before, QEMU
aborted before the firmware.
**Drop:** upstream skips unaligned sections in `hvf_set_phys_mem()`.

### 83-msvc-runtime
QEMU on Windows against MSVC's runtime instead of mingw's (`WIN_QEMU_CC=msvc`,
`docs/build-windows.md` "QEMU under MSVC"): MSYS2's clang targeting
`x86_64-pc-windows-msvc`, Visual Studio's headers, the UCRT and the static
C runtime, lld-link. Everything is inert in the mingw build (`host_msvc`,
`_MSC_VER`, `QEMU_ENUM_UNSIGNED` empty), except two changes that hold
for every build: four enum fields migrate through `VMSTATE_UINT32_ENUM`
(the same four bytes), and `accel_irqchip_begin_route_changes()` loses an
`inline` that left clang's MSVC mode no body to link. What it adds:
- `include/msvc/` (on the system include path for an MSVC compiler only)
  and `util/oslib-msvc.c`: the POSIX parts of mingw-w64 QEMU uses
  (`unistd.h`, `getopt_long` with glibc's argument permutation,
  `sys/time.h`, `dirent.h`, `libgen.h`, `clock_gettime`, `mkstemp`,
  `ssize_t`, `mode_t`, ...) and compiler-rt's 128-bit division
  (`__udivti3` and friends, x64 `divq` when the quotient fits).
- `setjmp` keeps mingw's `_setjmp(env, NULL)`, so a longjmp out of
  generated code never unwinds; the UCRT's `_setjmp` is called under
  another name, since MSVC's headers give it one parameter. Real
  `__try`/`__except` for mingw's `__try1`/`__except1`.
- **Enums.** MSVC's ABI makes every enum an `int`. An enum bit-field one
  bit short of its largest value reads back negative: `TCGTemp.kind:3`
  turned `TEMP_CONST` into -4 (`la_bb_end: code should not be reached`,
  `test.sh`'s `pad`), `TCGOp.opc:8` every opcode past 127, and softfloat's
  `float_status` its rounding and NaN rules; the Sound Blaster checks
  (`sb16-irq`, `sb-mixer`, `music`) and `dirdisc` failed with them. An
  enumerator past `INT_MAX` is truncated (559 of them, ~450 from
  `FIELD()`'s 64-bit masks). `QEMU_ENUM_UNSIGNED(type)` in
  `qemu/compiler.h` gives each such enum an unsigned underlying type; no
  compiler flag changes the rule (clang applies it with or without
  `-fms-compatibility`). The MSVC build makes `-Wbitfield-enum-conversion`,
  `-Wbitfield-constant-conversion` and `-Wmicrosoft-enum-value` errors, so
  a QEMU bump that adds one fails to build rather than to run; that is
  also how a rebase finds the new ones.
- meson: `host_msvc`, lld-link accepted beside ld.lld,
  `_USE_MATH_DEFINES`, `dxguid` for the D-Bus display, no `qemu-nbd`
  (POSIX threads), `libqemu-embed-*.dll` with mingw's `lib` prefix (the
  name the players import).
**Test:** `WIN_QEMU_CC=msvc scripts/test.sh all` matches the mingw build's
run (47 passed, the same 1 failed, 28 skipped, 2026-10-04); QEMU's unit
tests 99 of 99 under MSVC. **Drop:** upstream QEMU builds for MSVC's ABI
(it supports only mingw on Windows).
