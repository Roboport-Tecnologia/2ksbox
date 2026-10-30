#!/usr/bin/env bash
# Build the Win98/Me display driver for the d3dpt-vga adapter (doc 19,
# ADR-012 / M10): the 16-bit DIB Engine mini display driver d3dpt9x.drv,
# the ring-0 mini-VDD d3dpt9v.vxd, the ring-3 HAL d3dpt9hl.dll and the INF,
# staged as guest-tools/out/driver9x/, plus the 9x probe programs.
#
# Unlike everything else in guest-tools/, the .drv and the VxD need Open
# Watcom, not mingw-w64: they are a 16-bit NE module and a ring-0 LE
# module, and mingw can produce neither format. Point WATCOM at an
# installed tree, or install one where this script looks by default:
#
#   WATCOM=$HOME/.local/opt/open-watcom guest-tools/build-driver9x.sh
#
# Open Watcom v2 ships host binaries for all of our machines in one
# tarball: https://github.com/open-watcom/open-watcom-v2 (the Last-CI-build
# release's ow-snapshot.tar.xz unpacks ready to use, Linux x86-64 in binl64
# and macOS arm64 in armo64). So this builds on the Air, on the rig and on
# Windows in MSYS2 (binnt64; WATCOM=/c/WATCOM wherever it was unpacked).
#
# The headers this builds against are in src/d3dptvid/ddk9x/. No Microsoft
# DDK, same rule as the XP driver (doc 15). dibeng.lib is made here by wlib
# from a text import list, so the DIB Engine needs no DDK either.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
# A step that fails without a word of its own (a check that exits after
# printing to a stdout nobody shows) still says where it stopped.
set -E
trap 'echo "$(basename "$0"): stopped at line $LINENO: $BASH_COMMAND" >&2' ERR

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/guest-tools/msys2-i686.sh"
SRC="$ROOT/guest-tools/src/d3dptvid/w9x"
DDK="$ROOT/guest-tools/src/d3dptvid/ddk9x"
OUT="$ROOT/guest-tools/out/driver9x"
WATCOM="${WATCOM:-$HOME/.local/opt/open-watcom}"

# The snapshot carries a host directory per platform, and the same tarball
# has all of them: Linux x86-64 (binl64), macOS arm64 (armo64) and macOS
# x86-64 (bino64). Nothing else about this build is host-specific.
case "$(uname -s)/$(uname -m)" in
  Darwin/arm64)   OWBIN=armo64 ;;
  Darwin/x86_64)  OWBIN=bino64 ;;
  MINGW*|MSYS*)   OWBIN=binnt64 ;;
  *)              OWBIN=binl64 ;;
esac

[ -x "$WATCOM/$OWBIN/wcc" ] || {
  echo "need Open Watcom (WATCOM=$WATCOM has no $OWBIN/wcc)"
  echo "see the header of $0 for where to get it"
  exit 1
}
export WATCOM
export PATH="$WATCOM/$OWBIN:$PATH"
export INCLUDE="$WATCOM/h:$WATCOM/h/win:$DDK"
export EDPATH="$WATCOM/eddat"
export WIPFC="$WATCOM/wipfc"

rm -rf "$OUT" && mkdir -p "$OUT"
BUILD="$OUT/obj"
mkdir -p "$BUILD"

# 16-bit, Windows entry conventions (-zW), loadds on exports (-zu is for
# the DS != SS the DLL model needs), stack checking off, small model.
# -wcd=303: the Windows entry points have signatures we do not choose, so
# an unused parameter is not news. wcc drops its .err beside the cwd, hence
# the subshells.
CFLAGS=(-q -wx -wcd=303 -s -zu -zls -zW -6 -fp6 -I"$DDK" -I"$SRC")

echo "==> d3dpt9x.obj"
( cd "$BUILD" && wcc "${CFLAGS[@]}" -fo=d3dpt9x.obj "$SRC/d3dpt9x.c" )
echo "==> d3dpt9dd.obj (the DirectDraw escapes)"
( cd "$BUILD" && wcc "${CFLAGS[@]}" -fo=d3dpt9dd.obj "$SRC/d3dpt9dd.c" )
echo "==> dibthunk.obj"
( cd "$BUILD" && wasm -q -fo=dibthunk.obj "$SRC/dibthunk.asm" )

