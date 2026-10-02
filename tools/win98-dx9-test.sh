#!/usr/bin/env bash
# The Win98 driver's Direct3D battery in one boot (track M16 step 5): the
# reference scenes through Win98's own d3d9.dll / d3d8.dll on the 9x driver,
# the DX8 feature probes, SHTEST / CKTEST / EBTEST and DDTEST's system-memory
# blits. One verdict line per check; exit 1 when any fails.
#
#   tools/win98-dx9-test.sh [image.qcow2]    # default: the base98-br machine
#
# The scenes are judged as on XP (scripts/test.sh guest): D3DGAME9 and
# D3DGAME8 against the native DXVK frame (HUD masked, no tolerance),
# D3DFEAT9's frame byte for byte and its query / getter lines against the
# native run's, except that its A16B16G16R16F readback must fail
# D3DERR_DRIVERINTERNALERROR (0x88760827): Win98's HEL makes no system-memory float surface
# (M16 finding 35, doc 19 §45). The native frames come from
# `scripts/test.sh host` (build/test/g9-native.bmp, f9-native.bmp,
# f9-native.lines).
#
# Works on a raw copy (`RAW=`, build/test/w98.raw by default), never the
# image, through tools/win98-game-test.sh. `FRESH=1` converts it again.
# About 2 minutes under TCG.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
IMG="${1:-$HOME/.local/share/2ksbox/machines/base98-br/disk.qcow2}"
T="$ROOT/build/test"
export RAW="${RAW:-$T/w98.raw}" OUT="${OUT:-$T/w98}"
TESTS="$ROOT/guest-tools/out/iso/TESTS" DRV="$ROOT/guest-tools/out/driver"
PROBES="CUBETEST STRMTEST VOLTEST FMTTEST BUMPTEST SPRTEST ANISTEST PATCHTST MSAATEST MGDTEST SHTEST CKTEST EBTEST"
for f in "$TESTS/D3DGAME9.EXE" "$DRV/DDTEST.EXE" "$ROOT/guest-tools/out/driver9x/setbpp.exe" \
         "$T/g9-native.bmp" "$T/f9-native.bmp" "$T/f9-native.lines"; do
  [ -f "$f" ] || { echo "missing $f (guest-tools/build-*.sh, scripts/test.sh host)"; exit 1; }
done

. "$ROOT/tools/guestwait.sh"
# The copy has to have the driver. A machine set up on Cirrus never got it
# (the PC's base98-br, 2026-10-02), and Windows then stops at the Add New
# Hardware wizard for "Standard PCI graphics adapter". So it is installed
# once into this copy, the way tools/win98-driver-test.sh installs it (the
# INF staged in WINDOWS\INF, Plug and Play takes it on the next boot), and
# the copy keeps it for every later run. The user's image is never written.
if [ -f "$RAW" ] && [ -z "${FRESH:-}" ]; then
  have=$(mtype -i "$(gw_path "$RAW")@@32256" ::/WINDOWS/SYSTEM.DAT 2>/dev/null | grep -a -c -i d3dpt || true)
else
  have=0
fi
if [ "${have:-0}" = 0 ]; then
  echo "==> the copy has no d3dpt-vga driver: installing it once (tools/win98-driver-test.sh install)"
  RAW="$RAW" OUT="$T/w98inst" "$ROOT/tools/win98-driver-test.sh" "$IMG" install > "$T/w98-install.log" 2>&1 \
    || { echo "the driver install failed (see $T/w98-install.log)"; exit 1; }
  grep -a -q -i d3dpt <(mtype -i "$(gw_path "$RAW")@@32256" ::/WINDOWS/SYSTEM.DAT 2>/dev/null) \
    || { echo "the install ran but the copy's registry still has no d3dpt (see $T/w98-install.log)"; exit 1; }
fi

