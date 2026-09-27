#!/usr/bin/env bash
# XP's Direct3D on the display driver, for scripts/test.sh's guest stage
# (M16 step 7): a fresh overlay on the image, the driver installed from the
# guest-tools ISO, then DDVMTEST, D3DGAME9, D3DGAME8 and D3DFEAT9 in one
# boot through XP's own ddraw.dll, d3d9.dll and d3d8.dll. Nothing is copied
# next to the programs. The caller judges what it leaves in OUT: G9.BMP,
# G8.BMP, F9.BMP and guest-{ddvmtest,d3dgame9,d3dgame8,d3dfeat9}.log.
#
#   tools/xp-dx9-test.sh <image.qcow2> <guest-tools.iso>
#
# Env: OUT (build/test/xpdx9; keep it short, the QMP socket lives there).
# KVM when /dev/kvm exists. The image is only read: the overlay OUT/xp.qcow2
# is made fresh each run. Exit 1 when the install or the run did not finish.
# About 3 minutes under KVM.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${1:?image.qcow2}"; ISO="${2:?guest-tools.iso}"
OUT="$(realpath -m "${OUT:-$ROOT/build/test/xpdx9}")"; ISO="$(realpath "$ISO")"
rm -rf "$OUT"; mkdir -p "$OUT"
"$ROOT/build/qemu/qemu-img" create -q -f qcow2 -b "$(realpath "$IMG")" -F qcow2 "$OUT/xp.qcow2" || exit 1
# xp-driver-test.sh is run from a copy beside it (it finds the tree from its
# own folder), so editing it meanwhile cannot break this run
XDT="$ROOT/tools/.xdt-$$.sh"; cp "$ROOT/tools/xp-driver-test.sh" "$XDT"; trap 'rm -f "$XDT"' EXIT
xdt() { OUT="$OUT" DRIVER_ISO="$ISO" bash "$XDT" "$OUT/xp.qcow2" "$@"; }

echo "==> installing the driver"
xdt install > "$OUT/install.log" 2>&1
# the desktop programmed once more after the restart, by our driver
grep -q "linear mode on" "$OUT/qemu-install.log" 2>/dev/null \
  || { echo "the driver did not come up after the install (see $OUT/install.log)"; exit 1; }

# The run: the desktop at 32 bpp (d3d8 refuses the scenes' windowed
# A8R8G8B8 device on 16), then the four programs, their logs on the scratch
# disk (BOXLOG), COM1's XPDX9DONE ends it
printf '%s\n' '@echo off' 'mkdir E:\OUT' 'cd /d E:\OUT' 'set BOXLOG=E:\OUT' \
  'D:\DRIVER\SETMODE.EXE 1024 768 32 60' 'D:\TESTS\DDVMTEST.EXE' \
  'D:\TESTS\D3DGAME9.EXE -frames 600 -dump 300 E:\OUT\G9.BMP' \
  'D:\TESTS\D3DGAME8.EXE -frames 600 -dump 300 E:\OUT\G8.BMP' \
  'D:\TESTS\D3DFEAT9.EXE -frames 600 -dump 300 E:\OUT\F9.BMP' \
  'echo XPDX9DONE > COM1' > "$OUT/xpdx9.bat"
echo "==> the scenes"
UNTIL=XPDX9DONE CMD_WAIT=300 xdt bat "$OUT/xpdx9.bat" > "$OUT/run.log" 2>&1
FAT="$OUT/scratch.img@@1048576"
for f in G9.BMP G8.BMP F9.BMP; do mcopy -n -i "$FAT" "::/OUT/$f" "$OUT/$f" 2>/dev/null; done
for f in DDVMTEST D3DGAME9 D3DGAME8 D3DFEAT9; do
  mcopy -n -i "$FAT" "::/OUT/$f.LOG" "$OUT/guest-$(echo "$f" | tr A-Z a-z).log" 2>/dev/null
done
grep -q XPDX9DONE "$OUT/serial-bat.log" 2>/dev/null || { echo "the run did not finish (see $OUT/run.log)"; exit 1; }
echo "==> done: $(ls "$OUT"/*.BMP 2>/dev/null | wc -l) frames"
