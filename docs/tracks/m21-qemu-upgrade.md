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

Stop there and bring the table to the user. Steps 2 to 5 are the plan if
the user says go.

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
