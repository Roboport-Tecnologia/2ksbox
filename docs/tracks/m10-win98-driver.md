# Track M10: the native Win98 display driver (doc 19, ADR-012)

This track gives Win98/Me the driver XP has: a 9x layer over the XP
driver's OS-independent core, on the same `d3dpt-vga` adapter and
protocol. Doc 19 holds the design and every finding; doc 15 specifies
the core (adapter, flip chain, DP2 stream, DX8 DDI). Read
`docs/00-status.md` first for the track rules. Work happens on `main`.

## State

Steps 0–4 are done; step 5, real titles, is where the work is.

- **The split** (§19). `core/` includes no DDK header and asks the OS
  for six `d3dpt_os_*` hooks, which `build-driver.sh` checks with `nm`.
  XP's driver rebuilt on it gives pixel-identical frames.
- **Three binaries** (§1): the 16-bit DIB Engine driver `d3dpt9x.drv`,
  the mini-VDD `d3dpt9v.vxd`, and the ring-3 HAL `d3dpt9hl.dll`, which
  links the core and loads at one shared address in every process (§23).
- **Install.** PnP installs it from the INF with no clicks (§16). The
  ISO carries it as `DRIVER9X\`, `SETUP.EXE` installs it on 98/Me, and
  new Win98 machines default to `d3dpt-vga`.
- **The M7c matrix reproduces on 98** with no change to `core/` (§24,
  §25): paced flips, 8 bpp palettes, the DX3/DX7/DX8 faces (`EBTEST`
  5/5, `D3D7TEST` byte-identical to `d3dpt-dp2-test`'s frame, `SHTEST`,
  `CKTEST`, `DXTTEST`, `CUBETEST`).
- **Screen switches work**: a full-screen DOS box both ways (§26, §29),
  blue screens (§29), monitor power-down (§41), the shutdown screen
  (§35), DirectDraw's own Mode X for 320×200 titles (§30).
- **No executor** (`no-exec=on`): the HAL keeps DirectDraw and offers no
  Direct3D (§40). The 9x rule that a DLL beside a game reaches only the
  session's first DirectDraw program is §42.
- **Titles**: Total Annihilation, LEGO Island, Carmageddon, Blood (DOS
  box), Crimson Skies (§28, §34), Diablo II (§33), 3DMark 99, 3DMark2001
  SE's whole benchmark (§36–§39).
- **DirectX 9 on the same HAL** (track M16, closed 2026-09-27): Win98's
  own `d3d9.dll` / `d3d8.dll` run on the driver with SM3, and Max Payne
  2, Vice City, 3DMark2001 SE and Crimson Skies play on it. The design
  is doc 15's DX9 section; the `win98-dx9` and `win98-winetest` checks
  are M16's.

## Scope and files

Owned by this track:

- `guest-tools/src/d3dptvid/w9x/`: `d3dpt9x.c` + `dibthunk.asm` (the
  `.drv`), `d3dpt9dd.c` (its DirectDraw escapes), `d3dptvxd.c` (the
  mini-VDD), `d3dpthal.c` + `.def` (the HAL), shared headers,
  `d3dpt9x.inf`, `res/` (the `oembin` resources GDI requires), and the
  probes `ddprobe.c`, `gdiprobe.c`, `pwrprobe.c`, `devcaps.c`,
  `setbpp.c`, `bsod.c` + `bsodvxd.c`.
- `guest-tools/src/d3dptvid/ddk9x/` (vendored 9x headers, MIT, from
  `vmdisp9x`; provenance in its README) and
  `guest-tools/build-driver9x.sh`.
- `guest-tools/src/setup.c`'s 98/Me display-driver component and
  `tools/setup-guest-test.sh`'s Win98 expectations.
- Tests: `tools/win98-driver-test.sh`, `tools/win98-game-test.sh`,
  `tools/win98-bsod-test.sh`.

Shared (rebase first, edit minimally, name the other track): `core/`
and `nt/` (M7), `d3dpt/d3dpt_fb.h` and `d3dpt/hw/d3dpt_vga.c` (M7; 9x
needed no adapter change, so a change would be a finding),
`d3dpt/d3dpt_proto.h` and `d3dpt/exec/` (M4/M7), `build-wrappers.sh`,
`scripts/test.sh`.

## Where the design lives

| Topic | Doc 19 |
|---|---|
| why a native driver, the split | "Why a native driver…", "The split", §19 |
| the 9x driver model | §1–§10 |
| mini-VDD, linking a VxD the VMM loads | §11, §12 |
| silent refusals (NE relocation, ring-3 mapping, 16-bit register access, `__loadds`) | §13, §14, §18 |
| reading a fault out of VRAM | §15 |
| install, `SYSTEM.INI`, ending a run | §16, §17 |
| the HAL: publication, HALINFO validation, the shared arena | §20–§23 |
| DirectDraw and Direct3D DDIs | §24, §25 |
| titles and screen switches | §26–§39 |
| no executor, 9x's one DirectDraw per session, the front-buffer flush | §40, §42–§44 |
| monitor power-down | §41 |

## Build and test loop

```sh
scripts/build.sh                           # once: this checkout's QEMU, executor, ISOs
guest-tools/build-driver9x.sh              # .drv + .vxd + HAL DLL + INF -> guest-tools/out/driver9x/
tools/win98-driver-test.sh <image> install # fresh raw copy, driver staged, PnP installs it, reboot
tools/win98-driver-test.sh <image> boot    # every run after: re-stages the binaries, boots (~4 min)
PROG=guest-tools/out/driver9x/ddprobe.exe tools/win98-driver-test.sh <image> boot
```

- `install` converts a fresh raw copy (the user's image is never
  written) and stages the driver and INF in `WINDOWS\INF` (§16). It is
  also the reset button and the way out of safe mode. `NAME_IN_INI=1`
  names the driver in `SYSTEM.INI` instead, to test the build rather
  than the install. `boot` re-stages the binaries and `PROG` and deletes
  the last run's probe logs.
- `PROG=` goes in `WIN.INI`'s `run=`: the harness has no serial line,
  and nothing on a Win98 desktop calls `DirectDrawCreate` by itself.
- The harness prints the adapter's BARs, the driver's `d3dpt9x:` /
  `d3dpt9dd:` / `d3dpthal:` lines (DEBUG register), the screendump's
  colour count (16 or fewer means Windows fell back to VGA) and **the
  VGA text page read out of VRAM**, where a fatal exception writes
  itself. Believe it over the screendump (§15). To decode a
  selector:offset, take the selector's base from the LDT (`memsave` at
  the base `info registers` gives) and disassemble the code there.
- `DDFLAGS=` passes the adapter's knob (the 9x driver reads the high
  half, `D9F_*` in `w9x/d3dpt9x.h`); also `NO_EXEC=1`, `SHOTS=`,
  `BOOT_WAIT=`, `OUT=`.
- Games: `tools/win98-game-test.sh <image> <name>` (`PLAYER=1` for
  Glide/OpenGL titles); blue screens: `tools/win98-bsod-test.sh`. Both
  in `docs/testing.md`.
- After touching `core/`, run XP's oracle: `tools/xp-driver-test.sh
  <xp image> d3d7` plus `shtest`, `cktest`, `ebtest` (M7 track).

**Images.** Use a launcher Win98 machine installed after the BIOS-date
stamp (`~/.local/share/2ksbox/machines/<name>/disk.qcow2`; sessions have
used `test98`, `claude98`, `base98-us`, `base98-br`). **Not**
`~/vms/win98.qcow2`: a pre-stamp PnP-BIOS install our INF does not
match, which ends on the inbox VGA with an empty log. The guest runs
**DirectX 9.0c** by decision (§25); the in-box 6.1 cannot run
`D3D7TEST`'s DX7 path, the pixel oracle. Installing DirectX means
booting on `-vga cirrus`, which rebinds the display to the in-box driver
and drops `D3DPT9V.VXD`, so run `install`, not `boot`, after any hand
session on the image.

### The second toolchain

The `.drv` and `.vxd` need **Open Watcom v2** (mingw makes neither
format; the HAL DLL builds with `i686-w64-mingw32`). One tarball carries
every host (`binl64`, `armo64`, `bino64`, `binnt64`), unpacked without
sudo where `build-driver9x.sh` looks (`WATCOM=` overrides; Windows:
`docs/build-windows.md`; macOS: `docs/build-macos.md`):

```sh
curl -L -o ow.tar.xz https://github.com/open-watcom/open-watcom-v2/releases/download/Last-CI-build/ow-snapshot.tar.xz
mkdir -p ~/.local/opt/open-watcom && tar xJf ow.tar.xz -C ~/.local/opt/open-watcom
```

Without it the ISO builds without `DRIVER9X\` and says so. The script
also applies the post-link fixes `wlink` gets wrong (§12 for the VxD;
the NE's expected Windows version and zero local heap) and refuses the
three silent failures at build time.

**Reference trees**, read and never vendored (gitignored):

```sh
git clone --depth 1 https://github.com/JHRobotics/vmdisp9x build/ref/vmdisp9x  # .drv + VxD
git clone --depth 1 https://github.com/JHRobotics/vmhal9x  build/ref/vmhal9x   # ring-3 HAL
curl -L -o build/ref/fixlink.c https://raw.githubusercontent.com/JHRobotics/fixlink/master/fixlink.c
```

## Traps

Open Watcom's:

- **Its inline assembler does not resolve a callee through a macro
  parameter.** `_asm { call p }` inside a `#define` calls nothing; the
  only sign is warning W202. The four mini-VDD screen-switch thunks are
  written out by hand for this.