# The resource blobs: GDI and USER read the machine metrics, the colour
# table and the system fonts out of the .drv itself (doc 19). Each is a C
# file of pure data, linked as a raw DOS binary to get the bytes out.
for r in config colortab fonts fonts120; do
  echo "==> res/$r.bin"
  ( cd "$BUILD" && wcc -q -zu -zls -6 -I"$DDK" -I"$SRC/res" -fo=$r.obj "$SRC/res/$r.c" \
    && wlink op quiet disable 1014, 1023 name $r.bin sys dos output raw file $r.obj )
done

echo "==> d3dpt9x.res"
( cd "$BUILD" && cp "$SRC/res/d3dpt9x.rc" . && wrc -q -r -ad -bt=windows -fo=d3dpt9x.res \
    -I"$WATCOM/h/win" d3dpt9x.rc )

echo "==> dibeng.lib (import library, from the text list)"
# An @file is a path only Watcom reads: on Windows it has to be one Windows
# can open, and MSYS2's argument conversion does not reach behind the @.
LBC="$DDK/dibeng.lbc"
[ -z "${MSYSTEM:-}" ] || LBC="$(cygpath -m "$LBC")"
( cd "$BUILD" && wlib -b -q -n -fo -ii @"$LBC" dibeng.lib >/dev/null )

echo "==> d3dpt9x.drv (16-bit NE, module DISPLAY)"
# wlink reads its directives from a real file (@name), not a pipe.
cat > "$BUILD/d3dpt9x.lnk" <<'LNK'
system windows dll initglobal
file d3dpt9x.obj
file d3dpt9dd.obj
file dibthunk.obj
name d3dpt9x.drv
option map=d3dpt9x.map
library dibeng.lib
option oneautodata
option heapsize=0
option nodefaultlibs
option modname=DISPLAY
option description 'DISPLAY : 100, 96, 96 : 2ksbox d3dpt-vga mini display driver.'
segment class 'DATA' preload fixed
segment type data preload fixed
segment '_TEXT' preload fixed shared
export BitBlt.1
export ColorInfo.2
export Control.3
export Disable.4
export Enable.5
export EnumDFonts.6
export EnumObj.7
export Output.8
export Pixel.9
export RealizeObject.10
export StrBlt.11
export ScanLR.12
export DeviceMode.13
export ExtTextOut.14
export GetCharWidth.15
export DeviceBitmap.16
export FastBorder.17
export SetAttribute.18
export DibBlt.19
export CreateDIBitmap.20
export DibToDevice.21
export SetPalette.22
export GetPalette.23
export SetPaletteTranslate.24
export GetPaletteTranslate.25
export UpdateColors.26
export StretchBlt.27
export StretchDIBits.28
export SelectBitmap.29
export BitmapBits.30
export ReEnable.31
export Inquire.101
export SetCursor.102
export MoveCursor.103
export CheckCursor.104
export GetDriverResourceID.450
export UserRepaintDisable.500
export ValidateMode.700
import GlobalSmartPageLock KERNEL.230
import AllocCStoDSAlias KERNEL.170
LNK
( cd "$BUILD" && wlink op quiet, start=DriverInit_ disable 2055 @d3dpt9x.lnk )

# The ring-0 half: a dynamically loadable VxD (LE), 32-bit flat, no CRT.
# -zls drops the runtime references, -s the stack checks; the segments are
# PRELOAD NONDISCARDABLE because a mini-VDD must stay resident.
echo "==> d3dptvxd.obj (32-bit, ring 0)"
( cd "$BUILD" && wcc386 -q -wx -wcd=303 -s -zls -mf -6s -fp6 -ei -zp1 \
    -I"$DDK" -I"$SRC" -fo=d3dptvxd.obj "$SRC/d3dptvxd.c" )

