# Testing

The catalogue of test tools: the policy, the regression suite
(`scripts/test.sh`), how a guest run is driven, and every tool by area
with what it proves and how to run it. The design each tool guards is in
its design doc; the build that produces what the tools run is in
`docs/development.md`.

## Policy

- **Integration and end-to-end only, no unit tests.** A test exercises a
  real boundary (decoder + executor on a real batch, a guest program
  under TCG, a frame diffed against a golden BMP), never a function in
  isolation. No `#[cfg(test)]` modules, no per-function cases.
- When something starts working, add or extend a tool here and wire it
  into `scripts/test.sh`.
- **Run `scripts/test.sh all` before every commit that touches QEMU, the
  embed library, the D3D device, the display driver or the guest tools.**
- **Local only, by decision.** The suite needs the guest images and a
  GPU. CI (`.github/workflows/ci.yml`, manual trigger) will never run
  it; don't propose wiring it in.
- A control must fail only the check under test. A control built by hand
  uses the ISO's flags (msvcrt, `-march=pentium3`); a UCRT build never
  loads on XP and fails as `guest-run`.

## The suite: `scripts/test.sh`

```sh
scripts/test.sh            # host stage (~30 s): everything without a guest
scripts/test.sh guest      # the DOS batteries, the XP guest, then Win98 on the driver
scripts/test.sh all        # both (~2 min, ~7 min with the Win98 checks)
```

It expects `build/qemu`, `build/dxvk` and `build/d3dpt/libd3dpt_exec.*`
(`scripts/build.sh`) and compiles only the small tools in `tools/`.
Output goes to `build/test/`, one log per check, with a PASS / FAIL /
SKIP verdict each; a check that cannot run on this host (no x86, no
display, no image, no FreeDOS floppy yet) is a SKIP. A failed guest run
leaves the previous run's logs in `build/test/`, so read timestamps. A
change to anything a guest sees (a drive's behaviour, a device register)
needs `all`: a patch that broke `atapi-guest` once went unnoticed for
days behind a green `host`.

Environment: `WINXP_IMG` (`~/vms/winxp.qcow2`), `GUEST_ISO` (newest
`guest-tools/out/guest-tools-3dfx-*.iso`), `D3D_GOLDEN_BUDGET` (1200),
`WIN98_PAD_MACHINE`,
`WIN98_DX9_MACHINE` (`base98-br`: the launcher machine the Win98 checks
copy).

### Host-stage checks

| Check | What it proves |
|---|---|
| `x87-fast` | patch 05's x87 fast path equals the real x87 (`tools/x87-fast-test.c`; x86 hosts only) |
| `libdisc` | `discx selftest`: synthetic cue/bin, CCD, ISO; reads, EDC/ECC, Q synthesis, MMC responders (doc 17) |
| `cdimage` | the `cdimage` block driver (patch 50) through QEMU's block layer |
| `dirdisc` | a folder served as `isodir:` reads back identical through xorriso/bsdtar, qemu-img and SeaBIOS's ATAPI probe (M5g) |
| `dirshelf`, `shelforder`, `drive` | a folder on the disc shelf and on the boot drive; the shelf is one list in one order; the shelf's one drive: Insert and Eject set the boot disc, and on a running machine (a paused QEMU here) also swap the tray, which the card reads back through `query-block`, a folder disc included |
| `accel-choices` | the acceleration picker offers hardware acceleration only where the host has it (`launcherx --kvm`), checked on this host and under `bwrap` with a `/dev` that has no `/dev/kvm`; a machine already set to it keeps the entry (doc 07) |
| `machine-details` | what the machine window shows of a chosen machine (`launcherx --machine-details`): the family and state under its name, a group per page of the form with the form's labels (System, Storage, then the form's order), a disk by its file name, no Direct3D row on a machine without our adapter (doc 07) |
| `clone` | **Clone…** gives a machine with its own disk copy and snapshots, the original untouched; refused while the machine runs (doc 07); "same hard disk" boots the original's disk, copies none of it, warns, and goes ahead while the machine runs |
| `snapshot-tree` | the snapshot window's tree over a real qcow2 (doc 07): take, restore, take again gives siblings, not a line; a delete moves the branch up; a snapshot deleted or taken by hand is dropped from the record or shown at the top level; the clone carries the tree |
| `shader-defaults` | the first-run shader offer without a toolkit, and the library's default profile: the first download marks CRT Aperture, a machine on "(default)" resolves to it and follows it when moved, a named profile does not, a second run of the starters never moves it, clearing it and a deleted profile both mean no default |
| `mitsuami` | the launcher's real window, driven through its probes (`LAUNCHER_SCREEN`, `LAUNCHER_SHOT`; on a private Broadway display on Linux, for a moment on the desktop on a Mac): the machine window draws, `create:xp:Probe box` submits a fresh form and the machine lands in the library, `firstrun:no` asks with the shared model's headline and writes the marker, About draws (needs a built `launcher-mitsuami`; Linux also `gtk4-broadwayd`) |
| `host-check` | `launcherx --host-check`: no Vulkan reported unavailable, software Vulkan warned not refused, loader and floor always named (ADR-013/018) |
| `optimizations` | the form's emulation switches land on `-cpu` / `-accel tcg` and our QEMU accepts all fourteen flipped; an untouched machine emits nothing; `pinned-regs` never reaches the command line |
| `pointer` | "Seamless mouse": Windows gets `-usb -device usb-tablet`, DOS neither; toggling removes/restores both |
| `voodoo2` | the form's Voodoo 2 switch (doc 21) |
| `extra-args` | "Extra QEMU arguments": quoting round-trips, an open quote is refused, a `-global` reaches `info qtree` |
| `pad` | the gamepad on every family; `tools/hid-descriptor-check.py` on the shipped descriptor and two mutated copies that must fail |
| `family-other` | the Other family: `-vga std`, RTL8139 at `0x03`, ES1370 at `0x04`, no tablet |
| `hpet` | Win98 is `hpet=off` (no `hpet` in `info qtree`), XP still has one; both on a versioned `pc-i440fx-` board, and a bundle with no `board` gets 9.2's |
| `display-adapter` | each family's adapter choices and default; a foreign adapter refused; our adapter replaced, not added beside |
| `d3d9` | the Direct3D picker: a new machine writes nothing (`auto`); `dxvk`/`system` reach only `d3dpt-vga`, whose `info qtree` shows the property |
| `libsynth` | `synthx selftest`: AdLib detection, a 440 Hz FM note, the shipped bank through the MPU-401 path, running status; `--roms` adds the CM-32L (doc 20 §7) |
| `music` | the sound-card and music pickers into a real QEMU; the monitor writes the ports and the note must be in QEMU's own `wav` |
| `sb-mixer` | the SB16's FM, master and SB Pro FM volumes at −12 dB come out 12 dB down (patch 61) |
| `sb16-irq` | a DSP reset over auto-init DMA raises no IRQ 5 edge; each silence block exactly one (patch 25) |
| `bios-date` | F000:FFF5 as a guest reads it is ≥ 12/01/99, Win98's `ACPICheckDate` (doc 06) |
| `machine-map` | `info mtree` of a PC machine: the `mesapt` pass-through region is there, and no Glide one (patch 74, ADR-020) and no SysBus `d3dpt` one (M16 step 7) |
| `win11-snapshots` | a new Windows 11 machine's offline snapshot (take, restore, delete through `launcherx --snapshots`) covers its firmware variable store and a copy of its TPM state, with a snapshot name that has a space; a clone copies the TPM, and `--clone --new-tpm` leaves it behind (M20) |
| `tpm-qtest` | the libtpms TPM backend under qtest: a fresh TPM, a restart on its state file, a savevm / loadvm round trip (`tools/tpm-qtest.py`, patch 75); needs `qemu-system-x86_64`, or `-i386` on a Mac |
| `no-optionals` | no disabled library (libpng and libjpeg among them) is linked, named in a binary, or present as a QAPI audio enumerator; on a Mac, no Homebrew path in any load command of `libqemu-embed`, `qemu-system-i386` or `qemu-img`, whose libraries are our own static builds (`build-macos.md` "The libraries") |
| `icons` | `scripts/gen-icons.sh --check` |
| `package` | `scripts/package-linux.sh --no-tar` (or `package-macos.sh --no-sign --no-dmg` on a Mac) |
| `package-x86_64` | on an Apple Silicon Mac that has made the Intel build (`scripts/build.sh --x86_64`): `package-macos.sh --x86_64 --no-sign --no-dmg`, the same staging and checks under Rosetta, every Mach-O required to be x86_64; skipped where that build does not exist |
| `capi` | `launcher-capi/examples/smoke.c`, a C front end over the shared models |
| `preview-anim` | the shader preview: a still preset is one picture at any frame, an animated one is not |
| `embed-3d` | `tools/embed-3d-test.c` (Linux) |
| `d3dpt-dp2` | `tools/d3dpt-dp2-test.cpp` |
| `exec-wine` | the same test through the Wine executor; its frame must equal the in-process one |
| `exec-no-device` | the dp2 test with both Vulkan loader variables at a missing file, so DXVK's constructor throws out of `Direct3DCreate9`; it must end in the test's own exit 77, never a signal (DXVK patch 09, the executor's once-per-library rule) |
| `crtcal` | `build/crtcal-render`: every calibration pattern's circle round on its tube |
| `mode-sweep` | `player --mode-sweep`: the display path without a guest |
| `player-mitsuami` | `tools/player-mitsuami-test.sh`: the mitsuami player (M22) on a private headless sway, the mode sweep through it, its test pattern read back off the compositor, a key reaching its surface; skipped unless `player-mitsuami` is built and sway, grim and wtype are there |
| `d3dgame9-nat`, `d3dfeat9-nat` | the reference scene / feature test natively on DXVK, against the rig golden; the guest stage's oracle |

### Guest-stage checks

First the DOS batteries, each a FreeDOS floppy under our QEMU. They need
nasm, mtools and the floppy (run `tools/x87-guest-test.py` once to fetch
it) and skip without them. Then XP (Linux; KVM when `/dev/kvm` exists):
`tools/xp-dx9-test.sh` makes a fresh overlay on `WINXP_IMG`, installs the
display driver from the newest guest-tools ISO (`xp-driver-test.sh
install`), and in a second boot runs DDVMTEST, D3DGAME9, D3DGAME8 and
D3DFEAT9 through XP's own `ddraw.dll` / `d3d9.dll` / `d3d8.dll` (M16 step
7; until then the Direct3D DLLs on Cirrus). Its log is
`build/test/xpdx9.log`, a run that did not finish is `guest-run`:

