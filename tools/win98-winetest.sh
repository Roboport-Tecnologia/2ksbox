#!/usr/bin/env bash
# Wine's d3d8 / d3d9 conformance tests (track M16) in a Win98 guest, through
# Win98's own DirectX 9.0c runtime on the display driver. The 9x counterpart
# of `tools/xp-driver-test.sh <image> winetest`.
#
#   tools/win98-winetest.sh [image.qcow2]      # default: the base98-br machine
#
# guest-tools/build-winetests.sh first (build/winetest/out) and
# guest-tools/build-driver9x.sh (the driver is re-staged every run by
# tools/win98-game-test.sh, which boots the machine). The tests go to C:\WT,
# RUN.BAT runs each file under WTRUN's time cap, WTRUN writes
# C:\2KSBOX\WINETEST, and the batch's last line tells COM1 "WTALL", which
# ends the run. The folder comes back to OUT/winetest and
# tools/winetest-summary.py prints the table.
#
# Env:
#   WT_TESTS="d3d9:visual d3d8:device"  a subset (default: every file, the
#                       device files last, as on XP)
#   WT_CAP=s            WTRUN's cap per file (3600: TCG)
#   WT_CANARY=1         a device after every test function; the first that
#                       fails stops the run (STOP.TXT names the test; patch 09)
#   WT_SKIP="a,b"       leave those test functions out (patch 09)
#   WT_BASELINE=f       a verdict against reference/winetest/<f>.txt (or a path);
#                       w98-driver is the driver's Win98 baseline
#   WT_SAVE=f           save this run as a baseline
#   RUN_SECS=s          the whole run's cap (6 h)
#   RAW=, FRESH=1, OUT=, DDFLAGS=  as tools/win98-game-test.sh
#
# **Win98 runs under TCG** (CLAUDE.md), so a full run takes hours; a subset
# is the edit-test loop.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${1:-$HOME/.local/share/2ksbox/machines/base98-br/disk.qcow2}"
WT="${WT_DIR:-$ROOT/build/winetest/out}"
export OUT="${OUT:-$ROOT/build/w98wt/run}"
export RAW="${RAW:-$ROOT/build/w98wt/guest.raw}"
QIMG="${QEMU_IMG:-$ROOT/build/qemu/qemu-img}"
export MTOOLS_SKIP_CHECK=1
[ -f "$WT/wtrun.exe" ] || { echo "no $WT/wtrun.exe: run guest-tools/build-winetests.sh"; exit 1; }

# The raw copy is made here rather than by win98-game-test.sh, because the
# tests are staged into it before that script boots it.
mkdir -p "$(dirname "$RAW")"
if [ ! -f "$RAW" ] || [ "${FRESH:-0}" = 1 ]; then
  echo "==> raw copy of $IMG (the user's image is never written)"
  rm -f "$RAW"; "$QIMG" convert -O raw "$IMG" "$RAW"
fi
OFF=$(python3 -c "
import struct
m=open('$RAW','rb').read(512)
for i in range(4):
    e=m[446+i*16:446+(i+1)*16]
    if e[4]: print(struct.unpack_from('<I',e,8)[0]*512); break")
M="$RAW@@$OFF"

mmd -i "$M" ::/WT 2>/dev/null || true
mcopy -o -i "$M" "$WT/wtrun.exe" ::/WT/WTRUN.EXE
mcopy -o -i "$M" "$WT/d3d8_test.exe" ::/WT/D3D8_TEST.EXE
mcopy -o -i "$M" "$WT/d3d9_test.exe" ::/WT/D3D9_TEST.EXE
mdel -i "$M" '::/2KSBOX/WINETEST/*' 2>/dev/null || true

# **3dfx's Voodoo 2 DirectDraw driver out of the way when the machine has no
# card.** base98-br has 3dfx's driver installed; booted without the card,
# d3d8.dll still loaded `3DFX32V2.DLL` and d3d8 visual and device crashed
# inside it. Renamed in the raw copy only (`.OFF`), and back when EXTRA puts
# a voodoo2 on the machine.
case " ${EXTRA:-} " in
  *voodoo2*) mren -i "$M" ::/WINDOWS/SYSTEM/3DFX32V2.OFF ::/WINDOWS/SYSTEM/3DFX32V2.DLL 2>/dev/null || true ;;
  *) mren -i "$M" ::/WINDOWS/SYSTEM/3DFX32V2.DLL ::/WINDOWS/SYSTEM/3DFX32V2.OFF 2>/dev/null \
       && echo "==> 3dfx's 3DFX32V2.DLL renamed .OFF in the raw copy (no voodoo2 on this machine)" || true ;;
esac

# **The desktop at 32 bpp first.** The tests ask for a windowed A8R8G8B8
# back buffer, which d3d8 refuses on a 16 bpp desktop: every d3d8 file then
# fails device creation and skips (the rig's first Win98 run did). Saved in
# the registry (`-save`, into the raw copy only): a full-screen device
# restores the registry's mode when it goes, and every d3d8 device after
# d3d8 visual's first full-screen one failed on a 16 bpp desktop again.
CMD=
if [ "${WT_BPP:-32}" != 0 ]; then
  mcopy -o -i "$M" "$ROOT/guest-tools/out/driver9x/setbpp.exe" ::/WT/SETBPP.EXE
  CMD+="C:\\WT\\SETBPP.EXE -save ${WT_BPP:-32}"$'\n'
fi
# WT_CANARY=1: after every test function one plain device, traced
# ("canary after <test>: device ok" / "NO DEVICE"; winetest patch 09), to
# find the test after which no device can be made
[ -n "${WT_CANARY:-}" ] && CMD+="set WT_CANARY=1"$'\n'
# WT_SKIP="a,b": leave those test functions out (patch 09, the rig's list)
[ -n "${WT_SKIP:-}" ] && CMD+="set WT_SKIP=$WT_SKIP"$'\n'
# one WTRUN per file, so a crash or a hang costs that file only
for t in ${WT_TESTS:-d3d9:visual d3d9:stateblock d3d9:d3d9ex d3d8:visual d3d8:stateblock d3d9:device d3d8:device}; do
  CMD+="C:\\WT\\WTRUN.EXE ${WT_CAP:-3600} C:\\WT\\$(echo "${t%%:*}" | tr a-z A-Z)_TEST.EXE ${t#*:}"$'\n'
done
CMD+='echo WTALL > COM1'

GUEST_CMD="$CMD" UNTIL=WTALL RUN_SECS="${RUN_SECS:-21600}" SHOTS="${SHOTS:-300}" \
  "$ROOT/tools/win98-game-test.sh" "$IMG" winetest

rm -rf "$OUT/winetest"; mkdir -p "$OUT/winetest"
mcopy -n -i "$M" '::/2KSBOX/WINETEST/*' "$OUT/winetest/" 2>/dev/null || true
echo "=== COM1"; sed 's/\r$//; s/^/   /' "$OUT/com1.log" 2>/dev/null || true
# a baseline by name (reference/winetest/<f>.txt) or by path
BL="${WT_BASELINE:-}"; [ -n "$BL" ] && [ ! -f "$BL" ] && BL="$ROOT/reference/winetest/${BL%.txt}.txt"
# with a baseline, its verdict is the exit status (scripts/test.sh guest)
rc=0
python3 "$ROOT/tools/winetest-summary.py" "$OUT/winetest" ${WT_SAVE:+--save "$WT_SAVE"} ${BL:+--baseline "$BL"} || rc=$?
[ -n "$BL" ] && exit $rc
exit 0