# wlink's LE output is not quite a VxD yet. The fix is the object table,
# not the header (this is what vmdisp9x's `fixlink -vxd32` does): every
# object must be marked executable, and every object's base virtual
# address must be zero, because a VxD is flat and all its pages start at
# the beginning. The module-type field is set to what a real Windows 98 VxD
# carries as well (checked against the guest's own VJOYD.VXD and
# FXMEMMAP.VXD, both 0x38000). Then the DDB must sit at offset 0 of the
# VxD's one object, where the entry table points and where the VMM's
# loader looks. Anything the compiler emits into the same segment ahead of
# it (a string literal, a const array, a static without an initialiser)
# pushes it off, and the VMM then declines the module in complete silence
# (doc 19 §12). The map says where it went, so the
# build checks. Two VxDs go through this: the mini-VDD and the blue-screen
# trigger of tools/win98-bsod-test.sh.
link_vxd() {   # link_vxd <name> <object>: $BUILD/<name>.vxd from $BUILD/<object>.obj
  local name="$1" obj="$2"
  cat > "$BUILD/$name.lnk" <<LNK
system win_vxd dynamic
option map=$name.map
option nodefaultlibs
name $name.vxd
file $obj.obj
segment '_LTEXT' PRELOAD NONDISCARDABLE IOPL
segment '_TEXT'  PRELOAD NONDISCARDABLE IOPL
segment '_DATA'  PRELOAD NONDISCARDABLE IOPL
segment 'CONST'  PRELOAD NONDISCARDABLE IOPL
segment 'CONST2' PRELOAD NONDISCARDABLE IOPL
export VXD_DDB.1
LNK
  ( cd "$BUILD" && wlink op quiet @$name.lnk )
  python3 - "$BUILD/$name.vxd" <<'PYVXD'
import struct, sys
p = sys.argv[1]
f = bytearray(open(p, 'rb').read())
le = struct.unpack_from('<I', f, 0x3c)[0]
assert f[le:le+2] == b'LE', 'not an LE module: %r' % f[le:le+2]

flags = struct.unpack_from('<I', f, le + 0x10)[0]
struct.pack_into('<I', f, le + 0x10, flags | 0x00038000)

# The entry table's one bundle exports the DDB. wlink types it as a
# 16-bit/call-gate entry because the symbol is data; a real VxD's is a
# 32-bit entry (FXMEMMAP.VXD again). The record bytes are the same either
# way when the offset is zero, so this is just the type byte.
ent = le + struct.unpack_from('<I', f, le + 0x5c)[0]
if f[ent] == 1 and f[ent+1] == 2:
    f[ent+1] = 3
    print("   entry table: 16-bit bundle -> 32-bit")

objtab = le + struct.unpack_from('<I', f, le + 0x40)[0]
nobj   = struct.unpack_from('<I', f, le + 0x44)[0]
for i in range(nobj):
    o = objtab + i * 24                       # sizeof(LE_object)
    size, addr, oflags = struct.unpack_from('<III', f, o)
    struct.pack_into('<II', f, o + 4, 0, oflags | 0x0004)   # addr = 0, executable
    print("   object %d: size %d, addr %d -> 0, flags %08x -> %08x"
          % (i, size, addr, oflags, oflags | 4))
open(p, 'wb').write(f)
PYVXD
  awk -v n="$name" '/^0001:00000000 +VXD_DDB$/ { found = 1 }
       END { if (!found) {
          print n ".vxd: VXD_DDB is not at 0001:00000000 — the VMM will"
          print "  ignore this module without a word. Something in _LTEXT is"
          print "  emitted ahead of it; see the map:"
          exit 1 } }' "$BUILD/$name.map" || {
    grep -m5 -A4 "Module: $obj" "$BUILD/$name.map"
    exit 1
  }
  cp "$BUILD/$name.vxd" "$OUT/"
}

echo "==> d3dpt9v.vxd"
link_vxd d3dpt9v d3dptvxd

echo "==> bsodvxd.vxd (blue-screens on load: the trigger for tools/win98-bsod-test.sh)"
( cd "$BUILD" && wcc386 -q -wx -wcd=303 -s -zls -mf -6s -fp6 -ei -zp1 \
    -I"$DDK" -I"$SRC" -fo=bsodvxd.obj "$SRC/bsodvxd.c" )
