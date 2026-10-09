#!/usr/bin/env bash
# Build qemu-3dfx guest wrappers (Windows DLLs) from the SAME
# third_party/qemu-3dfx commit our QEMU fork is signed with, plus our own
# per-game DLLs, drivers and test programs, and stage them as a
# guest-tools ISO. Needs: i686-w64-mingw32-gcc, gendef, xxd, shasum,
# git, make, nasm; xorriso or genisoimage/mkisofs for the ISO.
#   Linux (Arch):  pacman -S mingw-w64-gcc mingw-w64-tools xorriso
#   macOS:         brew install mingw-w64 xorriso
#   Windows:       MSYS2's MINGW64 shell, scripts/build-windows.sh --msys2-deps
#                  (msys2-i686.sh sets up the rest; docs/build-windows.md)
# The DJGPP DXEs are skipped outright.
set -euo pipefail
# A step that fails without a word of its own (a check that exits after
# printing to a stdout nobody shows) still says where it stopped.
set -E
trap 'echo "$(basename "$0"): stopped at line $LINENO: $BASH_COMMAND" >&2' ERR

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FX="$ROOT/third_party/qemu-3dfx"
OUT="$ROOT/guest-tools/out"
REV="$(git -C "$FX" rev-parse --short HEAD)"
. "$ROOT/guest-tools/msys2-i686.sh"