# The run: the desktop at 32 bpp first (d3d8 refuses the scenes' windowed
# A8R8G8B8 device on 16 bpp), the three scenes, the probes, DDTEST; COM1's
# W98DONE ends it
STAGE="$TESTS/D3DGAME9.EXE $TESTS/D3DGAME8.EXE $TESTS/D3DFEAT9.EXE $ROOT/guest-tools/out/driver9x/setbpp.exe $DRV/DDTEST.EXE"
PULL='G9.BMP G8.BMP F9.BMP 2KSBOX\D3DGAME9.LOG 2KSBOX\D3DGAME8.LOG 2KSBOX\D3DFEAT9.LOG 2KSBOX\DDTEST.LOG'
CMD=$'C:\\SETBPP.EXE -save 32\ncd \\'
for g in D3DGAME9:G9 D3DGAME8:G8 D3DFEAT9:F9; do
  CMD+=$'\n'"C:\\${g%%:*}.EXE -frames 600 -dump 300 C:\\${g#*:}.BMP"
done
for p in $PROBES; do
  f=$(ls "$DRV" | grep -i "^$p.exe$") || { echo "missing $DRV/$p.EXE"; exit 1; }
  STAGE+=" $DRV/$f"; PULL+=" 2KSBOX\\$p.LOG"; CMD+=$'\n'"C:\\$p.EXE"
done
CMD+=$'\nC:\\DDTEST.EXE 640 480 32 200 -windowed\necho W98DONE > COM1'
rm -rf "$OUT"
STAGE="$STAGE" PULL="$PULL" GUEST_CMD="$CMD" UNTIL=W98DONE RUN_SECS="${RUN_SECS:-600}" SHOTS=0 \
  "$ROOT/tools/win98-game-test.sh" "$IMG" w98dx9 > "$T/w98-run.log" 2>&1

rc=0
pass() { echo "  PASS $1"; }
fail() { echo "  FAIL $1${2:+ ($2)}"; rc=1; }
cd "$OUT" 2>/dev/null || { echo "  FAIL no run output ($T/w98-run.log)"; exit 1; }
for g in G9 G8; do
  if [ ! -f "$g.BMP" ]; then fail "w98-$g=native" "no frame"
  elif python3 "$ROOT/tools/bmpdiff.py" "$T/g9-native.bmp" "$g.BMP" --mask 0,368,270,112 > "$g-diff.log" 2>&1; then pass "w98-$g=native"
  else fail "w98-$g=native" "$(head -1 "$g-diff.log")"; fi
done
if [ ! -f F9.BMP ]; then fail w98-F9=native "no frame"
elif cmp -s "$T/f9-native.bmp" F9.BMP; then pass w98-F9=native
else fail w98-F9=native "frame differs"; fi
# the native lines with finding 35's readback as Win98 answers it
sed 's/A16B16G16R16F readback 0x00000000 [0-9a-f]*/A16B16G16R16F readback 0x88760827 0000000000000000/' "$T/f9-native.lines" | sort > f9-expected.lines
tr -d '\r' < D3DFEAT9.LOG 2>/dev/null | grep "occlusion query\|getters" | sort > f9-guest.lines
if diff f9-expected.lines f9-guest.lines > f9-lines.diff; then pass w98-F9-log=native; else fail w98-F9-log=native "$OUT/f9-lines.diff"; fi
for p in $PROBES; do
  l=$(ls | grep -i "^$p.log$")
  if [ -z "$l" ]; then fail "w98-$p" "no log"
  elif [ "$p" = PATCHTST ]; then
    grep -q 'failed (not offered' "$l" && pass "w98-$p (not offered, as on XP)" || fail "w98-$p" "offered or failed"
  elif grep -qE ': [1-9][0-9]* cases, 0 failed' "$l"; then pass "w98-$p"
  else fail "w98-$p" "$(tr -d '\r' < "$l" | tail -1)"; fi
done
# DDTEST's system-memory blits: 9x DirectDraw's HEL keeps them with
# DDCAPS_BLT in dwSVBCaps (doc 19 §45's neighbour, doc 15 "Blit caps")
if tr -d '\r' < DDTEST.LOG 2>/dev/null | grep "^sysmem blt:" | grep -q "Blt 00000000 ok, BltFast 00000000 ok, stretched Blt 00000000 ok, keyed BltFast 00000000 ok"; then
  pass w98-ddtest-sysmem
else fail w98-ddtest-sysmem "$(tr -d '\r' < DDTEST.LOG 2>/dev/null | grep '^sysmem blt:')"; fi
exit $rc