link_vxd bsodvxd bsodvxd

echo "==> bsodtmr.vxd (the same fault from a timer callback: a blue screen with no screen switch, WHEN=event)"
( cd "$BUILD" && wcc386 -q -wx -wcd=303 -s -zls -mf -6s -fp6 -ei -zp1 -DBSOD_TIMER \
    -I"$DDK" -I"$SRC" -fo=bsodtmr.obj "$SRC/bsodvxd.c" )
link_vxd bsodtmr bsodtmr

echo "==> resources into the module"
( cd "$BUILD" && wrc -q d3dpt9x.res d3dpt9x.drv )

# The linker stamps "expected Windows 3.00" into the NE header. GDI loads the
# driver either way, but the DIB Engine treats a 3.x module as a 3.x driver
# (vmdisp9x hits the same thing and calls the fix -40): say 4.00.
python3 - "$BUILD/d3dpt9x.drv" <<'PYFIX'
import struct, sys
p = sys.argv[1]
f = bytearray(open(p, 'rb').read())
ne = struct.unpack_from('<I', f, 0x3c)[0]
assert f[ne:ne+2] == b'NE', 'not an NE module'
f[ne+0x3e] = 0        # minor
f[ne+0x3f] = 4        # major -> 4.00
# A display driver's DGROUP is PRELOAD FIXED SINGLE with no local heap
# (CIRRUSMM.DRV and every other one on the guest say 0); wlink insists on
# giving a DLL 1 KiB and `option heapsize=0` does not move it.
struct.pack_into('<H', f, ne + 0x10, 0)
open(p, 'wb').write(f)
PYFIX

# Every internal relocation must name a segment the NE segment table has.
# wlink can emit one that does not: a segment that ends up empty is dropped
# from the table while the relocations that referenced it keep its old
# index, and KERNEL then refuses the whole module without a word (doc 19
# §13). Nothing downstream complains, so the build does.
python3 - "$BUILD/d3dpt9x.drv" <<'PYCHK'
import struct, sys
p = sys.argv[1]
f = open(p, 'rb').read()
ne = struct.unpack_from('<I', f, 0x3c)[0]
segcount,  = struct.unpack_from('<H', f, ne + 0x1c)
segtaboff, = struct.unpack_from('<H', f, ne + 0x22)
align = 1 << (struct.unpack_from('<H', f, ne + 0x32)[0] or 9)
bad = 0
for s in range(segcount):
    off, size, flags, _ = struct.unpack_from('<HHHH', f, ne + segtaboff + s * 8)
    if not flags & 0x0100:                     # no relocation records
        continue
    base = off * align + size
    n, = struct.unpack_from('<H', f, base)
    for i in range(n):
        r = base + 2 + i * 8
        if f[r + 1] & 3:                       # not an internal reference
            continue
        seg = f[r + 4]
        if seg != 0xff and not 1 <= seg <= segcount:
            print("   segment %d relocation at 0x%04x names segment %d of %d"
                  % (s + 1, struct.unpack_from('<H', f, r + 2)[0], seg, segcount))
            bad += 1
if bad:
    sys.exit("d3dpt9x.drv: %d dangling relocation(s) — KERNEL will refuse to "
             "load this module. Check the .map for an empty segment." % bad)
PYCHK

