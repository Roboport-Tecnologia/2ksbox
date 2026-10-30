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
# is made fresh each run, over the image or over the kept install
# (build/test/xpdx9-cache, XPDX9_CACHE=; the install runs again when the
# image, the ISO's driver or the install procedure changed, or FRESH=1).
# Exit 1 when the install or the run did not finish. About 3 minutes under
# KVM; on the PC under WHPX 4.4 with the install, 1.6 from the cache.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${1:?image.qcow2}"; ISO="${2:?guest-tools.iso}"
. "$ROOT/tools/guestwait.sh"
OUT="$(gw_path "$(realpath -m "${OUT:-$ROOT/build/test/xpdx9}")")"; ISO="$(gw_path "$(realpath "$ISO")")"
rm -rf "$OUT"; mkdir -p "$OUT"
# xp-driver-test.sh is run from a copy beside it (it finds the tree from its
# own folder), so editing it meanwhile cannot break this run
XDT="$ROOT/tools/.xdt-$$.sh"; cp "$ROOT/tools/xp-driver-test.sh" "$XDT"; trap 'rm -f "$XDT"' EXIT
xdt() { OUT="$OUT" DRIVER_ISO="$ISO" bash "$XDT" "$OUT/xp.qcow2" "$@"; }

# The installed driver is kept (CACHE): the install, a restart and the wait
# for the driver were 5 of the run's 6.5 minutes on the PC (2026-10-22). The
# key is what the install depends on: the image (path, size, time), the
# files the install takes from the ISO (the driver, its INF and DRVINST)
# and the install procedure (xp-driver-test.sh); anything else changed, the
# driver is installed again, and the cache keeps only the last. The files
# are hashed with their PE time stamps and checksum zeroed: the ISO is
# rebuilt whenever any guest source changes, and a rebuild of the same
# driver differs in nothing else (2026-10-22: a WDDM change reinstalled
# XP's driver). The scratch disk is kept with it: XP gives a new disk the
# next free letter, not E:. FRESH=1 installs anyway. A run from the cache
# still has to show the driver up on its own boot ("linear mode on", below).
CACHE="$(gw_path "$(realpath -m "${XPDX9_CACHE:-$ROOT/build/test/xpdx9-cache}")")"
INST="$OUT/inst-files"; mkdir -p "$INST"
bsdtar -xf "$ISO" -C "$INST" $(bsdtar -tf "$ISO" 2>/dev/null \
  | grep -i -E '^DRIVER/(D3DPTVID\.SYS|D3DPTDISP\.DLL|D3DPTVID\.INF|DRVINST\.EXE)$') 2>/dev/null
KEY=$( { echo "$(realpath "$IMG") $(stat -c '%s %Y' "$IMG")"
         cat "$ROOT/tools/xp-driver-test.sh"
         python3 - "$INST" <<'PY'
import os, struct, sys
for top, _, names in sorted(os.walk(sys.argv[1])):
    for name in sorted(names):
        b = open(os.path.join(top, name), "rb").read()
        if b[:2] == b"MZ":
            pe = struct.unpack_from("<I", b, 0x3c)[0]
            stamp = b[pe + 8:pe + 12]
            b = b[:pe + 88] + b"\0" * 4 + b[pe + 92:]      # CheckSum
            if stamp != b"\0" * 4:
                b = b.replace(stamp, b"\0" * 4)              # every TimeDateStamp
        sys.stdout.buffer.write(name.upper().encode() + b"\0" + b)
PY
       } | sha256sum | cut -c1-16)
rm -rf "$INST"
if [ -z "${FRESH:-}" ] && [ -f "$CACHE/$KEY.qcow2" ] && [ -f "$CACHE/$KEY.scratch" ]; then
  echo "==> the driver installed already ($CACHE/$KEY.qcow2)"
  cp "$CACHE/$KEY.scratch" "$OUT/scratch.img"
  "$GW_QDIR/qemu-img" create -q -f qcow2 -b "$CACHE/$KEY.qcow2" -F qcow2 "$OUT/xp.qcow2" || exit 1
  CACHED=1
else
  "$GW_QDIR/qemu-img" create -q -f qcow2 -b "$(gw_path "$(realpath "$IMG")")" -F qcow2 "$OUT/xp.qcow2" || exit 1
  echo "==> installing the driver"
  xdt install > "$OUT/install.log" 2>&1
  # the desktop programmed once more after the restart, by our driver
  grep -q "linear mode on" "$OUT/qemu-install.log" 2>/dev/null \
    || { echo "the driver did not come up after the install (see $OUT/install.log)"; exit 1; }
  rm -rf "$CACHE"; mkdir -p "$CACHE"
  cp "$OUT/xp.qcow2" "$CACHE/$KEY.qcow2" && cp "$OUT/scratch.img" "$CACHE/$KEY.scratch" \
    || { rm -rf "$CACHE"; echo "(the installed driver could not be kept)"; }
  CACHED=
fi

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
if [ -n "$CACHED" ] && ! grep -q "linear mode on" "$OUT/qemu-bat.log" 2>/dev/null; then
  echo "the cached install's driver did not come up on this boot (see $OUT/qemu-bat.log; FRESH=1 installs again)"; exit 1
fi
echo "==> done: $(ls "$OUT"/*.BMP 2>/dev/null | wc -l) frames"
