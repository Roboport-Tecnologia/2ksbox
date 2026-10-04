# guest-tools

The guest-tools ISO holds everything a guest needs from us: the display
drivers, the OpenGL and Direct3D wrappers, the installer, the
disc-shelf program and the test programs. This file covers how the disc
is built, what is on it and what `SETUP.EXE` does. The end-user text on
the disc is `README-ISO.txt` (the root `README.TXT`) and
`README-DRIVER.txt`. Test programs are in
`docs/testing.md`; the drivers' designs are docs 15 (XP) and 19 (9x).
Era binaries are built, never committed (`out/` is git-ignored).

## Building

`scripts/build.sh` runs the `guest` stage. By hand it is
`guest-tools/build-wrappers.sh`, which builds everything below and rolls
`out/guest-tools-3dfx-<rev>.iso`, `<rev>` being the
`third_party/qemu-3dfx` commit the host QEMU is signed with. The
wrappers come from that same commit and the host checks the stamp; a
mismatch means no acceleration.

- **Toolchain.** i686 mingw-w64, nasm, gendef, xxd, shasum and an ISO
  tool (xorriso, or genisoimage/mkisofs). Arch: `mingw-w64-gcc
  mingw-w64-tools nasm xorriso`; macOS: `brew install mingw-w64 nasm
  xorriso`. Homebrew's mingw-w64 has no `gendef`, so the script builds it
  from pinned mingw-w64 sources into `out/tools/bin`
  (`GENDEF_FORCE_BUILD=1` forces that path).
