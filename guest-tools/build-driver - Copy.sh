#!/usr/bin/env bash
# Build the XP display driver for the d3dpt-vga adapter (doc 15, ADR-008 /
# M7a): the video miniport d3dptvid.sys, the display driver d3dptdisp.dll,
# the INF and the DRVINST.EXE installer, staged as guest-tools/out/driver/
# and a small ISO (guest-tools/out/d3dpt-driver.iso) for fast iteration.
# build-wrappers.sh calls this too and copies the folder into the big ISO
# as DRIVER\.
#
# Kernel-mode PE files with GCC: no CRT (-nostdlib -ffreestanding), the
# native subsystem, an explicit entry point, imports only from
# videoprt.sys (miniport) / win32k.sys (display driver). mingw-w64 ships
# the DDK headers and import libraries. Same toolchain as the wrappers;
# needs i686-w64-mingw32-gcc, xorriso (or genisoimage/mkisofs) for the ISO.
set -euo pipefail
# A step that fails without a word of its own (a check that exits after
# printing to a stdout nobody shows) still says where it stopped.
set -E
trap 'echo "$(basename "$0"): stopped at line $LINENO: $BASH_COMMAND" >&2' ERR

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/guest-tools/msys2-i686.sh"
SRC="$ROOT/guest-tools/src/d3dptvid"
NT="$SRC/nt"        # the NT layer: the miniport and the display driver
CORE="$SRC/core"    # the OS-independent core both this and the 9x HAL link
CORE_SRC=("$CORE/core_dp2.c" "$CORE/core_surf.c" "$CORE/core_caps.c" "$CORE/core_ctx.c" "$CORE/core_flip.c")
OUT="$ROOT/guest-tools/out/driver"
CC=i686-w64-mingw32-gcc
command -v "$CC" >/dev/null || { echo "need $CC (mingw-w64)"; exit 1; }
# The headers sit under the toolchain's sysroot: /usr/i686-w64-mingw32 on
# Arch, Cellar/mingw-w64/<ver>/toolchain-i686/i686-w64-mingw32 on Homebrew
# (where the compiler in /opt/homebrew/bin is only a symlink, so deriving the
# prefix from the binary's location finds nothing).
DDK_INC="$("$CC" -print-sysroot 2>/dev/null)/i686-w64-mingw32/include/ddk"
[ -f "$DDK_INC/video.h" ] || DDK_INC="$(dirname "$(dirname "$(command -v "$CC")")")/i686-w64-mingw32/include/ddk"
[ -f "$DDK_INC/video.h" ] || DDK_INC="/usr/i686-w64-mingw32/include/ddk"
[ -f "$DDK_INC/video.h" ] || DDK_INC="/mingw32/include/ddk"   # MSYS2
[ -f "$DDK_INC/video.h" ] || { echo "mingw-w64 DDK headers (ddk/video.h) not found"; exit 1; }

# kernel mode: no stack probes (no __chkstk in the kernel), no stack
# protector, no unwind tables; -lgcc for the odd helper (64-bit shifts).
KFLAGS=(-O2 -Wall -Wno-unused-function -nostdlib -shared -ffreestanding
        -fno-stack-protector -mno-stack-arg-probe -fno-asynchronous-unwind-tables
        -fno-ident -march=pentium3 -mtune=generic -fno-tree-loop-distribute-patterns
        -Wl,--enable-stdcall-fixup
        -Wl,--subsystem,native -Wl,--image-base,0x10000
        -Wl,--major-os-version,5 -Wl,--minor-os-version,1
        -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,1)

rm -rf "$OUT" && mkdir -p "$OUT"
echo "==> d3dptvid.sys (video miniport)"
"$CC" "${KFLAGS[@]}" -I"$DDK_INC" -Wl,--entry,_DriverEntry@8 -Wl,--exclude-all-symbols \
  -o "$OUT/d3dptvid.sys" "$NT/d3dptvid.c" "$SRC/kcrt.c" -lvideoprt -lntoskrnl -lgcc
# The core is compiled on its own first, and then asked what it needs from
# outside itself. On NT it is linked into a kernel DLL and on 9x into a
# ring-3 one, so it may call no operating-system service at all (doc 19,
# "The split"): every undefined symbol it has must be one of its own, one
# of the d3dpt_os_* hooks the per-OS layer provides, or memcpy / memset.
# Nothing enforces that in the source, and a single Eng* call that slipped
# in would build here and fault on 9x, where there is no win32k at all.
echo "==> the OS-independent core"
CORE_OBJ=()
for c in "${CORE_SRC[@]}"; do
  o="$OUT/$(basename "${c%.c}").o"
  "$CC" -O2 -Wall -Wno-unused-function -ffreestanding -fno-stack-protector -mno-stack-arg-probe \
       -fno-asynchronous-unwind-tables -fno-ident -march=pentium3 -mtune=generic \
       -fno-tree-loop-distribute-patterns -I"$SRC/ddk" -c -o "$o" "$c"
  CORE_OBJ+=("$o")