- **It takes a function's attributes from the first declaration.** A
  DDK prototype without `__loadds` strips it (§18), and a Win16 API of
  the same name wins (`SetCursor` vs the driver's ordinal 102). Hide
  both with a `#define` before the headers.
- **No CRT, so no 32-bit multiply** in 16-bit code: `(DWORD)a * b` links
  against an undefined `__U4M`. Use `MulW`.

The guest's:

- Win98 runs under TCG and every run ends with the ACPI power button, or
  the next boot is safe mode (§17, `docs/00-status.md` "Gotchas").
- **A VxD the VMM dislikes is silently not loaded**, with no
  `BOOTLOG.TXT` line. Suspect the linker first (§12), and check
  `BOOTLOG.TXT`'s date: `BootLog=1` did not refresh it on one image.
- **Edit `SYSTEM.INI` in binary or not at all.** A text-mode rewrite
  strips CRLFs and eats a section header.
- A value the layer **derives** instead of reading from the adapter
  fails silently (§19's wrong DDK constant, §25's command window 16 KiB
  low: every call succeeded, no `ddi:` line). Read
  `D3DPT_FB_REG_CMD_OFFSET` and the other registers, as `nt/` does.
- A DOS/4GW EXE needs `cd` to its folder first, and an image with no
  `SET BLASTER=` fails every DOS game's sound probe before it draws
  (§26).

## Next steps

The doc 04 title matrix, Total Annihilation's exit from a skirmish and
Crimson Skies' QUIT button are done (user, 2026-09-23).

1. **A fault inside a HAL callback leaks `cmd_lock`** and freezes the
   session until the process dies (§36). Accepted for v1 (user
   decision); an unwind that releases it would turn the next such bug
   into one failed call.
2. **ACPI standby.** On resume nothing reprograms the adapter and the
   screen is a blank VGA text page; the player does not report
   `SUSPEND`/`WAKEUP` (§41).