- **Open Watcom v2** for the two formats mingw cannot make: the Win98
  display driver (a 16-bit NE `.drv` and a ring-0 LE `.vxd`,
  `build-driver9x.sh`). The build looks in `WATCOM=`, then
  `~/.local/opt/open-watcom` (the CI release's `ow-snapshot.tar.xz`,
  which carries every host's binaries). Without it the ISO is built minus
  those files, and the build says so.
- **The XP driver** is `build-driver.sh` (doc 15). Its failure stops the
  ISO build.
- **On Windows**, `scripts/build-windows.sh guest` builds the disc in
  MSYS2 through `msys2-i686.sh` (MSYS2's i686 toolchain as MINGW32,
  which qemu-3dfx's wrapper build requires).

Every binary on the disc passes two checks:

- **CRT.** Win9x has no UCRT, which modern mingw-w64 links by default
  (`api-ms-win-crt-*.dll` imports, "required DLL not found" on Win98). A
  compiler shim forces classic `msvcrt.dll` (`-D__MSVCRT_VERSION__=0x700
  -mcrtdll=msvcrt-os`), and the script refuses anything that still
  imports the UCRT api-sets.
- **ISA.** Upstream builds the wrappers `-march=x86-64-v2` for `-cpu
  host`. Our reference guests are `pentium3`, so the shim appends
  `-march=pentium3` and the script rejects SSE2+ or POPCNT instructions
  ("invalid instruction in module opengl32.dll" otherwise).

Every text file on the disc is converted to CRLF, because Win9x Notepad
shows LF-only text as one line.

## The disc

```
SETUP.EXE   the installer (below; guest-tools/src/setup.c)
README.TXT  README-ISO.txt with the commit stamped in
MAPPER\     the device mapper: FXMEMMAP.VXD (9x), FXPTL.SYS +
            INSTDRV.EXE (2000/XP)
DRIVER\     the 2000/XP display driver for d3dpt-vga (D3DPTVID.SYS,
            D3DPTDISP.DLL, D3DPTVID.INF, DRVINST.EXE) and its test
            programs; README.TXT
DRIVER9X\   the 98/Me display driver for d3dpt-vga: D3DPT9X.INF,
            D3DPT9X.DRV, D3DPT9V.VXD, the DirectDraw HAL D3DPT9HL.DLL;
            and the blue-screen test VxDs
WDDM\       the Windows 7 WDDM driver for d3dpt-vga,irq=on (track M18):
            D3DPTKMD.SYS, D3DPTUMD.DLL, D3DPTKMD.INF; MSVC, so only on an
            ISO built on Windows (build-windows.sh's wddm stage)
DINPUT\     per game: DINPUT.DLL, the DirectInput keyboard fix
OPENGL\     per game: OPENGL32.DLL (the GL pass-through) and
            WRAPGL32.EXT, its extension-list cap
VOODOO2\    V2START.EXE, the start-up guard for 3dfx's Voodoo 2 driver
CDSHELF\    CDSHELF.EXE (98/2000/XP), CDSHELF.COM (DOS)
TESTS\      every test, benchmark and calibration program
```

**One folder per role, one copy of every file.** The folder a game's
files are copied from decides its stack, so no name on the disc means two
things, and each folder carries the DLLs under the names a game loads,
so a user copies from Explorer and renames nothing (user request). The
files are the same for 98 and XP.

**The mapper is not optional.** `OPENGL32.DLL` reaches the device through
it and refuses to load without it (`0xc0000142` on NT).
9x has it as a VxD in `SYSTEM`; NT as a kernel driver that the `MAPMEM`
service points at, registered by `INSTDRV.EXE`.

**`WRAPGL32.EXT`** (`guest-tools/wrapgl32.ext`, `ExtensionsYear,1997`)
caps the extension list a game is shown (`docs/development.md`, "OpenGL
pass-through"). It is the one file a user edits per title, so `SETUP
/GAME 3` never overwrites one already next to a game.

## SETUP.EXE

A console program on purpose: it is the one interface Windows 98, XP and
a rescue command prompt all have, and every step is scriptable. It
installs from its own folder, so it works from the CD, a copy on disk or
a share.

```
SETUP                  a menu
SETUP /ALL             every component this Windows can use
SETUP /I <n> [<n>...]  those components
SETUP /LIST            the component and file-set lists
SETUP /GAME <n> <dir>  copy file set <n> next to a game's EXE
SETUP /REBOOT          with /ALL or /I: restart if a step asked for it
SETUP /LOG <file>      the log elsewhere (default C:\2KSBOX\SETUP.LOG)
```

**Components** are installed into Windows. Only the ones this family can
use are listed, in this order; new ones are appended so no existing `/I`
number moves.

| `/I` 9x | `/I` NT | Component | What it does |
|---|---|---|---|
| 1 | 1 | Display adapter driver | NT: `DRVINST.EXE` on `DRIVER\D3DPTVID.INF`. 9x: the four `DRIVER9X\` files into `WINDOWS\INF` (and the three binaries into `SYSTEM`), where PnP installs them on the next boot (doc 19 §16). Restart required |
| 2 | 2 | The device mapper | 9x: `FXMEMMAP.VXD` into the system folder (left alone where 3dfx's driver put one); NT: `FXPTL.SYS` and the `MAPMEM` service, checked running afterwards |
| 3 | 3 | Disc shelf tool | `CDSHELF.EXE` into `WINDOWS`, on both families' search path |
| 4 | 4 | Test programs | `TESTS\` into `C:\2KSBOX`; off in the menu, on with `/ALL` |
| 5 | | Sound Blaster 16 device names | only where a translation made an SB16 wave name too long for DirectX 9: a shorter one in the override `SB16.VXD` reads (doc 20 §5.3) |
| 6 | | Voodoo 2 start-up guard | only with a 3dfx card. 3dfx's `Voodoo2` Run entry moves to `HKLM\SOFTWARE\2ksbox\Voodoo2` and `V2START.EXE` takes its place (doc 21 §11) |

**File sets** (`/GAME <n> <dir>`) are copied next to one game, never into
the system folder. Each set is self-contained, so two stacks never share
a folder.

| n | Set | From |
|---|---|---|
| 1 | *retired*: the Direct3D 8/9 DLLs (M16 step 7). Direct3D is the display driver's now, through Windows' own runtime | |
| 2 | DirectInput keyboard fix (`DINPUT.DLL`) | `DINPUT\` |
| 3 | OpenGL pass-through (`OPENGL32.DLL WRAPGL32.EXT`) | `OPENGL\` |

Set 1 keeps its number so set 3 does not move; `/GAME 1` says it is
retired. `DINPUT.DLL` merges `GetAsyncKeyState` into a
non-exclusive keyboard's state, for a game whose loop stops pumping
messages (FIFA 2000's match, doc 15); `D3DPT_DINPUT_LOG=1` adds its log.
It is per game by decision, never system-wide.

**A machine with a 3dfx card** (the emulated Voodoo 2, doc 21) gets its
Glide from 3dfx's driver; nothing on the disc is Glide (ADR-020). SETUP
finds the card in the live devnode tree (`HKEY_DYN_DATA` on 9x,
`CM_Locate_DevNode` on NT), not the registry's history, and component 2
then leaves an `FXMEMMAP.VXD` already there (3dfx's own binary, same
IOCTLs) alone rather than downgrade it.

### On Windows 98/Me

- **An installed driver file is never overwritten in place.** A module
  whose file is replaced under it runs the new build's bytes at the old
  build's addresses: KERNEL reloads a 16-bit `.DRV`'s discarded code
  segments from disk, and a ring-3 DLL is demand-paged from its file. On
  9x a fault in the display driver is a blue screen. The `.DRV` and VxD
  are held open and refuse the copy, but the HAL DLL is not held while an
  application has it loaded. So when a driver file already exists, all
  seven (four in `WINDOWS\INF`, three in `SYSTEM`) are staged beside
  their targets with the extension's last character made `_`
  (`D3DPT9X.DR_`, `D3DPT9X.IN_`, since the INF and `.DRV` share a base
  name) and listed in `WININIT.INI [rename]`, which WININIT applies
  before the GUI on the restart the step asks for anyway. A first install
  copies outright. (NT uses `MoveFileEx(MOVEFILE_DELAY_UNTIL_REBOOT)` for
  a locked file.)
- **The restart comes from a detached copy of SETUP.** `ExitWindowsEx`
  from a console process on Windows 98 never returns and starts no
  shutdown: a console program has no message queue of its own, and the
  stuck call holds the Win16Mutex, so a second thread pumping messages
  goes down with it. SETUP re-runs itself as `SETUP /REBOOTNOW` with
  `DETACHED_PROCESS`, and that copy restarts the machine (~20 s).
  `rundll32 shell32.dll,SHExitWindowsEx 2` does nothing, and `rundll32
  krnl386.exe,exitkernel` leaves the FAT dirty. NT restarts from SETUP
  itself.

`tools/setup-guest-test.sh <image> [xp|win98]` guards all of it in a real
guest (`REBOOT=1` for the restart, `VOODOO=1` for the 3dfx card;
`docs/testing.md`).

**Windows 7** (32-bit) takes the XP family's components, installed as on
XP (track M18). SETUP carries a manifest asking for an administrator
(`src/setup.manifest`), so UAC asks once, and the display driver's install
shows Windows' "can't verify the publisher" prompt, answered with "Install
this driver software anyway". Without the manifest Windows 7 elevated
SETUP by its name anyway, then showed the Program Compatibility
Assistant's "might not have installed correctly". Checked by hand on the
user's `win7` machine: `SETUP /ALL` installs all four and the machine
restarts on the driver. `setup-guest-test.sh` has no `win7` family yet.

## CDSHELF: the disc shelf from inside the machine

The launcher's shelf (doc 07) from a guest that may be mid-game.
`CDSHELF` lists it, `CDSHELF <n>` puts a disc in the drive, `CDSHELF E`
empties it. Both builds talk to the machine's own CD-ROM drive, which
answers a vendor ATAPI opcode with the shelf (patch 52,
`cdshelf/cdshelf_proto.h`), so there is no new device and nothing to
install.

- **`CDSHELF.EXE`** (`src/cdshelf.c`), one binary for both Windows
  families. XP/2000 use SPTI (`IOCTL_SCSI_PASS_THROUGH_DIRECT` on
  `\\.\<letter>:`); 98/Me use ASPI, with `WNASPI32.DLL` loaded at run time
  (absent on XP; a stock 98 has it with `APIX.VXD`).
  `SendASPI32Command` is **`__cdecl`**, not stdcall, and its exports carry
  no `@n` to say so. Declared `WINAPI`, it links, leaves the stack four
  bytes out on the first call, and Windows kills the program a moment
  later. Every CD-ROM drive is asked and the one that answers is used;
  `-d E:` overrides on XP, `-v` shows each CDB. Log:
  `C:\2KSBOX\CDSHELF.LOG`.

  With no arguments it opens a window (plain USER32 controls, nothing
  newer than Windows 95, the insert on a worker thread so it keeps
  painting). **Insert is grey while a disc is in the drive** (user
  decision), so the tray is emptied with Eject on purpose; the command
  line still swaps in one step. It is a `-mwindows` program whose verbs
  print to a redirected stdout. XP's `cmd` passes that on; under Win98's
  `COMMAND.COM`, read the log instead.
- **`CDSHELF.COM`** (`src/cdshelf.asm`, NASM) drives the drive by PIO as
  `tools/atapi-guest-test.py` does. It finds the drive with IDENTIFY
  PACKET DEVICE, because by the time a DOS program runs the BIOS has left
  the ATAPI signature registers at zero. With no arguments it lists the
  shelf and waits for a key: 0–9 loads that disc, `E` empties, `R`
  re-reads, Esc quits. `CDSHELF LIST` is the non-interactive form.

Both **empty the drive and wait for it to report the tray empty before
loading**. Windows and MSCDEX cache the last disc, so a swap they never
saw as a removal leaves the old files on screen; and the device runs a
medium change from one bottom half, so an eject and a load sent back to
back collapse into one. The Windows build then dismounts the volume
(`FSCTL_DISMOUNT_VOLUME`), because its own TEST UNIT READY polling
consumes the one media-change sense the drive raises.

Tests: `tools/atapi-guest-test.py` (`atapi-guest`) runs `CDSHELF.COM` on
a FreeDOS floppy; `tools/cdshelf-guest-test.sh <image> xp` runs
`CDSHELF.EXE` in XP. The user has run the EXE by hand on Win98; a
scripted `win98` pass is not recorded.

## Other programs on the disc

- **`D3DGAME9.EXE` / `D3DGAME8.EXE`** (`src/d3dgame9.c`, `d3dgame8.c`,
  scene in `d3dgame.h`), the Direct3D reference workload (doc 14, "P0a"):
  one deterministic scene on both APIs, windowed or `-fs` (`-w -h -bpp16
  -novsync`). `-frames N` runs a fixed-step sequence and exits, `-dump N
  file.bmp` writes frame N; `-shader` adds a vs_1_1 path when a d3dx9 DLL
  is present (d3dx9_33+ HLSL refuses ps_1_1, so the pixel stage stays
  fixed-function; don't change that without a new golden set). Keys:
  WASD/arrows/Q/E, F1 wireframe, Space pause, Esc. Output also goes to
  `d3dgame9.log` / `d3dgame8.log` (`-log file`). The rig's BMPs are the
  goldens in `reference/d3d/`.
- **`D3D9TEST.EXE`**, the D3D9 counterpart of wglgears: prints the adapter
  name, HAL caps, the x87 control word
  after `CreateDevice` (`PC=24` expected) and a spinning triangle's fps;
  an optional frame count.
- **`MODETEST.EXE`** prints the current desktop mode, the driver's mode
  list and the result of the `ChangeDisplaySettingsEx` calls DirectDraw
  and Direct3D make. Run it when a full-screen game dies at start-up.
- **`WGLGEARS.EXE`**, Mesa's wglgears from qemu-3dfx's demos. Next to
  `OPENGL32.DLL` it is the zero-dependency GL pass-through check.

Every program of ours writes its log (and any BMP it dumps) to
`C:\2KSBOX` through `src/guestlog.h` (`guest_log_open()`,
`guest_path()`), whatever folder it started from; `set BOXLOG=<dir>`
moves it. The rest of `TESTS\` and `DRIVER\` is catalogued in
`docs/testing.md`.

Later: SoftGPU (a pinned release), AC'97 and network drivers, an
in-guest `verify` tool (docs 04, 06).