done
bad="$(i686-w64-mingw32-nm --undefined-only "${CORE_OBJ[@]}" | awk '{print $2}' | sort -u \
       | grep -vE '^_?(memcpy|memset|d3dpt_os_[a-z_]+)$' \
       | while read -r sym; do
           i686-w64-mingw32-nm --defined-only "${CORE_OBJ[@]}" | awk '{print $3}' | grep -qx "$sym" || echo "$sym"
         done)"
if [ -n "$bad" ]; then
  echo "ERROR: the core calls out of itself: $bad"
  echo "       (it must reach the OS only through the d3dpt_os_* hooks)"
  exit 1
fi

echo "==> d3dptdisp.dll (display driver)"
"$CC" "${KFLAGS[@]}" -I"$SRC/ddk" -Wl,--entry,_DrvEnableDriver@12 -Wl,--kill-at \
  -o "$OUT/d3dptdisp.dll" "$NT/d3dptdisp.c" "${CORE_OBJ[@]}" "$SRC/kcrt.c" "$NT/d3dptdisp.def" -lwin32k -lgcc
rm -f "${CORE_OBJ[@]}"
echo "==> drvinst.exe (installer, user mode, msvcrt)"
"$CC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os -march=pentium3 -mtune=generic \
  -o "$OUT/drvinst.exe" "$SRC/drvinst.c" -ladvapi32 -luser32
echo "==> setmode.exe (mode switch from a script)"
"$CC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os -march=pentium3 -mtune=generic \
  -o "$OUT/setmode.exe" "$SRC/setmode.c" -luser32
echo "==> d3d7test.exe (Direct3D 7 HAL test)"
"$CC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os -march=pentium3 -mtune=generic \
  -o "$OUT/d3d7test.exe" "$SRC/d3d7test.c" -lddraw -ldxguid -lgdi32 -luser32
echo "==> ddtest.exe (DirectDraw 7 flip-chain test)"
"$CC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os -march=pentium3 -mtune=generic \
  -o "$OUT/ddtest.exe" "$SRC/ddtest.c" -lddraw -ldxguid -lgdi32 -luser32
echo "==> ditest.exe (DirectInput keyboard under load)"
"$CC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os -march=pentium3 -mtune=generic \
  -o "$OUT/ditest.exe" "$SRC/ditest.c" -ldinput -lddraw -ldxguid -lgdi32 -luser32
echo "==> dxttest.exe (which texture formats d3d8.dll creates, per pool)"
"$CC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os -march=pentium3 -mtune=generic \
  -o "$OUT/dxttest.exe" "$SRC/dxttest.c" -ld3d8 -lgdi32 -luser32
echo "==> cktest.exe (palettized textures and colour keying through the DX7 HAL)"
"$CC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os -march=pentium3 -mtune=generic \
  -o "$OUT/cktest.exe" "$SRC/cktest.c" -lddraw -ldxguid -lgdi32 -luser32
echo "==> zfilltest.exe (a Z buffer reset by a depth fill or through a Lock, doc 19 §34)"
"$CC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os -march=pentium3 -mtune=generic \
  -o "$OUT/zfilltest.exe" "$SRC/zfilltest.c" -lddraw -ldxguid -lgdi32 -luser32
echo "==> ebtest.exe (the DirectX 3 path: execute buffers and texture handles on the HAL)"
"$CC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os -march=pentium3 -mtune=generic \
  -o "$OUT/ebtest.exe" "$SRC/ebtest.c" -lddraw -ldxguid -lgdi32 -luser32
echo "==> shtest.exe (vertex / pixel shaders 1.x through d3d8.dll on the DX8 DDI)"
"$CC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os -march=pentium3 -mtune=generic \
  -o "$OUT/shtest.exe" "$SRC/shtest.c" -ld3d8 -lgdi32 -luser32