# Every export must load DGROUP before it touches a global. Open Watcom
# takes a function's attributes from its *first* declaration, so a DDK
# prototype without `__loadds` silently strips it from the definition, and
# the export then reads the driver's variables through whatever DS its
# caller had. Display Settings thunks down from 32-bit code, so that DS is
# not ours, and `ValidateMode` turned a stack word into a selector and GPFed
# on the first mode it was asked about (doc 19 §18). The three shapes
# an entry has: a far `jmp` to the DIB Engine (`ea`), a thunk that loads ES
# with DGROUP to push the PDEVICE (`b8 DGROUP 8e c0`), or a C function whose
# prologue loads DS (`b8 DGROUP 8e d8`). Anything else is a function that
# lost its `__loadds`.
python3 - "$BUILD/d3dpt9x.drv" <<'PYDS'
import struct, sys
p = sys.argv[1]
f = open(p, 'rb').read()
ne = struct.unpack_from('<I', f, 0x3c)[0]
segtaboff, = struct.unpack_from('<H', f, ne + 0x22)
align = 1 << (struct.unpack_from('<H', f, ne + 0x32)[0] or 9)
segs = [struct.unpack_from('<HHHH', f, ne + segtaboff + s * 8)
        for s in range(struct.unpack_from('<H', f, ne + 0x1c)[0])]
enttab, entlen = struct.unpack_from('<HH', f, ne + 0x04)
q, ordinal, bad = ne + enttab, 1, 0
while q < ne + enttab + entlen:
    count, kind = f[q], f[q + 1]
    q += 2
    if count == 0:
        break
    if kind == 0:                              # unused ordinals
        ordinal += count
        continue
    for _ in range(count):
        if kind == 0xff:                       # movable entry
            seg, off = f[q + 3], struct.unpack_from('<H', f, q + 4)[0]
            q += 6
        else:                                  # fixed entry in segment `kind`
            seg, off = kind, struct.unpack_from('<H', f, q + 1)[0]
            q += 3
        code = f[segs[seg - 1][0] * align + off:][:12]
        if code[:1] != b'\xea' and b'\xb8\xff\xff\x8e' not in code[:8]:
            print("   export %d at %d:%04x starts %s — no DGROUP load"
                  % (ordinal, seg, off, code[:8].hex(' ')))
            bad += 1
        ordinal += 1
if bad:
    sys.exit("d3dpt9x.drv: %d export(s) run on the caller's DS — a "
             "`__loadds` was lost to an earlier prototype." % bad)
PYDS