# gendef (mingw-w64-tools) is not in Homebrew's mingw-w64; build it from
# pinned upstream sources when missing. It is a standalone C program.
GENDEF_SRC_REF="93f3505a758fe70e56678f00e753af3bc4f640bb"   # mirror/mingw-w64 master, 2024-09-24
ensure_gendef() {
  if command -v gendef >/dev/null && [ -z "${GENDEF_FORCE_BUILD:-}" ]; then return; fi
  local d="$ROOT/guest-tools/tools" bin="$ROOT/guest-tools/tools/bin"
  if [ ! -x "$bin/gendef" ]; then
    echo "==> building gendef from mingw-w64 @ ${GENDEF_SRC_REF:0:7}"
    mkdir -p "$d/gendef-src" "$bin"
    local base="https://raw.githubusercontent.com/mirror/mingw-w64/$GENDEF_SRC_REF/mingw-w64-tools/gendef/src"
    for f in compat_string.c compat_string.h fsredir.c fsredir.h gendef.c gendef.h gendef_def.c; do
      curl -fsSL -o "$d/gendef-src/$f" "$base/$f"
    done
    cc -O2 -w -DVERSION='"1.0-win98xp"' -o "$bin/gendef" "$d"/gendef-src/*.c
  fi
  export PATH="$bin:$PATH"
}
ensure_gendef

# The wrapper Makefiles call bare `objdump` (exports-check counts PE export
# entries) and lean on GNU sed. On macOS, `objdump` is Apple's LLVM one and
# can't read PE the same way: route it to the mingw cross binutils' GNU
# objdump, and prefer gnu-sed when installed.
ensure_gnu_tools() {
  local bin="$ROOT/guest-tools/tools/bin"; mkdir -p "$bin"
  if ! objdump --version 2>/dev/null | grep -q 'GNU objdump'; then
    local xd; xd="$(command -v i686-w64-mingw32-objdump || true)"
    [ -n "$xd" ] || { echo "need GNU objdump (i686-w64-mingw32-objdump from mingw-w64)"; exit 1; }
    ln -sf "$xd" "$bin/objdump"
  fi
  if [ "$(uname -s)" = Darwin ]; then
    local g; g="$(brew --prefix gnu-sed 2>/dev/null || true)/libexec/gnubin"
    [ -d "$g" ] && export PATH="$g:$PATH"
  fi
  export PATH="$bin:$PATH"
}
ensure_gnu_tools

# Win9x has no UCRT. Modern mingw-w64 (Homebrew, Arch) defaults to UCRT, so
# force classic msvcrt: msvcrt-mode headers + link msvcrt.dll (import lib
# libmsvcrt-os.a on UCRT-default toolchains). Done via a compiler shim so
# qemu-3dfx's Makefiles need no changes; the same flags build wglgears.
MSVCRT_FLAGS="-D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os"
# qemu-3dfx's Makefiles use -march=x86-64-v2 (SSE4.2/POPCNT); our reference
# guests are pentium2/pentium3 class (doc 06) and would #UD. Appended AFTER
# the Makefile's flags so it wins: Pentium III floor (SSE1; doubles on x87).
ARCH_FLAGS="-march=pentium3 -mtune=generic"
ensure_msvcrt_cc() {
  local bin="$ROOT/guest-tools/tools/bin" real
  # A shim from a previous run may already be on PATH (tools/bin gets
  # prepended by the gendef/objdump steps): remove it first, or `command -v`
  # would find the shim itself and it would exec itself forever
  # ("argument list too long").
  rm -f "$bin/i686-w64-mingw32-gcc" "$bin/gcc"
  real="$(command -v i686-w64-mingw32-gcc || true)"
  case "$real" in "$bin"/*) echo "internal error: shim resolved to itself"; exit 1;; esac
  [ -n "$real" ] || { echo "need i686-w64-mingw32-gcc (mingw-w64)"; exit 1; }
  [ -f "$(dirname "$(dirname "$real")")/i686-w64-mingw32/lib/libmsvcrt-os.a" ] || \
  [ -f "$(dirname "$real")/../i686-w64-mingw32/lib/libmsvcrt-os.a" ] || \
  [ -f "$(dirname "$real")/../lib/libmsvcrt-os.a" ] || \
    echo "warning: libmsvcrt-os.a not found next to the toolchain; msvcrt link may fail"
  printf '#!/usr/bin/env bash\nexec "%s" %s "$@" %s\n' "$real" "$MSVCRT_FLAGS" "$ARCH_FLAGS" > "$bin/i686-w64-mingw32-gcc"
  chmod +x "$bin/i686-w64-mingw32-gcc"
  # MSYS2 (msys2-i686.sh): conf_wrapper's native mode writes CC=gcc into
  # qemu-3dfx's Makefiles, not the prefixed name, so plain gcc gets the
  # same flags there.
  if [ "${MSYSTEM:-}" = MINGW32 ]; then
    cp "$bin/i686-w64-mingw32-gcc" "$bin/gcc"
  fi
}
ensure_msvcrt_cc

check_isa() {  # fail loudly on SSE2+ / POPCNT (guest CPU floor is pentium3)
  # (MMX-register forms of punpck* are Pentium MMX; only the %xmm forms are SSE2)
  local n; n=$(objdump -d "$1" | grep -v '%mm[0-7]' | grep -cE '\b(movdq[au]|movapd|movupd|pshufd|punpck[hl](bw|wd|dq|qdq)|paddq|cvtsd2|cvtsi2sd|cvttsd2si|movsd[[:space:]]+%xmm|xorpd|andpd|popcnt|ptest|pcmpistr|pshufb|pmaddubsw)\b' || true)
  if [ "$n" -gt 0 ]; then echo "ERROR: $1 contains $n SSE2+/POPCNT instructions (pentium3 floor)"; exit 1; fi
}
check_crt() {  # fail loudly if anything still imports the UCRT api-sets
  if objdump -p "$1" | grep -q 'api-ms-win-crt'; then
    echo "ERROR: $1 links against the UCRT (not loadable on Win9x)"; exit 1
  fi
}
# Every text file staged on the disc goes through this: Win9x Notepad shows
# LF-only text as one line.
crlf() { awk '{ sub(/\r$/, ""); printf "%s\r\n", $0 }'; }

build_wrapper() {  # $1 = 3dfx | mesa
  local d="$FX/wrappers/$1/build"
  rm -rf "$d" && mkdir -p "$d"
  ( cd "$d" && bash "$FX/scripts/conf_wrapper" >/dev/null && make )   # serial: 'fxlib' step must precede objects
}

echo "==> qemu-3dfx commit $REV"
build_wrapper 3dfx
build_wrapper mesa

# The ISO: one folder per role. Each stack owns a folder, so "copy this
# next to the game" can never pick up the wrong DLL, and every test
# program lives in TESTS\. One copy of every file.
rm -rf "$OUT/iso"
mkdir -p "$OUT/iso"/{MAPPER,OPENGL,DINPUT,TESTS,CDSHELF,VOODOO2,DOSMODE}
G="$FX/wrappers/3dfx/build"; M="$FX/wrappers/mesa/build"
T="$OUT/iso/TESTS"

# MAPPER\: the device mapper (FXMEMMAP.VXD on 9x, FXPTL.SYS + INSTDRV on
# NT), which OPENGL32.DLL reaches the OpenGL pass-through device through.
# It comes out of qemu-3dfx's 3dfx wrapper build, whose Glide DLLs are not
# staged: the Glide pass-through is retired (ADR-020), and a Glide game
# runs on the emulated Voodoo 2 with 3dfx's own driver (doc 21).
cp "$G"/fxmemmap.vxd "$G"/fxptl.sys "$G"/instdrv.exe "$OUT/iso/MAPPER/"
# OPENGL\: the GL pass-through wrapper, per game, with the settings file
# the wrapper reads from the game's own folder. It ships with a year cap on
# the extension string: a modern host reports thousands of characters of
# extension names and a 1990s title reads that into a fixed buffer (GLQuake
# returns into the list itself and dies in an unknown module), so the
# default is what such a game can hold. wrapgl32.ext is the one file here a
# user edits, so SETUP /GAME never overwrites one already next to a game.
cp "$M"/opengl32.dll "$OUT/iso/OPENGL/"
cp "$ROOT/guest-tools/wrapgl32.ext" "$OUT/iso/OPENGL/WRAPGL32.EXT"
# DINPUT\: the DirectInput shim (dinput/dinput.c), per game. It forwards
# to the system dinput.dll and merges GetAsyncKeyState into a non-exclusive
# keyboard's state, for a game whose loop stops pumping messages (FIFA
# 2000's match, doc 15). Silent by default; D3DPT_DINPUT_LOG=1 in the
# environment adds the log of what the game asks of its keyboard / mouse
# devices and what it gets back.
i686-w64-mingw32-gcc -O2 -Wall -shared -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os \
  -o "$OUT/iso/DINPUT/dinput.dll" "$ROOT/guest-tools/src/dinput/dinput.c" "$ROOT/guest-tools/src/dinput/dinput.def" \
  -static-libgcc -Wl,--kill-at -Wl,--enable-stdcall-fixup -ldxguid

# TESTS\: every test, benchmark and calibration program, one copy each.
# Which stack a test runs on is decided by what is copied next to it, not
# by which folder it came from. SETUP.EXE's /GAME does that.
# Reference workloads (doc 14 P0a): the same deterministic game-like scene on
# Direct3D 9 and Direct3D 8; -frames N -dump N x.bmp for golden images.
# msvcrt, never the UCRT: modern mingw defaults to api-ms-win-crt-*.dll, which
# no era Windows has. A UCRT-linked build runs only on an image that happens
# to carry the redistributable and dies before main() everywhere else (on
# Win98, silently: the process never reached DirectDraw).
i686-w64-mingw32-gcc -O2 -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os -march=pentium3 \
  -o "$T/d3dgame9.exe" "$ROOT/guest-tools/src/d3dgame9.c" -ld3d9 -lgdi32 -luser32
i686-w64-mingw32-gcc -O2 -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os -march=pentium3 \
  -o "$T/d3dgame8.exe" "$ROOT/guest-tools/src/d3dgame8.c" -ld3d8 -lgdi32 -luser32
# Feature test (doc 14 P3): shaders, declarations, state blocks, queries, cube maps, surfaces.
i686-w64-mingw32-gcc -O2 -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os -march=pentium3 \
  -o "$T/d3dfeat9.exe" "$ROOT/guest-tools/src/d3dfeat9.c" -ld3d9 -lgdi32 -luser32
# D3D9 smoke test (guest-tools/src/d3d9test.c): adapter string, HAL caps,
# x87 control word after CreateDevice, spinning triangle with fps.
i686-w64-mingw32-gcc -O2 -o "$T/d3d9test.exe" "$ROOT/guest-tools/src/d3d9test.c" -ld3d9 -lgdi32 -luser32
# DDVMTEST.EXE prints what a launcher's video-memory check sees (Vice
# City's: DirectDraw's free video memory, which the display driver answers).
i686-w64-mingw32-gcc -O2 -o "$T/ddvmtest.exe" "$ROOT/guest-tools/src/ddvmtest.c" -lddraw -ldxguid
# Display-mode probe (guest-tools/src/modetest.c): current mode, mode list,
# ChangeDisplaySettingsEx results for the switches DirectDraw and Direct3D make.
i686-w64-mingw32-gcc -O2 -o "$T/modetest.exe" "$ROOT/guest-tools/src/modetest.c" -luser32
# DITHTEST.EXE: what repeated alpha blending does to a 16-bit frame buffer
# (doc 21 §9): a grey that dithers in every channel, blended onto itself
# 1 to 128 times per column, so the dither the chip should subtract on a
# blend read-back accumulates where it is not subtracted. 86Box's
# interpreter subtracts it and neither of its recompilers does, so run it with
# `-device voodoo2,recompiler=on` and `=off` and compare. A Glide 2.x
# program against qemu-3dfx's GLIDE2X import library and our own subset of
# the Glide 2.4 header (guest-tools/src/glide2sdk.h), run on 3dfx's own
# DLL on a machine with the card.
i686-w64-mingw32-gcc -O2 -o "$T/dithtest.exe" "$ROOT/guest-tools/src/dithtest.c" \
  -I"$ROOT/guest-tools/src" -L"$G" -lglide2x -luser32
# GL smoke test: Mesa's wglgears, ships in qemu-3dfx's demos. Run it next to
# OPENGL32.DLL inside the guest; the title/console shows the renderer.
i686-w64-mingw32-gcc -O2 -o "$T/wglgears.exe" "$FX/wrappers/mesa/demos/wglgears.c" \
  -lopengl32 -lgdi32 -lglu32 -mwindows
# SSE throughput (guest-tools/src/ssebench.c, doc 16): D3DX-shaped SSE1
# kernels plus the same math in x87 C; ns per op, console + C:\2KSBOX\SSEBENCH.LOG.
i686-w64-mingw32-gcc -O2 -o "$T/ssebench.exe" "$ROOT/guest-tools/src/ssebench.c"
# CDTEST.EXE: CD audio through MCI (doc 17 §6.3), the CD-ROM backend's in-guest check
i686-w64-mingw32-gcc -O2 -o "$T/cdtest.exe" "$ROOT/guest-tools/src/cdtest.c" -lwinmm
# CRT calibration patterns (doc 09, guest-tools/src/crtcal.c + crtcal.h): the
# same patterns tools/crtcal-render writes for the shader side, put on a real
# tube at the exact mode through an exclusive full-screen DirectDraw primary.
i686-w64-mingw32-gcc -O2 -o "$T/crtcal.exe" "$ROOT/guest-tools/src/crtcal.c" \
  -lddraw -ldxguid -luser32
# The 720x400 text-mode patterns (doc 09, guest-tools/src/textcal.asm). DOS
# only: 720x400 is the VGA *text* mode, which no Windows display driver
# offers, so it is reachable from FreeDOS or a "Restart in MS-DOS mode"
# screen and nowhere else.
nasm -f bin -o "$T/textcal.com" "$ROOT/guest-tools/src/textcal.asm"
# PADTEST.COM: the gameport at 0x201 as a DOS game reads it (M13 path B,
# guest-tools/src/padtest.asm). One write arms four one-shots and the axes
# are how long the loop counted before each bit fell. DOS only, on purpose:
# DOS is the one family the USB pad cannot reach. Prints to COM1
# and to the screen, so it is both the harness's evidence
# (tools/pad-guest-test.py) and something to run by hand in a DOS box.
nasm -f bin -o "$T/padtest.com" "$ROOT/guest-tools/src/padtest.asm"
# QCLOCK.COM: DOS Quake's clock (Sys_FloatTime: the BIOS tick count plus PIT
# counter 0, backward readings clamped to zero) read in a tight loop beside
# the TSC, one line per second of how far it ran ahead (guest-tools/src/
# qclock.asm). For a DOS game that speeds up in bursts: in a Win9x DOS box
# against the same machine in pure DOS, which separates Windows' queued
# timer ticks from QEMU's own. Prints to COM1 and the screen at the end.
nasm -f bin -o "$T/qclock.com" "$ROOT/guest-tools/src/qclock.asm"
# PADWIN.EXE: the same question of the USB HID pad (M13 path A), asked the
# way a game asks it rather than the way the Game Controllers panel shows it,
# through *both* APIs a title of the era can call: DirectInput, and winmm's
# joyGetPosEx on top of 9x's VJOYD, which is the one that settles whether
# Windows 98 needs the gameport's driver half. Enumerate attached joysticks,
# put every axis on the report's own 0..255 range in both columns, read the
# POV hat (where a missing null state shows up) and the buttons. Writes
# to COM1 itself, so the harness can start it from the Run dialog with no
# shell to redirect. The name is PADWIN, not PADTEST, because the DOS probe above is
# PADTEST.COM in this same folder, and both DOS and cmd resolve a bare name
# to the .COM first.
i686-w64-mingw32-gcc -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os \
  -march=pentium3 -mtune=generic -o "$T/padwin.exe" "$ROOT/guest-tools/src/padwin.c" \
  -ldinput -ldxguid -lwinmm -luser32
# WAVECAPS.EXE: every wave / MIDI / mixer device's 32-byte name as
# GetDevCaps returns it, with where its NUL is, into C:\2KSBOX\WAVECAPS.LOG. A name
# that fills all 32 bytes is what kills DirectX 9's DSOUND.DLL (its /GS
# cookie, c0000409): the Portuguese Win98's SB16 wave-in name (doc 20 §5.3).
i686-w64-mingw32-gcc -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os \
  -march=pentium3 -mtune=generic -mwindows -o "$T/wavecaps.exe" \
  "$ROOT/guest-tools/src/wavecaps.c" -lwinmm

# WAITFILE.EXE: wait for a file, then start something. A CHOICE loop in a
# DOS box polls: it pegged the guest and made 3dfx's card initialisation
# twelve times slower than at an idle login (12,646 ms against 1,047 ms on
# base98-br). A sleeping Win32 program costs nothing, and with `then=` no
# DOS box is open during the wait at all. A loose copy beside the ISO too: tools/win98-game-test.sh
# stages it into the image rather than mounting the disc.
i686-w64-mingw32-gcc -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os \
  -march=pentium3 -mtune=generic -mwindows -o "$OUT/iso/TESTS/waitfile.exe" \
  "$ROOT/guest-tools/src/waitfile.c"
cp "$OUT/iso/TESTS/waitfile.exe" "$OUT/waitfile.exe"

# GLPROBE: whose OpenGL a program on this machine gets (DDPROBE's counterpart,
# doc 19 §43). It loads opengl32 at run time rather than importing it, so that
# a pass-through refusing to load without its device mapper is a line in its
# log instead of a program that dies before main().
i686-w64-mingw32-gcc -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os \
  -march=pentium3 -mtune=generic -o "$OUT/iso/TESTS/glprobe.exe" \
  "$ROOT/guest-tools/src/glprobe.c" -lgdi32 -luser32
cp "$OUT/iso/TESTS/glprobe.exe" "$OUT/glprobe.exe"

# CDSHELF: the host's disc shelf from inside the machine (doc 07, patch 52;
# protocol cdshelf/cdshelf_proto.h). One EXE for both Windows families (SPTI
# on XP, WNASPI32 loaded at run time on Win98) plus a DOS .COM that drives
# the drive by PIO, for a DOS box that has neither. -mwindows: run with no
# arguments it is a window (a disc swap is a thing you do, not a command line
# you retype); its verbs still write to a redirected stdout.
i686-w64-mingw32-gcc -O2 -Wall -mwindows -o "$OUT/iso/CDSHELF/cdshelf.exe" \
  "$ROOT/guest-tools/src/cdshelf.c" -I"$ROOT/cdshelf"
nasm -f bin -o "$OUT/iso/CDSHELF/cdshelf.com" "$ROOT/guest-tools/src/cdshelf.asm"

# VOODOO2\: V2START.EXE, the start-up guard for the emulated Voodoo 2's own
# 3dfx driver (doc 21 §11). Its login helper initialises the card from
# another process for seconds under TCG and hangs any Glide game started
# meanwhile; SETUP takes the helper's Run entry and puts this in its place,
# which runs it and keeps a notice on the desktop until it is done. Win98/Me only.
i686-w64-mingw32-gcc -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os \
  -march=pentium3 -mtune=generic -mwindows -o "$OUT/iso/VOODOO2/v2start.exe" \
  "$ROOT/guest-tools/src/v2start.c"

# DOSMODE\: UIDE.SYS, FreeDOS's CD/DVD driver (third_party/uide, public
# domain, as packaged with FreeDOS 1.4), for Win98's "Restart in MS-DOS
# mode". SETUP's MS-DOS mode step copies it to C:\2KSBOX and loads it from
# CONFIG.SYS as a CD-only driver, with MSCDEX in DOSSTART.BAT (setup.c).
cp "$ROOT/third_party/uide/UIDE.SYS" "$OUT/iso/DOSMODE/uide.sys"

# XP display driver for the d3dpt-vga adapter (doc 15, M7a): built and
# checked by its own script (kernel-mode PE rules differ), staged as DRIVER\.
# Its progress is not wanted here, its failure is. The script reports a
# failed check on stdout and exits, which under >/dev/null ended this whole
# ISO build without a word.
if ! drv_log="$("$ROOT/guest-tools/build-driver.sh" 2>&1)"; then
  echo "the XP display driver did not build (guest-tools/build-driver.sh):" >&2
  printf '%s\n' "$drv_log" | tail -15 | sed 's/^/    /' >&2
  exit 1
fi
mkdir -p "$OUT/iso/DRIVER" && cp "$ROOT"/guest-tools/out/driver/* "$OUT/iso/DRIVER/"

# The Win98/Me display driver for the same adapter (doc 19, M10), staged as
# DRIVER9X\. A separate folder and not a second copy in DRIVER\: the ISO's
# rule is one folder per role, and "the display driver" is two roles here.
# Nothing in either folder is wanted by both families, and SETUP picks the
# folder from the Windows it is running on.
#
# It needs a second toolchain, Open Watcom, because a 16-bit NE `.drv` and
# a ring-0 LE `.vxd` are formats mingw cannot make. This script will not
# install it, so a host without it still builds a usable ISO with the 98
# driver missing and a note saying so. A silently smaller ISO is how a
# guest ends up being told a component is "not on this disc".
if drv9x_log="$("$ROOT/guest-tools/build-driver9x.sh" 2>&1)"; then
  # The HAL DLL too: the INF's CopyFiles names it, so a disc without it
  # gets an Update Driver wizard asking for d3dpt9hl.dll. The headless
  # tools copy it into the image themselves, so they do not catch this.
  mkdir -p "$OUT/iso/DRIVER9X" && cp "$ROOT"/guest-tools/out/driver9x/*.drv \
    "$ROOT"/guest-tools/out/driver9x/*.vxd "$ROOT"/guest-tools/out/driver9x/*.inf \
    "$ROOT"/guest-tools/out/driver9x/d3dpt9hl.dll "$OUT/iso/DRIVER9X/"
else
  # Say *why*, not just "no Watcom". The driver build fails for other
  # reasons too (a HAL that will not link, a bad export), and blaming
  # Watcom for those sends the reader looking in the wrong place.
  # build-driver9x.sh's own first line is "need Open Watcom …" when that
  # is the cause; otherwise its last lines are the real error.
  echo "note: the Win98 display driver is NOT on this ISO — build-driver9x.sh failed:" >&2
  printf '%s\n' "$drv9x_log" | tail -3 | sed 's/^/    /' >&2
fi

# SETUP.EXE at the root: the installer that reads the folders above and
# knows which of them this guest's Windows wants (guest-tools/src/setup.c).
# Its manifest (setup.manifest) asks Vista and 7 for an administrator and
# keeps their Program Compatibility Assistant quiet afterwards.
( cd "$ROOT/guest-tools/src" && i686-w64-mingw32-windres setup.rc -O coff -o "$OUT/setup-res.o" )
i686-w64-mingw32-gcc -O2 -Wall -o "$OUT/iso/setup.exe" "$ROOT/guest-tools/src/setup.c" "$OUT/setup-res.o" \
  -ladvapi32 -luser32 -lwinmm -lshell32

# Every binary on the disc, however deep and whatever case it was staged
# in (the per-game folders carry the names a game loads).
while IFS= read -r f; do check_crt "$f"; check_isa "$f"; done \
  < <(find "$OUT/iso" -type f \( -iname '*.dll' -o -iname '*.exe' \))

# The WDDM display driver for Windows 7 (track M18), when build/wddm/x86
# has it (build-windows.sh's wddm stage: MSVC through the EWDK, so Windows
# only; Linux and macOS fetch the PC's build for the same sources,
# scripts/windows-drivers.sh), staged as WDDM\: the kernel driver, the Direct3D 9 user-mode
# driver and the INF. Staged after the checks above, which are the mingw
# rules for Win9x / XP guest code: the user-mode DLL is MSVC with a static
# C runtime, whose own SSE2 routines are picked by a CPU check at run time
# (its code is /arch:IA32). SETUP installs it on 32-bit Windows 7 when the
# adapter has its interrupt (-device d3dpt-vga,irq=on), and the XP driver
# otherwise (setup.c, wddm_wanted).
WDDM_OUT="$ROOT/build/wddm/x86"
if [ -f "$WDDM_OUT/d3dptkmd.sys" ] && [ -f "$WDDM_OUT/d3dptumd.dll" ] && [ -f "$WDDM_OUT/d3dptkmd.inf" ]; then
  mkdir -p "$OUT/iso/WDDM"
  cp "$WDDM_OUT/d3dptkmd.sys" "$WDDM_OUT/d3dptumd.dll" "$WDDM_OUT/d3dptkmd.inf" "$OUT/iso/WDDM/"
else
  echo "note: the Windows 7 WDDM driver is not on this ISO (built on Windows only: build-windows.sh wddm; elsewhere scripts/windows-drivers.sh fetch wddm)" >&2
fi
# The same driver for 64-bit Windows 11 (track M20), from build/wddm/x64,
# as WDDM64\ with its installer, wddm-install.ps1, which test signs it in
# the guest. Nothing installs it by itself.
WDDM64_OUT="$ROOT/build/wddm/x64"
if [ -f "$WDDM64_OUT/d3dptkmd.sys" ] && [ -f "$WDDM64_OUT/d3dptumd.dll" ]; then
  mkdir -p "$OUT/iso/WDDM64"
  cp "$WDDM64_OUT/d3dptkmd.sys" "$WDDM64_OUT/d3dptumd.dll" "$ROOT/guest-tools/src/d3dptvid/wddm/km/d3dptkmd.inf" \
     "$ROOT/guest-tools/src/d3dptvid/wddm/wddm-install.ps1" "$OUT/iso/WDDM64/"
  # the 32-bit user-mode driver, for 32-bit Direct3D programs (WoW64)
  if [ -f "$WDDM64_OUT/d3dptumd32.dll" ]; then cp "$WDDM64_OUT/d3dptumd32.dll" "$OUT/iso/WDDM64/"; fi
fi
sed -e "s/@REV@/$REV/" "$ROOT/guest-tools/README-ISO.txt" \
  | crlf > "$OUT/iso/README.TXT"
# 8.3-safe upper-case names for Win9x
( cd "$OUT/iso" && find . -type f | while IFS= read -r f; do
    u="$(dirname "$f")/$(basename "$f" | tr a-z A-Z)"
    # through a temporary name: NTFS (MSYS2) may refuse a case-only rename
    [ "$f" = "$u" ] || { mv "$f" "$f.~" && mv "$f.~" "$u"; }; done )

ISO="$OUT/guest-tools-3dfx-$REV.iso"
if command -v xorriso >/dev/null; then
  xorriso -as mkisofs -o "$ISO" -V "GUESTTOOLS" -J -r "$OUT/iso" >/dev/null 2>&1
elif command -v genisoimage >/dev/null; then
  genisoimage -o "$ISO" -V "GUESTTOOLS" -J -r "$OUT/iso" >/dev/null 2>&1
elif command -v mkisofs >/dev/null; then
  mkisofs -o "$ISO" -V "GUESTTOOLS" -J -r "$OUT/iso" >/dev/null 2>&1
else
  echo "no ISO tool found; staged files are in $OUT/iso"; exit 0
fi
echo "==> $ISO"
