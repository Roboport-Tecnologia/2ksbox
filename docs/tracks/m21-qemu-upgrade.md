# Track M21: QEMU 9.2 to 11.1

Opened 2026-10-01 (user: "upgrade to latest qemu version and just backport
the qemu-3dfx part"). The tree is QEMU v9.2.4 plus qemu-3dfx plus our 71
patches (`patches/qemu/README.md`). The target is the newest stable
release, **v11.1.2** on the day this was written. Re-check with `git
ls-remote --tags https://gitlab.com/qemu-project/qemu.git 'v11*'` before
starting.

Read `CLAUDE.md` and `docs/00-status.md` first: the track rules, "A build
belongs to one checkout", "Never run two TCG guests at once", and the
patch queue rules. This doc has the plan and the facts the plan rests
on. Write what you find into it as you go.

## Where this runs

- A worktree of its own on the branch `track/m21-qemu-upgrade` off
  `main`: `git worktree add ../2ksbox-m21 -b track/m21-qemu-upgrade main`,
  then `git submodule update --init` and `scripts/build.sh` there (~15
  minutes). **Never** configure, build or run against the main
  checkout's `build/`, and never point `QEMU_BIN` at it.
- Track M20 (Windows 11) runs KVM guests in the main checkout on
  `track/m20-win11`. Benchmarks need the machine quiet, so check `ps -C
  qemu-system-i386,qemu-system-x86_64 -o pid,args` before any timed run,
  and ask the user if anything is running.

## What makes this hard

1. **qemu-3dfx stops at 9.2.** Upstream (`kjliew/qemu-3dfx`, last commit
   2026-09-28) has `00-qemu92x-mesa-glide.patch` as its newest patch. No
   10.x or 11.x version exists, so we port it and own the port. It is
   757 lines over 11 files: `accel/kvm/kvm-all.c`, `hw/i386/pc.c`,
   `include/hw/i386/pc.h`, `include/sysemu/kvm.h`,
   `include/sysemu/whpx.h`, `include/ui/console.h`, `meson.build`,
   `system/vl.c`, `target/i386/whpx/whpx-all.c`, `ui/console.c`,
   `ui/sdl2.c`. QEMU 10 renamed `include/sysemu/` to `include/system/`,
   so every header hunk moves. Only the OpenGL half (`hw/mesa`) is
   built: the Glide pass-through went in ADR-020, and patch 74 keeps
   `hw/3dfx` out of the build. Find out whether the port can drop the
   `hw/3dfx` hunks entirely. `sign_commit` stamps both directories, and
   the guest's `OPENGL32.DLL` checks that stamp
   (`guest-tools/build-wrappers.sh`).
2. **Our queue is mostly TCG internals,** the code QEMU changes most
   between releases. Counting `+++` headers across the queue:
   `target/i386` 82, `accel/tcg` 36, `include/tcg` 16, `tcg/aarch64` 11,
   `tcg/i386` 9, `tcg/tcg.c` 6, `tcg/tcg-op.c` 4. That is the x87
   shadow (05, 06, 37, 45, 47 to 49, 67), SSE/SIMD (11, 12, 36, 39),
   lookup, TB and TLB work (15, 16, 18, 20-inline, 24, 35, 38, 42 to 44,
   63) and pinned registers (21). Each one carries a measured speed-up
   (doc 22, docs 13 and 16). A rebase that compiles and passes the
   batteries can still lose speed silently.
3. **Toolchain floors.** QEMU 11 may want newer meson, Python, GLib or
   macOS SDK APIs than 9.2. The community macOS build's floor is
   **macOS 12** (`scripts/macos-floor.sh`, `docs/build-macos.md` "The
   floor"), and QEMU only promises the two newest macOS releases. The
   Windows build uses clang through mingw (patch 68), the Intel Mac build
   runs under Rosetta, and `scripts/build-deps.sh` builds GLib and the
   other libraries from pinned sources. Any of these can block the
   upgrade by itself.

## Steps

### 1. The census (read-only, do this first)

The census says how big the job is before anyone commits to it. Its
deliverable is a table in this doc and a recommendation to the user.

- Clone v11.1.2 into a scratch directory, not the `qemu/` submodule.
- For each of the 71 patches, record:
  - **Upstreamed**: the patches named `*-upstream-*` (01, 07, 08, 09,
    22, 23, 25, 28) are backports. Find each upstream commit in 11.1's
    log by its subject. If it's there, the patch goes.
  - **Applies**: `git apply --check` against v11.1.2 with the overlays
    copied in the way `prepare-qemu.sh` lays them out, in queue order.
    Also try each patch alone, so one early failure doesn't hide the
    rest.
  - **Conflicts**: for a patch that does not apply, what moved upstream
    (renamed file, refactored function, removed API). Note the upstream
    commit that moved it.
  - **Obsolete**: something upstream now does the same job (a TCG fast
    path, a device fix). Say what, and how you'd measure that it's as
    good.
  - **Effort**: none / small / large.
- The same for qemu-3dfx's `00-qemu92x` patch, hunk by hunk, plus
  whether `hw/mesa`'s own sources compile against 11.1's headers.
- The overlays (`embed/`, `d3dpt/hw/`, `voodoo/`, `libsynth/qemu/`,
  `libdisc/qemu/`, `gamepad/qemu/`) are our code compiled inside QEMU.
  List the QEMU APIs they call that changed: memory regions, chardev,
  audio (QEMU's audio subsystem has been reworked), block driver
  callbacks, qdev properties, the `include/sysemu` rename.
- The toolchain floors from point 3 above: minimum meson, Python, GLib,
  macOS version and SDK, mingw/clang requirements in 11.1's docs and
  `configure`.
- Anything 11.1 removed that we use. Check `-icount ... align=on` (the
  DOS family), the i386 system target, `isa-pit` / `isa-vga` /
  `cirrus-vga`, SB16, the ISA machine types `pc` / `pc-i440fx`, and the
  machine version Win98 and XP bundles pin.

#### What the census found (2026-10-01)

Method: a full-history clone of QEMU in `/mnt/data2/david/work/qemu-m21`
at v11.1.2, laid out with our overlays the way `prepare-qemu.sh` does,
then `patch -p1 -f` for the qemu-3dfx patch and our queue in order, with
rejects counted per hunk. The same script on 9.2.4 applies all 823 hunks
clean, which is the control. Each failing hunk was then traced to the
upstream commit that moved it, and the overlays, `hw/mesa` and the
qemu-3dfx port were compiled against 11.1.2 in a scratch tree (gcc,
i386-softmmu, our configure flags).

**The count, queue order:** 823 hunks, 521 clean, 79 with fuzz, 177
failed, 46 on files that moved. 36 of 71 patches have no failed hunk.
Upstream moved 14,652 commits between the two tags.

**Verdict:** nothing blocks the upgrade. The work splits into three
sizes.

- **Drops.** 01, 07, 08 and 09 are upstream in full. 07 and 09 each carry
  one hunk on our `x87-shadow.c.inc`, which folds into 06. 46, 68 and 69
  look obsolete (upstream fixed the same thing another way) and each
  needs a check on its platform before it goes. 22, 23, 25 and 28 are
  not upstream and stay.
- **Large, about a week together.**
  - TCG's backend layer. In 10.1 upstream replaced `tcg_out_op` with one
    `TCGOutOp` descriptor per opcode and merged the `_i32`/`_i64`
    opcodes; 11.0 renamed `tcg/i386` to `tcg/x86_64`. The 19 scalar
    float ops of 06 and 11 (about 860 lines over two backends) must be
    redone in that form. 39's allsign op fits no existing class. The
    vector ops of 11 and 12 only move files. About 80 % of the x87/SSE
    work (3,500 lines on the target side) carries over with mechanical
    fixes: upstream x87 is still one helper per op, SSE float is still
    helpers, and TCG has no float ops of its own.
  - The audio rework (10.2 to 11.0): `QEMUSoundCard` gone, `AUD_*`
    renamed `audio_be_*`, a backend is now a QOM type. That hits
    `embed/embedaudio.c` (a rewrite as an `audio-embed` type, about 60
    lines), 61 (the mixer volumes, a redesign), the OPL3/MPU-401
    overlays (after 61) and 51's CD audio.
  - 51: 11.1.1 rewrote the ATAPI PIO read path (`1e4ab5af46`) and
    removed `cd_read_sector_sync()`. 53 to 55 follow 51.
- **Small, mechanical.** The accel/tcg patches (15, 18, 19, 20, 24, 29,
  35, 42, 44) are signature churn from accel/tcg being compiled once
  per mode: no `ArchCPU`, `TARGET_LONG_BITS` poisoned (42's gate
  becomes a runtime check), `TARGET_PAGE_*` runtime, `exec/exec-all.h`
  and `exec/ram_addr.h` dissolved. About 1.5 weeks with retesting. The
  device patches and overlays need the header moves (`sysemu` to
  `system`, `hw/*.h` to `hw/core/`, `qapi/qmp` to `qobject`), const
  `class_init`, no `DEFINE_PROP_END_OF_LIST`, and the 11.1 console
  rename (`dpy_*` to `qemu_console_*`, `gfx_update` returns bool,
  `405a42e365`). The embed library also moves its key input to Linux
  keycodes (`184c07600d`) and to `qemu_console_register_listener`.

**21 (pinned registers)** is a near-rewrite on the new backend. It is
off and unoffered (2026-09-16). The census recommends dropping it from
the upgrade; the user decides.

**Behaviour changes that apply cleanly and only guest tests catch:**

- 10.1 raises FPUS.DE and MXCSR.DE on a denormal input (`57df511180`).
  11's fast SSE path assumes denormals raise nothing, so a fast TB
  would leave DE clear where the helper now sets it. Gate on DE sticky
  or send denormal inputs to the slow block; check the x87 fld m32/m64
  gates too.
- 11.1.2 fixed FXCH (C1, tag word), FSTP and FXTRACT tag words
  (`e0e13147cb`, `18e276bc72`, `0df1fa1311`, `115f1213ae`). 06's inline
  FXCH and FST must mirror them or the x87 battery's on/off runs
  differ.
- The aarch64 `UMOV … imm5=0` SIGILL that 06 fixes is still in 11.1.2.

**qemu-3dfx.** 16 of 25 hunks clean, 2 fuzz, 5 failed, 2 on the moved
`sysemu` headers. Every Glide-only piece drops: the glidept parts of the
`pc.c`/`pc.h`/meson hunks (74 removes most already), the `glide_*`
prototypes, and all 7 `ui/sdl2.c` hunks (we build `--disable-sdl`, and
30's `ui/fxui.c` defines the same symbols). About 6 hunks need real
porting, all small: `whpx.h` (restructured around
`CONFIG_WHPX_IS_POSSIBLE`), `whpx_update_guest_pa_range` moving to
`accel/whpx/whpx-common.c` beside `whpx_set_phys_mem` (`4610fee324`;
WHPX is our Windows accelerator, so it can't drop), and the four
console hunks behind `graphic_hw_passthrough`. With them, all ten
`hw/mesa` files compile on 11.1.2 unchanged (`mglcntx_mingw.c` is
unchecked; it needs the WHPX hunk). `sign_commit` runs clean on 11.1.2.
It insists on stamping exactly three files, one in `hw/3dfx`, so
`prepare-qemu.sh` keeps copying `hw/3dfx` in unbuilt. The guest's
`OPENGL32.DLL` check is `hw/mesa/mesapt_mm.c` against `mglfuncs.h`
only.

**Overlays.** `cdimage.c`: one include (the block-driver callbacks are
unchanged). Gamepad, d3dpt and Voodoo 2: small (includes, console
renames, and in the Voodoo shim `cpu_get_phys_page_debug` to
`cpu_translate_for_debug` and `ldl_le_phys` to `address_space_ldl_le`).
`mglcntx_embed.c` must include `<epoxy/gl.h>` before `ui/console.h`
(`cc47123440`). Embed audio and libsynth: above.

**Toolchain floors.** Nothing blocks. Meson ≥ 1.5 (the venv brings
1.11.1), Python ≥ 3.9, GCC ≥ 10.4, Clang 10, GLib ≥ 2.66 (we pin
2.90.0), Rust still optional and off. On macOS only ARM HVF code carries
`__builtin_available(macOS 15.x)` guards; nothing on the i386 TCG path
raises the floor past 12. Windows now needs `pathcch` and
`synchronization`, both in mingw-w64. C++ is gnu++23, used only on
Windows. To handle in step 2:

- `configure-qemu.sh` passes `--disable-glusterfs`; gluster was removed
  in 11.1 and `configure` stops on the unknown option.
- `configure` now always installs a `tooling` venv group (setuptools,
  wheel, pip, qemu.qmp; `4e55bb4be5`). Offline builds (the Flatpak)
  must provide them.
- Consider `--disable-pvg` for the Intel build under Rosetta, where HVF
  is built for i386.

**Removed features.** None we use. `-icount … align=on`,
`qemu-system-i386`, SB16, AdLib, Cirrus, ISA VGA, the PIT, PIC, ATAPI
and usb-tablet are all there. The launcher and tools use the
unversioned `pc` machine; 11.1 keeps versions 4.1 to 11.1. Moving `pc`
from 9.2 to 11.1 turns on four CPU compat properties
(`pc_compat_10_0`), a small CPUID change under `-cpu pentium3`, and
changes `isa-cirrus-vga`'s vmstate. **Live snapshots saved under 9.2
may not load.** Either accept that, or pin `pc-i440fx-9.2` (present in
11.1, deprecated around 12.1) for existing bundles: a decision for the
user.

**Per patch** (Hunks: total; Fail: failed plus moved-file hunks in the
queue run; effort is the port's, not the retest's):

| Patch | Hunks | Fail | Verdict | Effort | Why |
|---|---|---|---|---|---|
| 00 3dfx-darwin-contextalpha | 1 |  | keep | none |  |
| 01 upstream-i386-lss-tb-exit-fix | 1 | 1 | drop | none | upstream `0f1d6606c2` (10.1) |
| 02 3dfx-sdl-optional | 1 |  | keep | none |  |
| 04 3dfx-graceful-no-display | 2 |  | keep | none |  |
| 05 x87-fast | 30 | 6 | keep | small | `old_flags` is `int` (`397ef415ca`); fcomi save/merge (`1621cc4971`) |
| 06 x87-inline-tcg | 61 | 22 | keep | large | backend ops must become `TCGOutOp`s (10.1); `tcg/i386` is `tcg/x86_64`; `tmp2_i32`/`tmp1_i64` gone (27 uses); mirror FXCH/FSTP/FXTRACT tag and C1 fixes; takes 07's and 09's shadow hunks; aarch64 UMOV fix still needed |
| 07 upstream-x87-helper-fixes | 8 | 7 | drop | none | upstream `cf10af6c70`, `1621cc4971`; its `x87-shadow.c.inc` hunk folds into 06 |
| 08 upstream-i386-decoder-fixes | 16 | 14 | drop | none | all 7 upstream (10.2 to 11.1.1) |
| 09 upstream-i386-rep-string | 31 | 29 | drop | none | upstream (10.0); its `x87-shadow.c.inc` hunk folds into 06 |
| 10 embed-api | 3 |  | keep | none | needs a link test |
| 11 sse-inline-tcg | 56 | 17 | keep | large | 11 scalar ops to `TCGOutOp`; vector ops move files; TB flags to `x86_get_tb_cpu_state`; **denormals now raise DE** (`57df511180`), so the fast path's premise needs a gate |
| 12 simd-inline-tcg | 40 | 10 | keep | small | vector ops only; HAS defines to `tcg-target-has.h` |
| 13 perfmap-darwin | 12 |  | keep | none |  |
| 14 jit-wx-state | 2 | 1 | keep | none | worth more: `78420b59f0` adds a W^X toggle per invalidation |
| 15 tb-invalidate-fast | 9 | 5 | keep | small | `4af02681ff` merged the `__locked` variants; `precise_smc` is runtime (`77ad412b32`); `cpu` argument added |
| 16 tlb-floor | 1 | 1 | keep | none | defines moved to `accel/tcg/tlb-bounds.h` (`3504f104ea`) |
| 17 rep-fast | 11 |  | keep | none | retest |
| 18 smc-same-value | 21 | 6 | keep | small | renames (`854cd16e31`, `aa60bdb700`) |
| 19 tls-hot-paths | 22 | 11 | keep | small | dirty API went out of line into `system/physmem.c` (`4db362f68c`); the twins move there |
| 20 embed-audio | 4 |  | keep | none | the patch applies; `embed/embedaudio.c` is a rewrite (see overlays) |
| 20 inline-lookup | 12 | 3 | keep | small | translator.c built once per mode (`41fed3c992`): no `ArchCPU` |
| 21 pinned-regs | 63 | 11 | ask | large | near-rewrite on the new backend; off and unoffered since 2026-09-16 |
| 22 upstream-apic-reset-cpuid | 3 |  | keep | none | not upstream; fuzz from `2bb73a332c` |
| 23 upstream-dsound-option | 1 |  | keep | none | not upstream |
| 24 soft-immediates | 32 | 13 | keep | small+ | `gen_lea_modrm_0` folded into `decode_modrm()` (`7e7d54fc60`); `exec/exec-all.h` gone |
| 25 upstream-sb16-reset-irq | 3 |  | keep | none | not upstream |
| 26 usb-gamepad | 1 |  | keep | none | overlay include moves |
| 27 gameport | 2 |  | keep | none | overlay include moves |
| 28 upstream-vga-chain4-dirty | 1 |  | keep | none | not upstream |
| 29 optimization-switches | 13 | 4 | keep | small | follows 15, 16, 19 |
| 30 3dfx-ui-vtable | 3 | 1 | keep | small | `ui/meson.build` reworked (`c28f118805`); console renames (`405a42e365`) |
| 31 mesa-ctx-weak | 18 |  | keep | none |  |
| 32 mesa-setfunc | 2 |  | keep | none |  |
| 34 pit-overdue-irq | 5 | 1 | keep | small | const `Property[]`, no `DEFINE_PROP_END_OF_LIST` |
| 35 tb-code-map | 5 | 2 | keep | small | follows 15 |
| 36 sse-load-vector | 3 |  | keep | none |  |
| 37 x87-pe-sticky | 21 | 2 | keep | small | TB flags moved to `tcg-cpu.c` |
| 38 lookup-known-flags | 2 |  | keep | none |  |
| 39 vec-allsign | 14 | 10 | keep | small | vector in, i32 out fits no `TCGOutOp` class: a new one |
| 40 d3dpt-device | 1 |  | keep | none | overlay small (see below) |
| 41 disas-context-uninit | 1 |  | keep | none | could use upstream's `QEMU_UNINITIALIZED` |
| 42 jump-cache-keep | 18 | 7 | keep | small | `TARGET_LONG_BITS` is poisoned in cpu-exec.c: the gate becomes runtime |
| 43 eob-chain | 11 | 1 | keep | none | context from 06/36/37 |
| 44 tlb-retire | 44 | 23 | keep | small to large | 8 files moved (`exec-all.h`, `tcg-cpu-ops.h`, monitor stats) |
| 45 x87-prec24-f32 | 42 |  | keep | none |  |
| 46 darwin-strchrnul | 1 | 1 | drop? | none | upstream `a5b30be534` (10.1); verify `HAVE_STRCHRNUL` is unset at the macOS 12 floor |
| 47 x87-pc64-as-53 | 3 |  | keep | none |  |
| 48 x87-pc64-inline | 34 | 1 | keep | small | 5 `tmp2_i32` uses |
| 49 x87-pc64-inline-mul | 2 |  | keep | none |  |
| 50 cdimage-block-driver | 5 |  | keep | none | overlay: one include |
| 51 atapi-disc-model | 33 | 2 | keep | large | PIO read path rewritten in 11.1.1 (`1e4ab5af46`); CD-DA on the new audio API |
| 52 atapi-disc-shelf | 7 | 1 | keep | small | Property list |
| 53 atapi-dvd-profile | 5 |  | keep | none | after 51 |
| 54 atapi-audio-seek-stop | 3 |  | keep | none | after 51 |
| 55 atapi-audio-read-error | 2 |  | keep | none | after 51 |
| 56 atapi-medium-type | 4 |  | keep | none |  |
| 60 opl3-mpu401-devices | 5 |  | keep | none | overlays medium (audio API) |
| 61 sb16-mixer-volumes | 13 | 3 | keep | large | `audio/audio.h` gone; volumes are `audio_be_set_volume_out*` (`d2b15ae407`): a redesign |
| 62 voodoo2-device | 1 |  | keep | none | overlay small (see below) |
| 63 jit-buffer-near-helpers | 2 |  | keep | none |  |
| 64 voodoo2-dither-sub-recompilers | 2 |  | keep | none |  |
| 65 pit-reinject | 12 | 1 | keep | small | with 34 |
| 66 passthrough-hides-cursor | 5 | 2 | keep | small | after the qemu-3dfx console hunks |
| 67 x87x-arith-call-shape | 7 |  | keep | none |  |
| 68 windows-clang | 2 | 2 | drop? | none | upstream removed `gcc_struct` (`8f5a4cfc7e`, 10.0); check our packed bitfield structs |
| 69 mkvenv-file-uri | 1 | 1 | drop? | none | upstream `587f4a1805` (11.0); verify in MSYS2 |
| 70 mesa-darwin-no-xquartz | 4 |  | keep | none |  |
| 71 voodoo2-packet3-packed-color | 2 |  | keep | none |  |
| 72 voodoo2-fifo-order | 11 |  | keep | none |  |
| 73 vga-vram-prebacked | 1 | 1 | keep | small | `memory_region_init_ram_nomigrate` removed (`787495878f`) |
| 74 no-glidept | 3 |  | keep | none |  |

Stop there and bring the table to the user. Steps 2 to 5 are the plan if
the user says go.

**The user's answers (2026-10-01):** go; drop 21; existing bundles stay
on `pc-i440fx-9.2`. The last landed first: a bundle now records its
board (`Machine::board`, `bundle::CURRENT_BOARD`), a bundle without one
gets `LEGACY_BOARD` (9.2's), and `CURRENT_BOARD` moves to
`pc-i440fx-11.1` with the submodule. The `hpet` check covers both.

### 2. The tree builds on 11.1 with no TCG patches

- Bump the `qemu` submodule to the tag, and change `prepare-qemu.sh`'s
  `9.2.*` check and its `PATCH=` line.
- qemu-3dfx: carry the ported patch ourselves. Where it lives (our queue,
  or a fork of qemu-3dfx) is a decision for the user. Record it as an
  ADR amendment to ADR-001.
- Port the non-TCG patches and the overlays: the embed API (10,
  20-embed-audio), the OpenGL pass-through's own patches (00, 02, 04,
  30, 31, 32, 66, 70, 74), the devices (26, 27, 34, 40, 50 to 56, 60 to
  62, 64, 65, 71 to 73), and the build and platform patches (13, 14, 29,
  41, 46, 68, 69). Patch 29 is the switch framework the TCG patches hang
  their `*-fast=off` controls on, so it comes over here.
- Gate: `scripts/test.sh host`, a Win98 and an XP guest boot, GLQuake on
  the OpenGL pass-through, and a Voodoo 2 title. The TCG fast paths are
  absent, so guests run slower; that's expected here.

#### Where step 2 stands

- The submodule is at v11.1.2. qemu-3dfx's patch is ported into
  `patches/qemu-3dfx/00-qemu111x-mesa.patch` (our tree, not a fork: the
  ADR-001 amendment), OpenGL half only, which folds 02 and 74.
- 35 patches ported: 00, 04, 10, 13, 14, 20-embed-audio, 22, 23, 25 to
  28, 30 to 32, 34, 40, 41, 50 to 56, 60 to 62, 64 to 66, 70 to 73, and
  one new one, 75 (11.0 never finalizes an audio backend a device uses,
  so the `wav` audiodev's header kept lengths of 0 and the `music` and
  `sb-mixer` checks read silence). The TCG patches wait in
  `patches/qemu-pending/` as their 9.2 versions.
  Dropped: 01, 07, 08, 09 (upstream), 02 and 74 (folded), 21 (user), and
  46, 68, 69 (obsolete upstream, to confirm on their platforms in step
  4). 29 moved to step 3: with no fast path in the tree it would add
  switches for nothing, and its context is 15, 16 and 19's.
- What needed more than context: 51's PIO path fills a whole DRQ burst
  from the disc model and shares the async callback's completion half;
  61's mixer-input table moved to `audio/audio-be.c` and keeps each
  voice's `AudioBackend`; `embed/embedaudio.c` is the QOM type
  `audio-embed`; 20 needs the `audio_get_pdo_out`/`_in` cases or
  `-audiodev embed` aborts; the embed library's keys go through Linux
  keycodes (11.1's input layer) and its listener through
  `qemu_console_register_listener`; `mglcntx_embed.c` includes epoxy
  before the khronos headers; the Voodoo shim uses
  `cpu_translate_for_debug` and `address_space_ldl_le`.
- The player never exited (SIGTERM, a QMP `quit`, a guest power-off
  alike): since 10.0 QEMU's exit notifiers take the BQL (`e7bc0204e5`),
  and they run in the player's `exit()`, on the main thread, while the
  `qemu` thread had ended still holding the BQL `qemu_init` gave it.
  `qemu_embed_destroy` now gives the BQL and the replay lock back after
  `qemu_cleanup`, as upstream's `qemu_default_main` does. It showed as
  `pad-guest-xp` hanging after it had passed: the script waits for the
  player it terminated.
- The gate (2026-10-01), on Linux:
  - `scripts/test.sh` (host): 42 pass, 5 fail. `x87-fast` and
    `optimizations` want the step 3 switches; `icons` and
    `exec-no-device` are the 9.2 baseline's; `package` fails because this
    worktree's `build/` is a symlink (`package-linux.sh` compares the
    stage path as spelled with the path the binary resolves).
  - `scripts/test.sh guest`: XP's checks all pass (cdimage, dirdisc,
    ddvm, the G9/G8/F9 scenes against native and the rig, pad-guest-xp).
    DOS: atapi, atapi-read-error, midi, vbe-palette, pad and the four
    Voodoo 2 runs pass; x87, rep, smc and sse want the step 3 switches;
    pit-guest fails as on the 9.2 baseline (the 15.6 ms waits at 13 %).
  - Win98, `base98-br` on `pc-i440fx-9.2` in the player: GLQuake's
    `timedemo demo1` on our OpenGL, 969 frames, 171.0 fps (the host's
    Radeon through Mesa); on 3dfx's MiniGL and the Voodoo 2, 48.9 fps.
    No 9.2 figure for either yet; step 3 measures both.
  - `tools/win98-game-test.sh` now boots `pc-i440fx-9.2` (`BOARD=`) and
    puts an `EXTRA` Voodoo 2 in the launcher's slot, 0x05. On 11.1's `pc`
    with the card elsewhere, Windows 98 found new hardware and stopped
    at "restart to finish", so RUN.BAT never ran.
- How the port was done, for step 3: a full-history QEMU clone at
  `/mnt/data2/david/work/qemu-m21`, branch `m21`, one commit per patch
  on top of an overlay commit (`hw/3dfx`, `hw/mesa`, `hw/voodoo/86box`,
  the overlay files that patches edit; the other overlays are untracked
  there), compiled in a scratch build dir, then exported as the queue
  with each 9.2 patch's description kept.

### 3. The TCG patches, one group at a time

In this order, each group with its own battery and its own benchmark A/B
against the 9.2 build (kept built in a second worktree):

1. x87: 05, 06, 07, 37, 45, 47, 48, 49, 67. `tools/x87-guest-test.py`
   and `x87-fast-test.c` must stay bit-exact; SSEBENCH's x87 ns/op.
2. SSE/SIMD: 11, 12, 36, 39. `tools/sse-guest-test.py`, SSEBENCH.
3. REP and SMC: 09, 17, 18, 24. `rep-guest-test.py`,
   `smc-guest-test.py`, Blood (`tools/w98-blood.sh`).
4. Lookup, TB and TLB: 15, 16, 19, 20-inline, 35, 38, 42, 43, 44, 63.
   3DMark 99 (`tools/w98-3dmark.sh`) and Quake II
   (`tools/w98-quake2.sh`), plus Moto Racer on XP
   (`tools/xp-moto-race.sh`).
5. Pinned registers (21) is off and unoffered (user, 2026-09-16). Port
   it last, or ask the user whether to drop it.

Each A/B uses the same image, the same machine and a quiet host, and
reports the 9.2 number beside the 11.1 one. A loss outside the run-to-run
noise doc 22 gives is a stop: find it before the next group.

#### Where step 3 stands

- Every TCG patch is ported (05, 06, 11, 12, 15 to 20-inline, 24, 29,
  35 to 39, 42 to 45, 47 to 49, 63, 67) and the queue is whole again:
  62 patches plus the qemu-3dfx port, `patches/qemu-pending/` empty.
  They were ported in filename order rather than group order, because
  the groups interleave (37 sits on 11, 36 and 20's context); the groups
  stay the unit of measurement.
- The TCG backend layer was rewritten for 10.1's per-opcode `TCGOutOp`
  descriptors: 06's eight binary64 ops, 11's eleven binary32 and int32
  conversion ops and 39's `vec_allsign_i32` are `TCGOutOpBinary`,
  `TCGOutOpUnary` and a new `TCGOutOpTernary` (fmsub) in both backends,
  dispatched in `tcg_reg_alloc_op` beside upstream's own, gated by
  `TCG_TARGET_F64` (a backend that has them) and on x86-64 at run time
  by AVX + FMA. 11's and 12's vector ops keep the old vector path
  (`tcg_out_vec_op`, `tcg_target_op_def`). The aarch64 half was checked
  with `clang --target=aarch64-linux-gnu -fsyntax-only` here; the Air
  builds and runs it in step 4.
- 11.1 behaviour the fast paths had to follow: since 10.1 softfloat
  raises DE on a denormal operand (`57df511180`), so 11's SSE fast path
  now sends an instruction with a denormal operand to its slow block (the
  helper raises DE exactly); rcp/rsqrt (whose helpers restore the flags)
  and the int32 conversions (no DE, as on the hardware) need no check.
  The SSE battery caught it (MXCSR 1fa2 against 1fa0). The checks first
  cost the whole SSE bench regression (packed 0.27 to 0.49 s, scalar 0.38
  to 0.60 s, clamp+cmp 0.27 to 0.38 s); they are now skipped for
  registers already known clean in the TB, and every vector check uses
  signed compares (doc 16), which brought all three back to 9.2's times
  on the Ryzen. 06's inline FXCH
  and `fst st(i)` mirror 11.1.2's tag-word and C1 fixes. 11.1 made
  `float_status.float_exception_flags` a 16-bit bit-field (`8c86fe2451`),
  so the TCG loads of it read the struct's first 16 bits, little-endian
  hosts only (a build assertion).
- Code compiled once per mode since 10.1: 42's jump-cache generation is
  a run-time `target_long_bits() <= 32` (i386 keeps the generation
  scheme, x86_64 the clear), 20's inline probe addresses `CPUState` as
  `offsetof(CPUState, f) - sizeof(CPUState)` and reads `singlestep_flags`,
  19's dirty-bitmap twins live in `system/physmem.c`, 44's hook is in
  `accel/tcg/cpu-ops.h` and its page-walk record in
  `target/i386/tcg/system/excp_helper.c`.

### 4. The other platforms

The macOS build on the Air (both the App Store and community builds, and
the Intel build under Rosetta), the Windows cross build
(`scripts/build-windows.sh`, `package-windows.sh`) with
`build/win/d3dpt-dp2-test.exe` on both `D3DPT_D3D9` values, the Linux
package and the Flatpak. Each packager's own checks must pass.

### 5. Merge

`scripts/test.sh all` green (or at the known failures in
`docs/00-status.md`), the benchmark table in this doc, the patch README
rewritten for 11.1 (every row's "when to drop it" re-checked), doc 22's
numbers re-measured or marked as 9.2's, and `CLAUDE.md`'s v9.2.4
mentions updated. The user hand-tests a game before the merge.

## Owns

`qemu` (the submodule pin), `scripts/prepare-qemu.sh`,
`scripts/configure-qemu.sh`, `patches/qemu/`, the qemu-3dfx port, and
this doc. Every other track's QEMU patch lands through this track's
order while it runs, so tell the user before touching a patch another
track owns (M14's Voodoo patches, M5's ATAPI patches, M20's TPM patch
when it exists).
