#!/usr/bin/env bash
# Wine's Direct3D conformance tests as guest programs (track M16 step 0,
# docs/tracks/m16-dx9-ddi.md). Fetches the pinned Wine tag's test sources
# (a sparse checkout into build/winetest/, never vendored: they are
# LGPL) and builds d3d8_test.exe / d3d9_test.exe with the ISO's flags.
#
#   guest-tools/build-winetests.sh [outdir]   (default build/winetest/out)
#
# Each EXE is Wine's standalone runner: `d3d9_test.exe visual` runs one
# file, no argument lists them. WINE_TAG overrides the pin.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WINE_TAG=${WINE_TAG:-wine-11.0}
WT="$ROOT/build/winetest"
SRC="$WT/$WINE_TAG"
OUT=${1:-$WT/out}
OBJ="$WT/obj-$WINE_TAG"

if [ ! -f "$SRC/include/wine/debug.h" ]; then
  rm -rf "$SRC"
  git clone -q --depth 1 --filter=blob:none --sparse --branch "$WINE_TAG" \
    https://gitlab.winehq.org/wine/wine.git "$SRC"
  git -C "$SRC" sparse-checkout set --no-cone /include/wine/ \
    /dlls/d3d8/tests/ /dlls/d3d9/tests/
fi
# our queue (patches/winetest/README.md), onto files restored first so a
# re-run applies it to pristine sources
git -C "$SRC" checkout -q -- .
for p in "$ROOT"/patches/winetest/*.patch; do
  git -C "$SRC" apply -p0 "$p"
done

CC=${CC:-i686-w64-mingw32-gcc}
# the ISO's guest flags (build-wrappers.sh): msvcrt, never the UCRT.
# DECLSPEC_EXPORT is Wine's own winnt.h's; mingw's has none.
FLAGS=(-O2 -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os -march=pentium3
  -mtune=generic -I"$SRC/include" -DDECLSPEC_EXPORT=
  -DWINETEST_NO_D3D9ON12 -DWINETEST_NO_WOW64 -DWINETEST_NULL_DEVICE_SKIP ${WINETEST_CFLAGS:-})
mkdir -p "$OUT" "$OBJ"

build() {  # $1 = d3d8 | d3d9
  local dll=$1 dir="$SRC/dlls/$1/tests" objs=() names=() f n
  for f in "$dir"/*.c; do
    n=$(basename "$f" .c)
    names+=("$n")
    # w98compat.h: Win98's missing Unicode display calls
    "$CC" "${FLAGS[@]}" -include "$ROOT/guest-tools/src/winetest/w98compat.h" -c "$f" -o "$OBJ/${dll}_$n.o"
    objs+=("$OBJ/${dll}_$n.o")
  done
  {
    echo '#define STANDALONE'
    echo '#include <windows.h>'
    echo '#include <wine/test.h>'
    for n in "${names[@]}"; do echo "extern void func_$n(void);"; done
    echo 'const struct test winetest_testlist[] = {'
    for n in "${names[@]}"; do echo "  { \"$n\", func_$n },"; done
    echo '  { 0, 0 } };'
  } >"$OBJ/${dll}_testlist.c"
  "$CC" "${FLAGS[@]}" -c "$OBJ/${dll}_testlist.c" -o "$OBJ/${dll}_testlist.o"
  "$CC" "${FLAGS[@]}" -o "$OUT/${dll}_test.exe" "${objs[@]}" \
    "$OBJ/${dll}_testlist.o" "$ROOT/guest-tools/src/winetest/wine_dbg.c" \
    -l"$dll" -luser32 -lgdi32 -luuid   # -luuid: IID_IUnknown, which w98compat.h's early windows.h leaves uninstantiated
  echo "built $OUT/${dll}_test.exe (${names[*]})"
}

build d3d8
build d3d9
"$CC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os -march=pentium3 \
  -mtune=generic -o "$OUT/wtrun.exe" "$ROOT/guest-tools/src/winetest/wtrun.c"
echo "built $OUT/wtrun.exe"

# RUNALL.BAT, for the reference rig (doc 09): copy this folder to the rig,
# start RUNALL.BAT from inside it (98's command.com has no %~dp0, so the
# names are relative), and bring C:\2KSBOX\WINETEST back for
# tools/winetest-summary.py --save
{
  printf '%s\r\n' '@echo off' 'rem Wine d3d8/d3d9 tests, every file (2ksbox M16). Output: C:\2KSBOX\WINETEST' \
    'rem Start from a fresh boot: on Win9x a test that dies inside d3d8.dll / d3d9.dll' \
    'rem can leave DirectDraw locked, and every later device fails until a restart.'
  for dll in d3d9 d3d8; do
    tests=""
    # `device` last, as in xp-driver-test.sh: its fullscreen tests change modes
    for f in "$SRC/dlls/$dll/tests"/*.c; do
      [ "$(basename "$f" .c)" = device ] || tests="$tests $(basename "$f" .c)"
    done
    tests="$tests device"
    printf 'WTRUN.EXE 1800 %s_TEST.EXE%s\r\n' "$(echo "$dll" | tr a-z A-Z)" "$tests"
  done
} >"$OUT/RUNALL.BAT"
echo "wrote $OUT/RUNALL.BAT"