| Check | What it proves |
|---|---|
| `x87-guest`, `rep-guest`, `smc-guest`, `sse-guest` | the DOS CPU batteries |
| `atapi-guest`, `atapi-read-error` | the ATAPI battery, and with injected EIO |
| `midi-guest`, `pit-guest`, `vbe-palette` | music devices, the PIT, VBE 4F09h from a DOS guest |
| `voodoo-guest`, `-d3dpt`, `-mmiofifo`, `-undither` | the Voodoo 2 battery and its three variants |
| `pad-guest` | the gameport under the player (needs a display) |
| `guest-ddvm` | Windows' own `ddraw.dll` on the driver reports enough video memory for Vice City's launcher check (DDVMTEST) |
| `guest-G9=native`, `guest-G8=native` | D3DGAME9/8 pixel-identical to the native frame outside the HUD |
| `guest-G9~rig`, `guest-G8~rig` | the same frame within budget of the rig golden |
| `guest-F9=native`, `guest-F9-log=native` | D3DFEAT9 byte-identical to native, same query/getter lines |
| `guest-cdimage`, `guest-dirdisc` | `tools/xp-cdimage-test.sh` on a converted cue and on the same tree as a folder |
| `pad-guest-xp`, `pad-guest-98` | `tools/pad-guest-test.py xp` / `win98` (own boots) |
| `win98-dx9` | Win98 on the display driver through Microsoft's runtimes (M16 step 5), one boot under TCG of `tools/win98-dx9-test.sh`: D3DGAME9/8 pixel-identical to the native frame outside the HUD, D3DFEAT9 byte-identical with its lines (the A16B16G16R16F readback expected to fail, finding 35), the DX8 probes, SHTEST, CKTEST, EBTEST, DDTEST's sysmem blits; one line each in the log |
| `win98-winetest` | Wine's suites on Win98 (`tools/win98-winetest.sh`) against `reference/winetest/w98-driver.txt` |

The two Win98 checks run after the XP stage, on a raw copy of
`WIN98_DX9_MACHINE` kept in `build/test/w98.raw` (never the machine's
disk; delete the copy, or `FRESH=1`, after a run was killed), and skip
where that machine does not exist. About 5 minutes together.

## Driving a guest

What every guest tool below shares, and what to know before writing
another.

- **User images are read-only.** Boot a qcow2 overlay (`qemu-img create
  -f qcow2 -b <image> -F qcow2 <scratch>`), a raw copy or `snapshot=on`,
  never the image. While an overlay's QEMU runs, the backing image is
  locked ("Failed to get write lock"), so sequence those runs.
- **Waiting: `tools/guestwait.sh`**, sourced by every guest tool; no tool
  has a fixed boot wait. `gw_poke_until` knocks on the Run dialog until
  the guest says on COM1 that it ran something (the only proof a shell is
  there). `gw_wait_log` / `gw_wait_count` watch for a line our device
  wrote (two `d3dpt-vga: linear mode on` prove a restart).
  `gw_wait_quiet` watches QMP `query-blockstats` for machines with no
  serial line. `gw_wait_sock` / `gw_wait_exit` bound the ends.
  `BOOT_WAIT`, `WARMUP_WAIT` and `CMD_WAIT` are caps on giving up; every
  wait ends early if the guest exits (`GW_PID`).
- **`tools/qmpc.py`** drives a guest over an extra `-qmp
  unix:…,server,nowait`: keys, typing, screendumps.
- Never run a sleep-polling waiter beside a run: it starves the guest.
- The traps that read as the thing under test failing (the Win98 power
  button, two TCG guests at once, screendumps, the shell traps) are in
  `docs/00-status.md`, "Driving a guest headless".

### Getting things in and out