for v in 8 9; do
  echo "==> dx${v}caps.exe (what d3d${v}.dll makes of the driver: device types, depth formats, CreateDevice, caps; M16)"
  "$CC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os -march=pentium3 -mtune=generic -DDXVER=$v \
    -o "$OUT/dx${v}caps.exe" "$SRC/dx9caps.c" -lgdi32 -luser32
  echo "==> wtprobe${v}.exe (windowed CreateDevice across the ways Wine's tests and the caps tools differ; M16)"
  "$CC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os -march=pentium3 -mtune=generic -DDXVER=$v \
    -o "$OUT/wtprobe${v}.exe" "$SRC/wtprobe.c" -lgdi32 -luser32
done
echo "==> d9ctest.exe (the resources d3d9.dll creates on the driver, and float render-target readback; M16)"
"$CC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os -march=pentium3 -mtune=generic \
  -o "$OUT/d9ctest.exe" "$SRC/d9ctest.c" -lgdi32 -luser32
# the DX8 feature probes (d3d8probe.h): each one says "not offered" while the
# driver lacks its feature and is the feature's check once it has it
for t in cubetest strmtest voltest fmttest bumptest sprtest anistest patchtst msaatest mgdtest gammatest; do
  echo "==> $t.exe (a DX8 feature probe through d3d8.dll on the DX8 DDI)"
  "$CC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os -march=pentium3 -mtune=generic \
    -o "$OUT/$t.exe" "$SRC/$t.c" -ld3d8 -lgdi32 -luser32
done

# sanity: the kernel modules import only from their port driver, and nothing
# links the CRT (no import at all is the expected answer for the .sys/.dll)
check_imports() {  # file, allowed DLLs (regex, case-insensitive)
  local bad
  bad="$(i686-w64-mingw32-objdump -p "$1" | awk '/DLL Name:/ {print $3}' | grep -ivE "^($2)\$" || true)"
  [ -z "$bad" ] || { echo "ERROR: $1 imports from $bad (expected only $2)"; exit 1; }
}
check_imports "$OUT/d3dptvid.sys" "videoprt.sys|ntoskrnl.exe"
check_imports "$OUT/d3dptdisp.dll" win32k.sys
for f in "$OUT"/*.sys "$OUT"/*.dll "$OUT"/*.exe; do
  if i686-w64-mingw32-objdump -p "$f" | grep -q 'api-ms-win-crt'; then
    echo "ERROR: $f links against the UCRT"; exit 1
  fi
  n=$(i686-w64-mingw32-objdump -d "$f" | grep -cE '\b(movdq[au]|movapd|movupd|pshufd|punpck[hl](bw|wd|dq|qdq)|paddq|cvtsd2|cvtsi2sd|cvttsd2si|movsd[[:space:]]+%xmm|xorpd|andpd|popcnt|ptest|pcmpistr|pshufb|pmaddubsw)\b' || true)
  [ "$n" -eq 0 ] || { echo "ERROR: $f contains $n SSE2+/POPCNT instructions (pentium3 floor)"; exit 1; }
done

crlf() { awk '{ sub(/\r$/, ""); printf "%s\r\n", $0 }'; }
crlf < "$ROOT/guest-tools/README-DRIVER.txt" > "$OUT/README.TXT"
crlf < "$NT/d3dptvid.inf" > "$OUT/d3dptvid.inf"

# 8.3 upper-case names for the ISO folder
# Through a temporary name: on a case-insensitive filesystem (NTFS under
# MSYS2) a case-only rename can be refused as a move onto itself.
( cd "$OUT" && for f in *; do u="$(echo "$f" | tr a-z A-Z)"; [ "$f" = "$u" ] || { mv "$f" "$f.~" && mv "$f.~" "$u"; }; done )
ls -la "$OUT"

ISO="$ROOT/guest-tools/out/d3dpt-driver.iso"
STAGE="$ROOT/guest-tools/out/driver-iso"; rm -rf "$STAGE"; mkdir -p "$STAGE/DRIVER"; cp "$OUT"/* "$STAGE/DRIVER/"
if command -v xorriso >/dev/null; then
  xorriso -as mkisofs -o "$ISO" -V "D3DPTDRV" -J -r "$STAGE" >/dev/null 2>&1
elif command -v genisoimage >/dev/null; then
  genisoimage -o "$ISO" -V "D3DPTDRV" -J -r "$STAGE" >/dev/null 2>&1
elif command -v mkisofs >/dev/null; then
  mkisofs -o "$ISO" -V "D3DPTDRV" -J -r "$STAGE" >/dev/null 2>&1
else
  echo "no ISO tool found; staged files are in $OUT"; exit 0
fi
echo "==> $ISO"