# ---------------------------------------------------------------------------
# The ring-3 HAL DLL. This one is *not* Watcom's: on 9x the DirectDraw /
# Direct3D HAL is an ordinary user-mode Win32 DLL loaded into the game's
# own process (doc 19 §1), so it builds with the same i686 mingw-w64 the
# XP driver and the guest wrappers use. It is the only 9x binary that
# links the OS-independent core. A host with no mingw still gets a working
# display driver, without DirectDraw, and the build says so.
#
# Note the include path: *not* $DDK. Those are the 16-bit interface
# headers, full of `__far`; the 32-bit DirectDraw driver headers
# (ddrawi.h, d3dhal.h, dmemmgr.h) are ones mingw-w64 ships itself, and
# they are the ones this half wants.
HALCC=i686-w64-mingw32-gcc
# The base must be above 2 GiB. DirectDraw loads this DLL and calls
# DriverInit in `DDHELP.EXE`, not in the application, so the callback
# addresses it publishes are flat pointers in DDHELP's address
# space, and the HALINFO validator that IsBadCodePtr's them runs in the
# *game's*. Windows 9x maps a DLL based in the shared arena (0x80000000 -
# 0xBFFFFFFF) at one address for every process, which is what makes one
# set of pointers mean the same thing in both; a DLL based below that is
# private to whoever loaded it and every callback it publishes is a bad
# pointer everywhere else. The symptom is a HAL that is refused with no
# message at all (doc 19 §22). The reference driver bases its at
# 0xB00B0000 for the same reason.
#
# The address has to be one this Windows will actually give. A base it
# will not honour is relocated into the private arena instead, and the DLL
# then publishes callbacks no other process can reach. 0xB3D00000 and
# 0xB00D0000 both came back as 0x00b50000 on our guest, and stripping the
# relocation table to force the issue only made `LoadLibrary` fail
# outright. So the value is the reference driver's own, which is known to
# load on a 9x guest, and `DriverInit` checks the base it actually got and
# says so rather than running on a bad one.
HAL_BASE=0xB00B0000
if command -v "$HALCC" >/dev/null; then
  CORE="$ROOT/guest-tools/src/d3dptvid/core"
  "$HALCC" -O2 -Wall -Wno-unused-function -shared -nostdlib -ffreestanding \
     -fno-stack-protector -mno-stack-arg-probe -fno-asynchronous-unwind-tables \
     -fno-ident -march=pentium3 -mtune=generic -fno-tree-loop-distribute-patterns \
     -Wl,--enable-stdcall-fixup -Wl,--entry,_DllMain@12 \
     -Wl,--image-base,$HAL_BASE \
     -Wl,--disable-dynamicbase,--disable-nxcompat,--subsystem,windows \
     -I"$SRC" -I"$CORE" \
     -o "$BUILD/d3dpt9hl.dll" "$SRC/d3dpthal.c" "$SRC/d3dpthal.def" \
     "$CORE/core_flip.c" "$CORE/core_caps.c" "$CORE/core_surf.c" \
     "$CORE/core_ctx.c" "$CORE/core_dp2.c" "$SRC/../kcrt.c" \
     -lgcc -lkernel32
  # `-nostdlib` drops the default libraries, so kernel32 is named on
  # purpose: it is the one import this DLL is allowed (the check below
  # enforces exactly that), and the OS services the core asks for
  # (allocate, free, a performance counter) all come from it.
  #
  # Loaded into every game's address space, so it must pull in no runtime
  # and must not reach past the pentium3 floor the guests are built to.
  bad="$(i686-w64-mingw32-objdump -p "$BUILD/d3dpt9hl.dll" | awk '/DLL Name:/ {print $3}' \
         | grep -ivE '^(kernel32\.dll)$' || true)"
  [ -z "$bad" ] || { echo "ERROR: d3dpt9hl.dll imports from $bad"; exit 1; }
  n=$(i686-w64-mingw32-objdump -d "$BUILD/d3dpt9hl.dll" | grep -cE '\b(movdq[au]|movapd|movupd|pshufd|paddq|cvtsd2|cvtsi2sd|xorpd|andpd|popcnt|pshufb)\b' || true)
  [ "$n" -eq 0 ] || { echo "ERROR: d3dpt9hl.dll contains $n SSE2+ instructions (pentium3 floor)"; exit 1; }
  # `\bDriverInit$`, not a column-by-column match of objdump's export line.
  # The strict form (`\[.*\]  *[0-9a-f]+ DriverInit$`) depends on how many
  # hex digits objdump prints for the export RVA and how it spaces them, and
  # it failed twice on a good DLL that a rebuild with identical inputs
  # passed. The name appears nowhere else in this output.
  #
  # And it must not pipe objdump *into* `grep -q`: grep closes the pipe on
  # the match, objdump takes SIGPIPE (141), and under `set -o pipefail` the
  # pipeline then reports failure even though the export was found. That
  # race failed about one build in eight on a good DLL, under the CPU load
  # of a full build. Every check here reads objdump's whole output first;
  # this one matches against a here-string so there is no upstream process
  # to signal.
  hlexp="$(i686-w64-mingw32-objdump -p "$BUILD/d3dpt9hl.dll")"
  grep -qE '\bDriverInit(@4)?$' <<<"$hlexp" \
    || { echo "ERROR: d3dpt9hl.dll does not export DriverInit"; exit 1; }
  # The shared-arena base, checked rather than assumed: getting it wrong
  # costs a HAL that is silently refused, and nothing downstream says so.
  ib=$(i686-w64-mingw32-objdump -p "$BUILD/d3dpt9hl.dll" | awk '/ImageBase/ {print $2}')
  [ $((16#$ib)) -ge $((0x80000000)) ] || {
    echo "ERROR: d3dpt9hl.dll is based at 0x$ib, below the Win9x shared arena;"
    echo "       DDHELP's callbacks would be bad pointers in every game"; exit 1; }

  # A freestanding DLL has no CRT startup, so the entry point has to be
  # named by hand, and ld only *warns* when it cannot find one, leaving
  # AddressOfEntryPoint zero. Windows then calls address zero the moment
  # DirectDraw loads this into a game, which on 9x is a silent reboot.
  ep=$(i686-w64-mingw32-objdump -p "$BUILD/d3dpt9hl.dll" | awk '/AddressOfEntryPoint/ {print $2}')
  [ -n "$ep" ] && [ "$ep" != "00000000" ] \
    || { echo "ERROR: d3dpt9hl.dll has no entry point (AddressOfEntryPoint $ep)"; exit 1; }

  # Windows 9x relocates any DLL in the shared arena (>= 0x80000000) down into
  # the private per-process arena unless every section is marked shared
  # (IMAGE_SCN_MEM_SHARED = 0x10000000). DirectDraw loads the HAL in DDHELP.EXE,
  # but games validate callbacks in their own processes. Marking all sections
  # shared keeps the DLL at HAL_BASE across all processes.
  python3 - "$BUILD/d3dpt9hl.dll" <<'PYPE'
import struct, sys
p = sys.argv[1]
f = bytearray(open(p, 'rb').read())
pe = struct.unpack_from('<I', f, 0x3c)[0]
assert f[pe:pe+4] == b'PE\x00\x00', 'not a PE module: %r' % f[pe:pe+4]

coff = pe + 4
num_sections = struct.unpack_from('<H', f, coff + 2)[0]
opt_hdr_size = struct.unpack_from('<H', f, coff + 16)[0]
opt_hdr = coff + 20
sec_tab = opt_hdr + opt_hdr_size
IMAGE_SCN_MEM_SHARED = 0x10000000

for i in range(num_sections):
    o = sec_tab + i * 40
    name = f[o:o+8].rstrip(b'\x00').decode('latin1')
    chars = struct.unpack_from('<I', f, o + 36)[0]
    struct.pack_into('<I', f, o + 36, chars | IMAGE_SCN_MEM_SHARED)
    print("   section %-8s: flags %08x -> %08x (shared)" % (name, chars, chars | IMAGE_SCN_MEM_SHARED))

chk_off = opt_hdr + 64
struct.pack_into('<I', f, chk_off, 0)
flen = len(f)
padded = f if flen % 2 == 0 else f + b'\x00'
total = 0
for i in range(0, len(padded), 2):
    word, = struct.unpack_from('<H', padded, i)
    total += word
    total = (total >> 16) + (total & 0xffff)
total = (total >> 16) + (total & 0xffff)
total = (total + flen) & 0xffffffff
struct.pack_into('<I', f, chk_off, total)
print("   PE checksum: -> %08x" % total)

open(p, 'wb').write(f)
PYPE

  cp "$BUILD/d3dpt9hl.dll" "$OUT/"

  # The smallest thing that makes DirectDraw initialise, so that the
  # escapes above are reached at all: a Win98 desktop never calls
  # DirectDrawCreate on its own (doc 19 §2).
  echo "==> ddprobe.exe (makes DirectDraw initialise, for the harness)"
  "$HALCC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os \
     -march=pentium3 -mtune=generic -mwindows \
     -o "$OUT/ddprobe.exe" "$SRC/ddprobe.c" -lddraw -ldxguid -luser32

  # The monitor power-down without the wait: doc 19 §41's test, which
  # otherwise depends on the power scheme in whichever image it is run on.
  echo "==> pwrprobe.exe (asks for the monitor power-down the idle time-out asks for)"
  "$HALCC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os \
     -march=pentium3 -mtune=generic -mwindows \
     -o "$OUT/pwrprobe.exe" "$SRC/pwrprobe.c" -luser32

  echo "==> devcaps.exe (every DirectDraw device's caps, for diffing ours against another driver)"
  "$HALCC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os \
     -march=pentium3 -mtune=generic -mwindows \
     -o "$OUT/devcaps.exe" "$SRC/devcaps.c" -lddraw -ldxguid -luser32

  echo "==> ebtest.exe (DirectX 3 execute buffers and texture handles)"
  "$HALCC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os \
     -march=pentium3 -mtune=generic \
     -o "$OUT/ebtest.exe" "$ROOT/guest-tools/src/d3dptvid/ebtest.c" -lddraw -ldxguid -lgdi32 -luser32

  echo "==> d3d7test.exe (the DX7 HAL scene, the oracle against d3dpt-dp2-test)"
  "$HALCC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os \
     -march=pentium3 -mtune=generic \
     -o "$OUT/d3d7test.exe" "$ROOT/guest-tools/src/d3dptvid/d3d7test.c" -lddraw -ldxguid -lgdi32 -luser32

  # The DX8 half. Reachable on 98 only because a 2ksbox Win98 machine runs
  # DirectX 9 (doc 19 §25): these need d3d8.dll, which the in-box 6.1 has not
  # got, and they exercise the GDI2 negotiation GetDriverInfo answers.
  echo "==> setbpp.exe (change the desktop depth from a batch file)"
  "$HALCC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os \
     -march=pentium3 -mtune=generic \
     -o "$OUT/setbpp.exe" "$SRC/setbpp.c" -lgdi32 -luser32

  echo "==> gdiprobe.exe (which GDI operation the DIB Engine path gets wrong)"
  "$HALCC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os \
     -march=pentium3 -mtune=generic \
     -o "$OUT/gdiprobe.exe" "$SRC/gdiprobe.c" -lgdi32 -luser32

  echo "==> bsod.exe (blue-screens an unpatched Win98 on purpose, for win98-bsod-test.sh)"
  "$HALCC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os \
     -march=pentium3 -mtune=generic -mwindows \
     -o "$OUT/bsod.exe" "$SRC/bsod.c" -luser32

  echo "==> cktest.exe (palettized textures and colour keying through the DX7 HAL)"
  "$HALCC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os \
     -march=pentium3 -mtune=generic \
     -o "$OUT/cktest.exe" "$ROOT/guest-tools/src/d3dptvid/cktest.c" -lddraw -ldxguid -lgdi32 -luser32

  echo "==> zfilltest.exe (a Z buffer reset by a DirectDraw depth fill, doc 19 §34)"
  "$HALCC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os \
     -march=pentium3 -mtune=generic \
     -o "$OUT/zfilltest.exe" "$ROOT/guest-tools/src/d3dptvid/zfilltest.c" -lddraw -ldxguid -lgdi32 -luser32

  echo "==> dxttest.exe (which texture formats d3d8.dll creates, per pool)"
  "$HALCC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os \
     -march=pentium3 -mtune=generic \
     -o "$OUT/dxttest.exe" "$ROOT/guest-tools/src/d3dptvid/dxttest.c" -ld3d8 -lgdi32 -luser32

  echo "==> d3dgame8.exe (the M4 DX8 reference scene, no wrapper DLL)"
  "$HALCC" -O2 -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os \
     -march=pentium3 -mtune=generic \
     -o "$OUT/d3dgame8.exe" "$ROOT/guest-tools/src/d3dgame8.c" -ld3d8 -lgdi32 -luser32

  echo "==> shtest.exe (vertex / pixel shaders 1.x through d3d8.dll on the DX8 DDI)"
  "$HALCC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os \
     -march=pentium3 -mtune=generic \
     -o "$OUT/shtest.exe" "$ROOT/guest-tools/src/d3dptvid/shtest.c" -ld3d8 -lgdi32 -luser32

  # the DX8 feature probes (d3d8probe.h), as build-driver.sh builds them
  for t in cubetest strmtest voltest fmttest bumptest sprtest anistest patchtst mgdtest; do
    echo "==> $t.exe (a DX8 feature probe through d3d8.dll on the DX8 DDI)"
    "$HALCC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os \
       -march=pentium3 -mtune=generic \
       -o "$OUT/$t.exe" "$ROOT/guest-tools/src/d3dptvid/$t.c" -ld3d8 -lgdi32 -luser32
  done
else
  echo "==> no $HALCC: skipping d3dpt9hl.dll (no DirectDraw on 9x from this build)"
fi

cp "$BUILD/d3dpt9x.drv" "$BUILD/d3dpt9v.vxd" "$OUT/"
cp "$SRC/d3dpt9x.inf" "$OUT/" 2>/dev/null || true

echo
echo "==> $OUT/d3dpt9x.drv"
ls -la "$OUT"