- **Files out of XP.** A raw FAT32 image as `-hdb` is E:. Make it with
  `truncate -s 64M`, `sfdisk` (one partition at 2048) and `mkfs.fat -F 32
  --offset 2048`; read it with `mcopy -i img@@1048576 ::/path out`. XP's
  lazy writer can hold a small write for minutes, so wait on COM1 and
  read the disk after shutdown. Guest programs log to `C:\2KSBOX`
  (`guestlog.h`); `set BOXLOG=E:\` puts the logs on the scratch disk.
- **When a guest freezes,** the guest DLLs' and drivers' lines in the
  QEMU log are the only reliable channel: they arrive through the
  device's DEBUG register, in order with the device's own (`d3dpt: guest:
  …`), while files on the scratch disk sit in the guest's write cache. A
  DLL log with no `DLL_PROCESS_DETACH` means the process was terminated
  or crashed at exit.
- **Frames.** A QMP `screendump` shows only the VGA surface, frozen
  while 3D presents. Use `PLAYER_DUMP_OUT` or `PLAYER_SHOT_EVERY` for 3D
  frames. `grim` hangs inside the agent sandbox.
- **A disc swap under a running guest** is QMP `blockdev-change-medium`
  on `ide1-cd0`. Copy-protection runs need the disc on its own channel
  (`ide.1`, where the player puts it); SafeDisc 2 refused FIFA 2002 as
  the boot disk's slave (doc 17 §2.6b).

### Writing a harness

- **mtools scratch disks.** `mformat` leaves the BPB's hidden-sectors
  field at 0, and with the partition at LBA 2048 XP mounts no E:, which
  reads as "the batch never ran". Pass `-H 2048` (`mkfs.fat --offset`
  sets it).
- **A probe that waits for a marker in a log deletes the log first**, or
  it reads the previous run's marker.
- **`cmd | grep -q` under `set -o pipefail`** fails when the producer
  gets SIGPIPE. Capture the output, then match.
- **`--print-args` re-split by a shell cannot carry a path with a
  space.** The launcher spawns an argv, so the limit is the harness's.
- **macOS** has no `timeout`, `truncate` or `du -sb`, `stat -f` means
  something else, and BSD `tr` refuses the guest's CP-850 bytes in a
  UTF-8 locale (`LC_ALL=C`).
- **bsdtar** normalizes names to NFD on macOS (`café.txt` from a folder
  disc shows as missing; xorriso round-trips the bytes, so `dirdisc`
  prefers it), keeps ISO 9660's read-only modes (make an old extraction
  writable before deleting it), and lists an ISO in extent order.
- **Assert the value, not the line.** `d3dfeat9`'s occlusion query must
  answer `0x00000000` with a non-zero pixel count; grepping for the line
  passed while the native oracle said `S_FALSE, 0 pixels`.

## CPU and TCG

| Tool | Proves / runs |
|---|---|
| `tools/x87-fast-test.c` | host oracle for `x87-fast` (above) |
| `tools/x87-guest-test.py` | a DOS x87 battery, identical with the fast paths on and off; `QEMU_TCG_OPTS=<switch>` runs this and the other DOS batteries under an accelerator switch; `x87-guest` |
| `tools/x87-unwind-test.asm` | the x87 shadows are materialised on a fault's unwind (qemu-user, patch 06); run line in its header |
| `tools/sse-guest-test.py` | every SSE/SSE2 float op over edge-value pairs, `sse-fast` on/off identical, plus the SSEBENCH ratio (patch 11, doc 16); `sse-guest` |
| `tools/rep-guest-test.py` | 536 `rep movs`/`stos` cases, `rep-fast` on/off identical and equal to a Python model (patch 17); `rep-guest` |
| `tools/smc-guest-test.py` | 18 self-modifying-code cases right under all four `smc-same-value` × `soft-imm` combinations; the soft-imm runs must also *absorb* the writes at cases N, O, P, R, since a right answer does not prove a block survived (patches 18, 24); `smc-guest` |
| `tools/smc-diff.py` | two captures of a guest code page disassembled and diffed: which instructions and fields a game patches (`build/venv-capstone/bin/python`) |
| `tools/pit-guest-test.py` | QCLOCK under FreeDOS: no backward step, every window 100 % (patch 34; `isa-pit.overdue-irq=off` is the control, reported not required; on the Air 540 backward steps / 200 % without the patch). Then PITRATE at 1 kHz timed by the host, also with QEMU's waits rounded to 15.6 ms by `tools/wait-granularity.c` (LD_PRELOAD, Linux); `isa-pit.reinject=off` is the 6 % control (patch 65). `PIT_SECS=`, `PIT_RATE=0`; `pit-guest` |
| `tools/dos-guest-test.py` | the DOS family end to end: bundle → `--print-args` → QEMU → FreeDOS; boots from floppy, a throttled machine forced to TCG, and a timed loop proves the CPU combo's rate (`-icount` needs `align=on`) |
| `tools/string-bench.py` | rep movs/stos/scas throughput for two QEMU binaries (patch 09) |
| `guest-tools/src/ssebench.c`, `tools/xp-ssebench.sh` | `SSEBENCH.EXE` ns/op for SSE and x87; the script runs it in XP per `-cpu` config |
| `tools/specbench/` | doc 22's benchmark tier (nbench, 7-Zip, Super PI, SSEBENCH) in XP, one boot per emulator configuration: `build-guest.sh`, `run.sh <image> all` (resumable, `--status`, `PAUSE=1`, ~10 min a configuration, never beside other load), `report.py --md`; `SPEC=1` adds the CINT2006 ancestors |
| `tools/tcg-profile.sh <image> <name> ['cmd']` | where the vCPU's time goes (macOS `sample` + `-perfmap`, `tools/tcg-profile.py`); `CDS=`, `VGA=d3dpt`, `QEMU_BIN=`, `FPS=`, `SECS=`, `WARM=`, `BOOT_WAIT=`, `KEYS=`, `MEM=`, `PERFMAP=0`, `DDFLAGS=` (`32` puts a title back on its own software renderer), `QEMU_EXTRA=`; second pass `DFILTER=` + `tools/tcg-hot.py` (mnemonics need `capstone`) |
| `tools/tcg-fps.py <sock> <s>` | a guest's VGA frame rate from distinct screendumps; blind to 3D-device frames |
| `tools/moto-watch.py` | a row per second of a running guest: TB invalidations and host code generated (`info jit` at 4 Hz), a screendump, the worst seconds kept. A fourth argument (`4:3`) cycles throttle / brake over the same QMP connection (QEMU serves one client). `WATCH_TRACE_LOG=` (pages retranslated per phase), `WATCH_MEMSAVE=` (for `smc-diff.py`), `WATCH_SAMPLE_PID=` / `_PHASE` / `_SECS` (macOS `sample`); usually run by `xp-moto-race.sh` |
| `tools/memtrace.c` | LD_PRELOAD counting QEMU's `memcpy`/`memmove`/`memset` calls of ≥ 4 KiB per return address (perf cannot unwind out of glibc's AVX loops), into `$MEMTRACE_OUT.<pid>`; used as a `QEMU_BIN=` wrapper (header). Linux |
| `tools/hvf-el1/`, `tools/hwmmu/` | M9's Hypervisor.framework EL1 probe and the hardware-MMU census (macOS; their READMEs, the M9 track) |

## Display, adapters and the display drivers

| Tool | Proves / runs |
|---|---|
| `target/release/player --mode-sweep <dir>` | every mode through mode analysis, geometry and a real CRT preset: aspect, integer vertical scale, the preset's parameters, the scanline count counted in the frame; `PLAYER_MODE_PARAMS=0` the control; `mode-sweep` |
| `tools/player-mitsuami-test.sh [out]` | the mitsuami player (`player-mitsuami/`, M22, built from its own directory) on a private headless sway (`WLR_BACKENDS=headless`), so nothing opens on the desktop: `--mode-sweep` through it (player-core's session driven from mitsuami's loop), the test pattern on its `GpuSurface` read back with `grim` (five or more bright colours across the window's middle row), and a `wtype` Windows key arriving as `MetaLeft` down and up (`PLAYER_INPUT_LOG`). The headless seat has no pointer, so nothing here clicks; a guest run's recipe is in `tracks/m22-mitsuami-player.md`; `player-mitsuami` |
| `build/crtcal-render <dir>` (`tools/crtcal-render.c`) | doc 09's eight calibration patterns (`guest-tools/src/crtcal.h`) at every era mode, circles round; `crtcal`. `TESTS\CRTCAL.EXE` shows them on a real tube, `TESTS\TEXTCAL.COM` the 720×400 text-mode ones, `player --shader <preset> --calib <bmp\|dir>` renders them through a preset at 3200×2400 |
| `tools/vga-dirty-guest-test.py [13h\|vesa] [std\|cirrus]` | the display sees what the guest wrote to video memory, page by page (patch 28): polls screendumps throughout (an idle display hides lost dirty bits), stripes `rep stosw` against a byte loop (`FLIP=1`), and the guest reads back its writes (`VERIFY_BAD=`) to separate a lost store from a missed redraw; `--refill`. Two traps that look like a QEMU bug: `c6_to_8()` expands the 6-bit DAC by replicating the low bit (33 reads back 135, not 132), and the VBE window granularity is 64 KiB on std, 16 KiB on the Cirrus (read it from the mode-info block). `VBEPAL=1 … vesa` is the `vbe-palette` check: our VGA BIOS's 4F09h (`patches/seabios/`, `scripts/build-vgabios.sh`, `firmware/vgabios-*.bin`) |
| `tools/xp-driver-test.sh <image> <mode>` | the XP display-driver loop headless (doc 15), on Windows 7 too (track M18: `MEM=2048`; `install` then runs DRVINST in an administrator's console opened from the Start menu's search box and clicks the unsigned-driver prompt, the family read from `ver` on COM1): `install`, `ddtest` (8/16/32 bpp + windowed), `modes`, `vesa` (the inbox VGA driver's VESA mode change with no driver of ours installed, the patch 44 regression of 2026-09-24; PASS/FAIL from the screendump), `d3d7` (frame diffed against `d3dpt-dp2-test`'s), `d3dgame8` (XP's own d3d8.dll on the DX8 DDI), `shtest`, `cktest`, `ebtest [-rgb]`, `gamma`, `probe <NAME>`, `probes` (all ten DX8 probes, one boot), `cubetest`, `d3dgame9` (D3DGAME9 through XP's own d3d9.dll, M16), `d3dfeat9` (D3DFEAT9 the same way, its frame and query / getter lines against the native run's byte for byte, M16), `caps` (DX8CAPS + DX9CAPS), `winetest` (Wine's suites, M16), `cmd '<line>'`, `bat <file>` (staged as `E:\RUN.BAT`: the Run box truncates; `UNTIL=word` waits for the batch to echo it to COM1). Knobs: `CPU=pentium3`, `GAME_ISO=`, `SHOTS=n`, `SHOT_KEYS="12:esc"`, `QEMU_EXTRA=`, `VGA=cirrus` (inbox-driver control), `DRIVER_ISO=` (another build's driver), `NO_EXEC=1`, `EXEC=wine`, `FBVER=N` (the adapter reports another register-set version), `KEEP=1` (leave the guest up), `NO_KVM=1`, `OUT=`. `install` puts DRVINST's lines on COM1 and screendumps every 20 s |
| `tools/win98-driver-test.sh <image> [boot\|install]` | the 9x display driver (doc 19): installs the binaries, boots `d3dpt-vga`, reports the BARs, the driver's DEBUG-register lines, the screendump's colour count (≤ 16 = fell back to VGA) and the VGA text page read from VRAM, where a fatal exception writes itself (doc 19 §15); believe it over the screendump. `PROG=<exe>` starts a program through WIN.INI `run=` (`ddprobe.exe` exercises DirectDraw). `NAME_IN_INI=1` names the driver in SYSTEM.INI instead of letting PnP pick it ("does this build work", not "does it install"), `NO_EXEC=1`, `DDFLAGS=` (9x reads the high half), `SHOTS=`, `SETTLE=`, `BOOT_WAIT=`. Raw copy; the `test98` machine, not `~/vms/win98.qcow2` |
| `tools/win98-bsod-test.sh <image>` | a 9x blue screen is visible on `d3dpt-vga` (doc 19 §29): `BSOD.EXE` loads `BSODVXD.VXD`, whose init faults in ring 0; requires `message mode` and `linear mode off`, a blue screendump, the text page naming the VxD, then `linear mode on` after a key and a clean power-off. `WHEN=event` faults from a timer (`BSODTMR.VXD`, no screen switch), `TRIGGER=`/`STAGE=` another way |
| `tools/win98-reboot-test.sh <image> [qmp\|guest\|both]` | Win98 survives a restart (patch 22): a second SeaBIOS banner on the debugcon, a boot's worth of disk reads after it, `LVT0` back to ExtINT. `QEMU=`/`BIOS=0` the stock control |
| `pwrprobe.exe` | Windows' monitor power-down on demand (doc 19 §41): a blank reaches the driver as a screen switch only, so `switched out` then `linear mode off` within a millisecond in the QEMU log is the pass; `NO_DRIVER=1` is the control that must fail. `STAGE=…/pwrprobe.exe PULL=PWRPROBE.LOG GUEST_CMD='start /w C:\PWRPROBE.EXE 5' RUN_SECS=100 tools/win98-game-test.sh <image> pwr` |
| `DRIVER\SETMODE.EXE` | lists / switches XP display modes from a script |
| `DRIVER\DDTEST.EXE` | DirectDraw 7 through our driver: HAL caps, VRAM flip chain, windowed blit, fps, palette rotation at 8 bpp; `scanout offset` lines are the page flips; `sysmem blt` reads back four system-to-video blits (1:1, BltFast, stretched, colour-keyed) and says FAIL if one is wrong |
| `DRIVER\DITEST.EXE` | a game-style DirectInput keyboard: what buffered data, state, `GetAsyncKeyState` and `WM_KEYDOWN` each see |
| `ddprobe.exe` | a DirectDraw object's HAL/HEL caps and `WaitForVerticalBlank` (9x), `C:\2KSBOX\DDPROBE.LOG`. It, `pwrprobe.exe` and `bsod.exe` are built by `guest-tools/build-driver9x.sh` into `guest-tools/out/driver9x/` and staged by the harness, not on the ISO |
| `tools/embed-3d-test.c` | the window-less Mesa backend without a guest (Linux): several frames per dma-buf slot, each slot's memory followed; `embed-3d` |
| `tools/zc-vulkan-test.c` | the same ring through the frontend's Vulkan import alone, `--stage=`, `--use=`, `--threaded`, `--draw=` (doc 12 §4); every combination is clean |
| `tools/wgl-probe.c` | the Windows embed backend's WGL sequence without QEMU (`build/win/wgl-probe.exe`, shipped in the package): run it first on a Windows host that will run a GL guest. It cannot catch the WGL rule's fault (doc 12) |
| `PLAYER_DUMP_OUT=x.png` | the player's shaded frame, headless, even while occluded (`docs/development.md`) |

## Direct3D

| Tool | Proves / runs |
|---|---|
| `tools/d3dpt-dp2-test.cpp` | the display driver's records (doc 15 M7c): VRAM surfaces, a context, D3D7TEST's scene as DP2 tokens, readback checked, hostile records refused; its BMP is `D3D7TEST`'s oracle; `d3dpt-dp2`. Run it on both backends whenever the executor changes (107 checks each). `exec-no-device` runs it with no Vulkan device: a second `Direct3DCreate9` on a DXVK whose constructor had thrown dereferenced a null instance (the community app on macOS 15; DXVK patch 09) |
| `exec-wine` (check) | `d3dpt-dp2-test` with `D3DPT_EXEC_LIB=build/d3dpt/libd3dpt_exec_remote.$SO`: the executor's Windows build under Wine on Wine's d3d9 must draw the in-process frame (M15). Skips without Wine, without mingw's `build/d3dpt/wine/`, or over ssh on macOS; prefix `build/wine-prefix` |
| `guest-tools/src/d3dgame9.c`, `d3dgame8.c` | the reference scene (doc 14): rig goldens first, diffed against every emulated path; `tools/d3dgame9-native.cpp` is it natively on DXVK |
| `guest-tools/src/d3dfeat9.c` + `tools/d3dfeat9-native.cpp` | the D3D9 feature test (shaders, declarations, state blocks, queries, cube maps, surfaces, one quad per guest-DLL bug fixed): the guest frame byte-identical to native, getter lines equal |
| `tools/dxvk-d3d9-test.cpp` | DXVK's d3d9 on patch 04's headless WSI alone |
| `DRIVER\D3D7TEST.EXE` | Direct3D 7 through the HAL: enumeration, Z, texture, the scene, fps; BMP equals `d3dpt-dp2-test`'s |
| `DRIVER\SHTEST.EXE` | vs/ps 1.x through XP's d3d8.dll on the DX8 DDI; ends `shtest: N cases, M failed` |
| `DRIVER\CKTEST.EXE` | palettized textures and colour keying through the DX7 HAL (protocol v8) |
| `DRIVER\EBTEST.EXE` | the DirectX 3 execute-buffer path through XP's `d3dim.dll`; `-rgb` is the software-device control |
| `DRIVER\DXTTEST.EXE` | every D3D8 texture format × pool: CheckDeviceFormat, Create, Lock, a quad read back; `C:\2KSBOX\DXTTEST.LOG` |
| `DRIVER\DX8CAPS.EXE`, `DX9CAPS.EXE` | what d3d8.dll / d3d9.dll make of the driver (M16): every adapter the runtime lists, `CheckDeviceType` per back buffer, depth formats, `CreateDevice` as Wine's tests call it, the caps; `xp-driver-test.sh <image> caps` |
| `DRIVER\WTPROBE8.EXE`, `WTPROBE9.EXE <tag>` | windowed `CreateDevice` in every combination of what Wine's tests and the caps tools do differently (the `static` window class or our own, hidden or visible, a 0x0 or 640x480 back buffer, A8R8G8B8 / X8R8G8B8 / UNKNOWN, no depth or D24S8, SWVP or HWVP), one line each into `WTPROBE<n>_<tag>.LOG`. Made for the rig's Win98, where the tests made no device and DX8CAPS did: `build/winetest/rigdiag/RIGDIAG/RIGDIAG.BAT` runs them plain, redirected and under WTRUN beside the caps tools and the stateblock tests, into `C:\2KSBOX\RIGDIAG`. On our Win98 guest: WTPROBE9 96 of 96, WTPROBE8 64 of 96 (d3d8 takes no UNKNOWN back buffer) |
| `DRIVER\D9CTEST.EXE [-readback]` | the resources d3d9.dll creates on the driver (M16 step 5), one line and HRESULT each: textures of 10x10 and 16x16 with 0 to 2 levels, cubes of edge 3 and 4, volumes (Wine's 2x4x8 of 4 levels), per pool; a render target cleared and read back through `GetRenderTargetData` in A8R8G8B8 / A16B16G16R16F / A32B32G32R32F (texels printed); full-screen devices last. `C:\2KSBOX\D9CTEST.LOG`. On Win98: `STAGE=.../D9CTEST.EXE PULL='2KSBOX\D9CTEST.LOG' GUEST_CMD='C:\D9CTEST.EXE' tools/win98-game-test.sh <image> d9c` (with `SETBPP -save 32` first). `-readback` alone keeps the driver's per-process surface log on the readback; `-modechange` replays Wine's `test_wndproc`'s first case (a device window apart from the focus window, a mode change under the full-screen device, focus to the desktop and back with messages pumped, Reset, then new full-screen devices), `-modechange -nolock` the same with Win98's foreground lock off. On XP: `xp-driver-test.sh <image> cmd 'D:\DRIVER\D9CTEST.EXE -modechange & copy C:\2KSBOX\D9CTEST.LOG E:\d9ctest.log'`, the log read off the scratch disk with `mtype` |
| DX8 probes: `CUBETEST STRMTEST VOLTEST FMTTEST BUMPTEST SPRTEST ANISTEST PATCHTST MSAATEST MGDTEST` | one program per D3D8 feature (`guest-tools/src/d3dptvid/*.c` over `d3d8probe.h`), every draw read back. A feature the caps lack ends `(not offered: <why>)`, so the same program becomes the check the day it is offered. Verdict PASS / NOT OFFERED / FAIL; no log is a FAIL |
| `guest-tools/build-winetests.sh` + `tools/xp-driver-test.sh <image> winetest` | Wine's d3d8 / d3d9 conformance tests (M16, the pinned tag in the script, `patches/winetest/`) through XP's own runtime on our driver. `WTRUN.EXE` runs each test file in its own process under a time cap; `tools/winetest-summary.py` gives a table per file and, against a baseline in `reference/winetest/`, exits 1 on any check failing more than there (`WT_BASELINE=`, `WT_SAVE=`, `WT_TESTS="d3d9:visual"`, `WT_CAP=`). The build's `RUNALL.BAT` runs the same set on the rig. A file that crashes stops there, so its later checks are not counted. `--by-function build/winetest/wine-<tag>` counts the worse keys per test function, and `--baseline rig-xp.txt --split dxvk-wine.txt` marks how many of them DXVK's own run fails too (`rig-xp.txt` is the oracle, `rig-98.txt` Win98's (run with the rig's skip list, `WTSKIP.TXT`), `win11-rtx3090.txt` a modern card for contrast; each one's raw output is in `reference/winetest/logs/<name>/`) |
| `tools/xp-dx9-test.sh <image> <guest-tools.iso>` | the XP half of the `guest` stage's Direct3D checks, alone: a fresh overlay, the driver installed, then DDVMTEST, D3DGAME9, D3DGAME8 and D3DFEAT9 through XP's own runtime in one boot; leaves the frames and `guest-*.log` in `OUT=` for the caller to judge (about 3 minutes under KVM) |
| `tools/win98-dx9-test.sh [image]` | the `win98-dx9` check alone: the scenes, the probes and DDTEST in one Win98 boot, PASS / FAIL per line, exit 1 on any failure (`RAW=`, `OUT=`, `FRESH=1`, `DDFLAGS=`; `DDFLAGS=0x40000000` is a control that must fail D3DFEAT9) |
| `tools/win98-winetest.sh [image]` | the same tests through Win98's DirectX 9.0c runtime on the 9x driver (M16 step 5), on a raw copy of `base98-br` by default: stages them in `C:\WT`, switches the desktop to 32 bpp for the session (`SETBPP`; on 16 bpp d3d8 refuses the tests' windowed A8R8G8B8 device and every d3d8 file skips), runs each file under `WTRUN` and stops when COM1 says `WTALL` (`tools/win98-game-test.sh`'s `UNTIL=`). Without a `voodoo2` in `EXTRA` it renames 3dfx's `3DFX32V2.DLL` to `.OFF` in the raw copy (`base98-br` has 3dfx's driver installed, and d3d8.dll loaded it with no card and crashed in it). `WT_TESTS=`, `WT_CAP=`, `WT_CANARY=1` (a device after every test function; the first that fails stops the run, `STOP.TXT` names the test; winetest patch 09), `WT_SKIP=a,b` (leave test functions out), `WT_BASELINE=` (a name in `reference/winetest/` or a path; `w98-driver` is the driver's Win98 baseline, and with one the script exits 1 on a worse key), `WT_SAVE=`, `WT_BPP=` (0: leave the depth), `FRESH=1` |
| `tools/winetest-dxvk.sh` | the same test EXEs under the host's Wine on DXVK's own 32-bit `d3d9.dll` / `d3d8.dll` (built into `build/dxvk-win32`), in a headless sway when sway is installed: what DXVK itself fails, `reference/winetest/dxvk-wine.txt` (`WT_SAVE=`, `WT_TESTS=`; the DXVK built is our patched one, and it runs with `DXVK_SHADER_CACHE=0` so a patch under test is never hidden by a cached shader). It is also the edit-test loop for `patches/dxvk/` (25 s for d3d9 visual). A guest run compared against it (`winetest-summary.py --baseline`) leaves the failures that are the driver's or the executor's (M16) |
| `D3DPT_DP2_TRACE=<flag file>` | QEMU env: one whole DP2 frame per `touch`: states, tokens, first vertices, bound textures' texel means, every level and the target after each draw as `.ppm` (count pixels per `draw-<n>.ppm` to name the draw that paints an artefact). `D3DPT_DDI_REREAD=1` tells a stale host texture from VRAM never written, `D3DPT_DDI_NOFOG=1` rules fog out |
| `tools/xp-game-test.sh <image> "<dir>" <exe> [name]` | a game on the display driver headless (the image has the driver installed): `CDS=a.iso:b.iso`, `KEYS=8:ret,25:esc` (after the first Direct3D context), `SHOTS=n` (message boxes you cannot otherwise see, and the game's frames), `VGA=cirrus` (the control), `DRW_AFTER=s` (Dr. Watson: every thread's stack; `stacks <log>` prints them), `PAGEHEAP=1`, `CPU=pentium3`, `QEMU_EXTRA=`, `NO_ATTACH=1` (a run that expects no D3D device) |
| `tools/macvm-wine-spike.sh`, `tools/macos-wine-spike-local.sh` | the Wine executor on a pre-26 macOS: in a UTM VM (no GL: Apple's paravirtual GPU is Metal-only) and on a second macOS volume on the same Mac; frames diffed against DXVK's |

## OpenGL and the Voodoo 2

| Tool | Proves / runs |
|---|---|
| `TESTS\GLPROBE.EXE` | whose OpenGL a program gets: `GL_RENDERER` (`GDI Generic` is Microsoft's), a clear read back, 120 frames; `C:\GLPROBE.LOG` |
| `tools/voodoo-guest-test.py` | the Voodoo 2 device with no 3dfx code (doc 21): a FreeDOS program finds `121a:0002`, runs the init sequence, LFB fill, swap; the command FIFO as Glide drives it (JMP, read-pointer reads; under `ramfifo=on` only the device's packet walk can draw the frames); the dither phase (one 4x4 tile, patch 64; `RECOMP=off` interpreter, `DITHER_SUB=off` must fail); the partial packet (the read pointer never passes what was written); the swapped pair and its wrap (doc 21 §13); the teardown (status idle after the FIFO is switched off mid-ring); the stranded client (`FIFO_OFF_REGS=on` the control, doc 21 §11). The host's screendumps are the verdict. `VGA=d3dpt` also checks the hand-back to our adapter and the hardware cursor hidden while the Voodoo has the monitor (patch 66). `voodoo-guest`, `-mmiofifo` (`RAMFIFO=off`), `-d3dpt`, `-undither` (`UNDITHER=on`: one flat (130,130,130)) |

## CD-ROM

| Tool | Proves / runs |
|---|---|
| `target/release/discx` | the CD model (doc 17): `selftest <dir>` (`libdisc`), `info`/`dump`, `scan` (L-EC per sector, failures split into read-anyway / repaired / unreadable; a protection band must be all unreadable), `repair <image> <out>` (the negative-control copy), `subscan`, `convert`, `export`, `mktree` |
| `tools/atapi-guest-test.py` | DOS drives the ATAPI drive by PIO on a cdimage disc (patch 51): replies identical to `discx dump`, MODE SENSE 2A's medium type (03h on the mixed disc, patch 56), sense, audio positions, both stops (after a stop the head stays where playback ended, patch 54); the audio status prints in *decimal* (`21` is `0x15`, play completed); the shelf (patch 52) with `CDSHELF.COM`, the boot disc listed as in the drive before any LOAD. `ATAPI_READ_ERROR=1` (Linux, `tools/read-error-inject.c`) makes audio sectors fail with EIO and play must continue as silence (patch 55); `atapi-guest`, `atapi-read-error` |
| `tools/cd-rate-guest-test.py [image]` | what the guest reads does not depend on how fast it asks: PIO and bus-master DMA, several request sizes, byte-count limits and paces, checksummed against the host; `.iso` vs `.cue` is the driver A/B; `SCAN=1`. The guest must set PCI bus-master enable itself or DMA "succeeds" and writes nothing |
| `tools/xp-cdimage-test.sh <image> <disc> <ref>` | XP copies a whole disc (`.cue/.ccd/.mds/.iso` or `isodir:<dir>`) through cdrom.sys and every file matches; `CDTEST=` also plays track 2 through MCI into the drive's wav. Runs on macOS (mtools). `guest-cdimage`, `guest-dirdisc` |
| `tools/dirdisc-guest-test.sh <image> [win98\|xp]` | a host folder as the CD on each family, proved by the guest's `dir`/`type`; `BIG=1` places markers past 703 MiB, 878 MiB, 2, 4 and 7.8 GiB (both families read all five), `MARKS=` |
| `tools/cdaudio-guest-test.sh <image> [win98\|xp]` | CD-DA through the guest's MCI with `CDIMAGE_TRACE=1`, printing the commands each family sends (Win9x stops with a SEEK, doc 17 §5.4); the drive and the wav are the verdict, not MCI's `status mode`. `PLAY=`, `DISC=`, `WAIT_SECS=` |
| `tools/cdshelf-guest-test.sh <image> [xp\|win98]` | `CDSHELF.EXE` in Windows: the machine boots with the ISO in the drive and the first listing must say so; then load it and read it with `dir`/`type`, refuse a missing disc, eject; SWAPTEST swaps in one step. The verbs' stdout goes to COM1, which XP's `cmd` passes; no `win98` pass recorded |
| `TESTS\CDTEST.EXE` | CD audio through MCI: tracks, play, positions while playing / paused / resumed |
| `CDSHELF\CDSHELF.EXE` / `.COM` | the disc shelf from inside the guest (doc 07); `CDSHELF LIST`, `CDSHELF <n>`, `CDSHELF E`; log `C:\2KSBOX\CDSHELF.LOG` |

## Audio and music

| Tool | Proves / runs |
|---|---|
| `target/release/synthx` (`cargo build --release -p libsynth`) | the engines without QEMU (doc 20): `selftest <dir>` (`libsynth`, `--roms <dir>` for the CM-32L), `wavtone <wav> <hz>`, `bank <sf2>`, `opl <wav>`; `midilog`/`opllog`/`play <log> <wav>` read captures from `LIBSYNTH_MIDI_LOG` / `LIBSYNTH_OPL_LOG` (doc 20 §7.2) and replay them with no guest. A fault both engines show is in neither engine |
| `tools/midi-guest-test.py` | a DOS program detects the AdLib, plays 440 Hz on the OPL3, then A4 through the MPU-401; QEMU's recorded wav is the evidence. Two boots; `midi-guest` |
| `tools/audio-glitch-test.py [sb16\|opl\|cd\|cd+sb16]` | clicks counted in what the host would hear: a pure 441 Hz tone in the player into a simulated DAC (`PLAYER_AUDIO_NULL`, `PLAYER_AUDIO_TAP`); residual spikes of a sine recurrence are breaks. `PERIOD=`, `ICOUNT=1`, `MARGIN=`, `STALL=<MB>` (a 3D race's main-loop holds), `LOAD=`, `VGA=d3dpt`, `CDVOL=` (patch 61's CD half), `CDAMP=16000` (past full scale: samples at full scale are their own FAIL) |
| `tools/duke-guest-test.py` | Duke Nukem 3D's DOS build (from the user's disc, read-only) plays its score into our MPU-401 and the wav; `--run`, `--keys` |
| `TESTS\WAVECAPS.EXE` | every wave/MIDI/mixer caps name with its NUL position (`-1` = none: kills DX9's DSOUND.DLL, doc 20 §5.3); `C:\2KSBOX\WAVECAPS.LOG` |
| `TESTS\QCLOCK.COM [s]` | DOS Quake's `Sys_FloatTime` against the TSC per ~1 s window: speed %, backward reads, tick bursts, gaps. In a DOS box Windows' tick clock skews the TSC calibration, so read `tsc_khz` first |
| `TESTS\WAITFILE.EXE` | a login wait that does not starve the guest (a CHOICE loop made 3dfx's init 12x slower): `run=` names it, `C:\WAITFILE.CFG` gives `file=`, `seconds=`, `then=` |

## Input and pads

| Tool | Proves / runs |
|---|---|
| `tools/hid-descriptor-check.py [file]` | the `usb-gamepad` HID descriptor: balanced collections, input items totalling `GAMEPAD_REPORT_LEN`, a hat with a null state; `pad` |
| `tools/pad-guest-test.py [dos\|xp\|win98]` | a pad as a guest meets it, always **in the player** (skips without a display), `PLAYER_PAD_SCRIPT` as the controller. `dos`: `PADTEST.COM` on the gameport under `-icount`, each axis judged against the undriven axis's spread (`UNTHROTTLED=1` the control). `xp`/`win98`: `PADWIN.EXE`, DirectInput and winmm agreeing on every axis, the hat's null state and the buttons; the d-pad drives axes on the gameport and only the hat on HID. `win98` names a launcher machine with the pad bound once. Never a `usb-tablet` beside the pad (Win98 DirectInput then sees no joystick). `pad-guest`, `pad-guest-xp`, `pad-guest-98` |
| `TESTS\PADWIN.EXE` | the HID pad through DirectInput and winmm on 0..255; logs to `%TEMP%` and COM1 |
| `TESTS\PADTEST.COM` | the gameport from DOS: counts and buttons to COM1 and the screen; idle reads `f0`, absent `ff` |
| `qemu-embed: input:` lines | the player's input queue: drain latency, zero-length presses, drops (only when something is off) |

## Launcher, installer and packaging

| Tool | Proves / runs |
|---|---|
| `launcher-capi/examples/smoke.c` | a C front end over the same models: a DOS machine's defaults, what the family picker does to fields under it, the shelf, the library, the profile editor, on a scratch library; `capi` |
| `SETUP.EXE` (ISO root) | the guest-tools installer (`guest-tools/README.md`); `SETUP /ALL`, `/LIST`, `/I <n>`, `/GAME <n> <dir>`; writes `SETUP.LOG` |
| `tools/setup-guest-test.sh <image> [xp\|win98]` | SETUP in a real guest on both families: `/LIST`, `/ALL`, `/GAME 3`, `/GAME 2`, `/GAME 1` (retired: says so, copies nothing), then Windows' own `dir` (and `net start MAPMEM` on NT) over COM1, because the exit code is not evidence. XP on `d3dpt-vga` (`d3dptvid: adapter found`); Win98 on cirrus checks the four INF files, and the second `/ALL` must stage the driver files with `WININIT.INI` renames. `REBOOT=1` adds `/REBOOT` (a second SeaBIOS banner; on Win98 the staged copies gone). `VOODOO=1` plants markers under 3dfx's Glide names that must survive and a `Voodoo2` Run entry that must move to our key. On Win98 `/I 7` (MS-DOS mode) runs twice: the driver copied and the desktop PIF written both times, and Windows' own `dir` finds the PIF. `NO_NET=1`/`NO_USB=1` for images installed without them |
| `scripts/package-linux.sh` | stages the relocatable prefix and asks the staged launcher (`--paths`, its GTK 4 through `ldd`, and its window through `LAUNCHER_SHOT` on a private Broadway display), player (`--companions`, `ldd`), `qemu-img` and the AppStream/desktop files whether everything resolves inside it; `package` |
| `scripts/package-macos.sh` | the app: the dylib closure rewritten to `@rpath`, then every image the loader touches under `DYLD_PRINT_LIBRARIES=1` must be inside the app, for the player and for the launcher drawing its window (`LAUNCHER_SHOT`; AppKit, nothing deployed); `LSMinimumSystemVersion` measured, and every Mach-O's architecture checked; `package` on a Mac (`docs/build-macos.md` "The app"). `--x86_64` is the Intel app from `scripts/build.sh --x86_64`, checked the same way under Rosetta; `package-x86_64` |
| `scripts/package-flatpak.sh` | the offline Flatpak build against `org.gnome.Sdk` 49, then the Wine add-on (`com._2ksbox.Launcher.Wine`, Wine from source plus the executor's PE pair through the mingw SDK extension; `--no-wine` skips it), then the installed app's `--paths` and `--companions` in its sandbox, its window through `LAUNCHER_SHOT` on a Broadway display inside the sandbox (GTK 4 from the runtime), where `wine`, `wine-host` and `d3dpt-remote` must resolve under /app and the pair must start under the add-on's Wine, and `--picked` on a file exported through the document portal must print the file's own path; regenerate `packaging/flatpak/cargo-sources.json` with `scripts/gen-flatpak-cargo-sources.sh` after any dependency change; `FLATPAK_BUILD_DIR` |
| `scripts/gen-icons.sh [--check]` | every icon size derived from `packaging/icon/2ksbox.png`, the Store's four logos (`packaging/windows/Assets/`) included; `icons` |
| `scripts/package-windows.sh` | the Windows zip: the DLL closure (import tables, then names loaded at run time), then the staged launcher's `--paths`, `LAUNCHER_PACKAGED=1`'s library, the player's `--companions`, the display driver's host test through the staged executor on DXVK and on the system d3d9, `launcher.log`, a static C runtime in `2ksbox.exe`, the launcher's window (`LAUNCHER_SHOT`, WinUI 3), `--wizard-new` through the staged `qemu-img`, `wgl-probe`. Natively in MSYS2's MINGW64 shell, where the launcher is built, with Windows-only `PATH` and `LAUNCHER_DATA_DIR` in scratch, where the window and the system-d3d9 run fail the package; or under wine on Linux with a launcher built on the PC, where the window is not tried (`build-windows.md` "Packaging on Windows") |
| `scripts/package-msix.sh <staged> [--pfx …]` | the Windows package as an MSIX: `makeappx` validates the manifest and every path it names, and with `--pfx` the package is signed for a sideload; on Linux the layout only (`build-windows.md` "The Store package"). `package-windows.sh`'s checks also run the staged launcher with `LAUNCHER_PACKAGED=1`, and its `library` line must be `<profile>\2ksbox (packaged)`, outside the AppData an uninstall deletes |
| `scripts/win-sideload.ps1 [<msix>] [-Check\|-NoInstall\|-Remove]` | on the PC: certificate, trust (one UAC prompt), sign, install. With `-Check`, the installed launcher's `--diagnose` run with package identity must write its `library … (packaged)` line into `%USERPROFILE%\2ksbox\launcher.log`; the real answer to "where does a Store install keep the machines" |
| `appcert.exe test -appxpackagepath <signed msix> -reportoutputpath <xml>` | the Windows App Certification Kit on the sideload's signed copy: the checks Store certification runs. `OVERALL_RESULT` must be `PASS` (a WARNING also certifies; the DPI one was fixed by the executables' manifest); the two optional tests that fail by the package's nature are named in `build-windows.md` "The Store package" |
| `LAUNCHER_SHOT=<png> launcher-mitsuami` | the mitsuami launcher (M19) opens its window with its toolkit found and photographs the content; `LAUNCHER_SCREEN` picks a window and what it opens on (the table in the track doc), and the create and saveprofile runs must print `saved`; `LAUNCHER_PICK=<label>=<path>` answers the named path field's dialog and prints the dialog it would have opened (its start folder and filters); on Linux under `gtk4-broadwayd` (`GDK_BACKEND=broadway`), the recipe in `tracks/m19-mitsuami-launcher.md`. `scripts/test.sh`'s `mitsuami` check drives four of these |
| `tools/rfb-shot.py <port> <png>` | one frame of a local VNC server: the whole KDE window, sidebar included, on Qt's VNC platform, where `LAUNCHER_SHOT` sees only the content (GTK: Broadway's web page) |
| `tools/gpl-scan.py`, `tools/third-party-notices.py` | GPL-2.0-only QEMU files (after a QEMU bump, ADR-010) and the crate licence listing |

## Games and benchmarks

Local only; each works on a raw copy or overlay of an image.

| Tool | Runs |
|---|---|
| `tools/win98-game-test.sh <image> <name>` | a game on the Win98 driver: `GUEST_CMD=` as `C:\RUN.BAT` started by WIN.INI `run=` (a DOS game needs `cd` first; a second Windows program needs `start /w` before the first), `CDS=`, `SHOTS=`, `KEYS=`/`CLICKS=`, `JIGGLE=1`, `DUMP_EVERY=`/`TRACE=1`, `VGA=cirrus`, `STAGE=`, `PULL=`, `TEXT_AT=`, `UNTIL=` (end the run when COM1 prints it; `RUN_SECS` is then the cap), `EXTRA=`, `MUSIC=gm\|mt32\|none`, `NO_DRIVER=1`. **`PLAYER=1`** runs it in the player, the only way to run an OpenGL title (frames every `PLAYER_SHOT_EVERY`). It builds the machine `launcherx --print-args` gives, sound card included (Total Annihilation quits without one, which reads like a driver failure); with a `voodoo2` it waits out 3dfx's login helper (`V2START.LOG`, `VOODOO_WAIT=`). Ends with the power button; a machine that ignores it gets `OUT/hang.txt` (`info registers` twice, `info pic`/`lapic`). With `EXTRA=-perfmap` delete `/tmp/perf-<pid>.map` afterwards (it grows by gigabytes) |
| `tools/w98-3dmark.sh <name> [perf\|whole]` | 3DMark 99 end to end; `tests.txt` places every rate line by the test on screen (read a test's rate there only); `whole` + `tools/tcg-perf-cut.py`, `JIT_SNAPS=`, `PAGES=` + `tools/tcg-form-weights.py`, `QEMU_TCG_OPTS=`, `TABLET=0` |
| `tools/w98-mp2.sh <name> [perf\|dwarf]` | Max Payne 2 in `base98-br`'s hospital corridor from the user's save, uncapped (`CAP=1` keeps the vertical blank): the driver's benchmark (track M17). `rates.txt` from `tools/ddi-rate.py` (the level's frames/s, readback and executor time; with `D3DPT_DDI_FLUSH_AB=n` the within-run A/B by adjacent periods), `cpu.txt` per QEMU thread; `perf` adds `tools/guest-code-owner.py` (the vCPU by host library and by guest module), `dwarf` a call-graph profile for the executor's inclusive costs. Refuses to start beside another QEMU |
| `tools/w98-3dmark2001.sh <name>` | 3DMark2001 SE's Benchmark and its detail pages (`details-NN.png`); `CPU=pentium3,x87-pc64-as-53=on` the inexact switch's A/B |
| `tools/w98-blood.sh <name>` (+ `w98-blood-fps.py`) | Blood in a DOS box, fps from VBE page flips (`FRESH=1` each run) |
| `tools/w98-moto.sh <name>` | Moto Racer into a practice race by screendump classifier (`tools/motoracer-state.py`); `SOFT=1` software renderer, uncapped with `EXTRA='-global d3dpt-vga.ddflags=32768'` |
| `tools/w98-quake2.sh <name>` | Quake II's software timedemo, `fps.txt` from `QCONSOLE.LOG` |
| `tools/xp-motoracer.sh install\|play\|vm\|stop <image>` | Moto Racer on the XP driver (DX3 path): installer, then menus by `motoracer-state.py` into a race |
| `tools/xp-moto-race.sh <image> <name> [qemu]` | the M9 game oracle's fps; `RACE_SAMPLE=`, `RACE_MEMSAVE=` (for `smc-diff.py`), `RACE_DELAY=`, `FPS_RATE=`, `PERFMAP=0`; `RACE_BRAKE=1` (throttle and brake apart), `RACE_WATCH=<s>` / `RACE_CYCLE=A:B` / `RACE_TRACE=1` (`moto-watch.py`), `RACE_STAGE=demo` (the attract demo, no menus) |
| `tools/xp-vicecity.sh play\|vm\|attach\|stop <image>` | GTA Vice City on the DX8 DDI into the city, `rates.txt` from `ddi:` lines; `DDFLAGS=` for the A/B, `NO_KVM=1`; the game's frame limiter must be off |
| `tools/xp-fifa-match.sh kvm\|tcg <image>` | FIFA 2000 into a match and a keyboard test (Esc's pause menu is the pass); `EXEC=wine` |
| `tools/xp-fifa2000.bat`, `tools/xp-maxpayne.bat` | batch files for `xp-driver-test.sh bat`: FIFA 2000 and Max Payne on the HAL with no wrapper DLL |
| `tools/xp-diablo.sh install\|play <image>` | Diablo on 8 bpp palettized modes into Tristram |

## Other tools

`tools/tpm-qtest.py [qemu-system-x86_64]` (the `tpm-qtest` host check;
patch 75, M20 step 2): the libtpms TPM backend with no guest. QEMU runs
under `-accel qtest` and the script is the CPU, sending raw TPM 2.0
commands through `tpm-crb`'s registers as QEMU's `tests/qtest/tpm-util.c`
does. A fresh TPM (Startup, GetRandom, a PCR extend checked against
SHA-256 computed in Python, an NV index), a second QEMU on the same state
file (the index survives, the PCR is reset), and a savevm / loadvm round
trip (PCR and index come back, and a third QEMU reads the snapshot's
index from the file). `OUT=`. On a Mac, which builds no x86_64 QEMU,
`scripts/test.sh` runs it on `qemu-system-i386`, whose q35 has the same
`tpm-crb`.

`tools/win11-spike.py install <iso>` / `boot` (M20 step 1): stock
Windows 11 on our `qemu-system-x86_64` with q35, the secure EDK2 and a
swtpm TPM. `install` runs setup from `tools/win11-spike/autounattend.xml`
(one click on the product key page) and `spike.ps1` reports the build,
firmware, TPM and Secure Boot on COM1; `boot` times power-on to the
desktop and ends with the power button. `ACCEL=tcg`, `OUT=`, `SMP=`,
`MEM=`, `VNC=`, `SHOT=`, `SETTLE=`; needs `swtpm`, `xorriso`, `7z`.
`ARCH=aarch64` (M20 step 4) is Windows 11 on Arm from Microsoft's Arm64
ISO on `qemu-system-aarch64`'s `virt` board under HVF, the launcher's
machine: our EDK2 (`scripts/build-edk2.sh`; `FW_ARM=edk2` for QEMU's,
which needs `SB_BYPASS=1`), the disk and CDs on AHCI, `tpm-tis-device`
with the libtpms TPM, `ramfb`, HD Audio. Windows does not make the
board's PL011 a COM port, so the guest also writes its lines to a FAT
disk on USB (`OUT/report.img`), which the script reads with mtools;
`PROBE=1` on `boot` runs `tools/win11-spike/probe.ps1` from it elevated
(the TPM through `tpmtool` and WMI, every storage, display, sound and
USB device, the network adapters and addresses, the driver store, the
volumes). `LANG_ISO=en-US` stands in for `7z`. `NET=1` adds a network
card (`virtio-net` on aarch64, whose driver comes from the drivers disc,
`build/virtio-win/2ksbox-drivers-arm64.iso`, attached when it exists,
`DRIVERS=` for none); `GPU=virtio-gpu-pci` swaps ramfb for virtio-gpu.

`tools/bmpdiff.py` (frame diffs with masks and budgets),
`tools/ipc-latency-spike.c` (ADR-010's process-boundary numbers),
`tools/upload-server.py [dir] [port]` (getting files off the rig: one plain upload form on port 8000, files into `dir`, nothing overwritten; it prints the LAN address to open).
