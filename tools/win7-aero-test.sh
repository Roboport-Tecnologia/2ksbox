#!/usr/bin/env bash
# Windows 7 from its install disc to Aero on the launcher's Windows 7
# family, headless (track M18 finding 14).
#
#   tools/win7-aero-test.sh <windows7-x86.iso>
#
# Two phases. The base, made once per disc and edition and kept: a machine
# from the launcher's own New machine form (`launcherx --wizard-new win7`,
# in a library of its own under OUT), booted with exactly what `launcherx
# --print-args` gives it (the player's audio backend swapped for a null
# one), the disc in its CD drive and tools/win7-aero-test/autounattend.xml
# on a removable USB stick: Windows setup runs unattended to its first
# desktop (14-19 min under TCG on the PC) and the machine powers off.
# Then every run, on an overlay of that base: the guest-tools disc in the
# drive, SETUP /ALL from an administrator's console (the unsigned-driver
# prompt clicked on the tablet), one restart, and the checks:
#
#   setup    SETUP chose the WDDM driver (the adapter's interrupt found)
#   driver   the kernel driver started (its StartDevice in the QEMU log)
#   dwm      dwm.exe has d3dptumd.dll loaded: DWM composes on our driver
#   d3dgame9 D3DGAME9's frame 300 under composition against the native d3d9
#            frame (build/test/g9-native.bmp, scripts/test.sh's host stage)
#
# and a picture of the Start menu over the desktop (OUT/startmenu.png).
# Exit 0 when every check passes. The disc's edition comes from its
# sources\ei.cfg (EDITION="Windows 7 ULTIMATE" overrides, the install.wim
# image name); Starter and Home Basic have no Aero.
#
# Env: OUT (build/win7-aero-test), REBASE=1 (make the base again),
# GUEST_ISO (the newest guest-tools/out/guest-tools-*.iso, which must carry
# WDDM\), LAUNCHERX, INSTALL_WAIT (5400 s, the cap on Windows setup),
# BOOT_WAIT (900 s, the cap on each desktop), KEEP=1 (leave the last
# machine running; QMP in OUT). Needs mtools, python3, and the
# guest-tools ISO built on Windows (build-windows.sh's wddm stage) or with
# the PC's driver (scripts/wddm-prebuilt.sh).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/guestwait.sh"
WIN_ISO="$(gw_path "${1:?usage: $0 <windows7-x86.iso>}")"
[ -f "$WIN_ISO" ] || { echo "no $WIN_ISO"; exit 1; }
OUT="$(gw_path "${OUT:-$ROOT/build/win7-aero-test}")"; mkdir -p "$OUT"
GUEST_ISO="$(gw_path "${GUEST_ISO:-$(ls -t "$ROOT"/guest-tools/out/guest-tools-*.iso 2>/dev/null | head -1)}")"
[ -f "$GUEST_ISO" ] || { echo "no guest-tools ISO: run scripts/build.sh (or build-windows.sh) guest"; exit 1; }
if [ "$GW_WIN" = 1 ]; then
  LX="${LAUNCHERX:-$ROOT/target/x86_64-pc-windows-msvc/release/launcherx.exe}"
  QIMG="$GW_QDIR/qemu-img.exe"
  export PATH="$GW_QDIR:$PATH"      # QEMU's DLLs, for qemu-img too
else
  LX="${LAUNCHERX:-$ROOT/target/release/launcherx}"
  QIMG="$GW_QDIR/qemu-img"
fi
[ -x "$LX" ] || { echo "no $LX: build the launcher's tools"; exit 1; }
SOCK="$(gw_qmp_addr "$OUT")"
Q() { python3 "$ROOT/tools/qmpc.py" "$SOCK" "$@"; }
say() { echo "== $*"; }
export LAUNCHER_LIBRARY_DIR="$OUT/library" LAUNCHER_DISC_LIBRARY="$OUT/discs.toml"
export LAUNCHER_QEMU_IMG_BIN="$(gw_path "$QIMG")"
TOML="$OUT/library/windows-7/machine.toml"

# The edition: the disc's own (sources\ei.cfg's EditionID), as install.wim names it
if [ -z "${EDITION:-}" ]; then
  # UDF, which xorriso and bsdtar do not read (tools/udfcat.py)
  id=$(python3 "$ROOT/tools/udfcat.py" "$WIN_ISO" sources/ei.cfg 2>/dev/null | tr -d '\r' | sed -n '/^\[EditionID\]/{n;p;}' || true)
  EDITION="Windows 7 $(echo "${id:-Ultimate}" | tr a-z A-Z)"
fi
say "disc $WIN_ISO, edition \"$EDITION\""

# boot <log tag> <disk> <cd> [extra QEMU arguments...]: the machine as the
# launcher starts it, with that disk and that disc
boot() {
  local tag=$1 disk=$2 cd=$3 a; shift 3
  local -a args=()
  read -ra raw <<< "$("$LX" --print-args "$TOML" | tr -d '\r')"
  for a in "${raw[@]}"; do
    a=${a//\\//}
    case "$a" in
      file=*disk.qcow2,if=ide,*) a="file=$disk,if=ide,${a#*,if=ide,}" ;;
      if=none,id=cd0,media=cdrom) a="$a,readonly=on,file=$cd" ;;
    esac
    args+=("$a")
  done
  SER="$OUT/serial-$tag.log"; LOG="$OUT/qemu-$tag.log"; rm -f "$SER"
  echo "${args[*]}" > "$OUT/args-$tag.txt"
  "$GW_QDIR/qemu-system-i386" "${args[@]}" -audiodev none,id=embed0 -display none \
    -qmp "$(gw_qmp_opt "$SOCK")" -serial "file:$SER" -monitor none "$@" > "$LOG" 2>&1 &
  GW_PID=$!
  gw_wait_sock "$SOCK" 60
}
power_off() {
  Q json '{"execute":"system_powerdown"}' > /dev/null || true
  gw_wait_exit "$GW_PID" 180 || { Q json '{"execute":"quit"}' > /dev/null || true; gw_wait_exit "$GW_PID" 30 || true; }
}

# ---------------------------------------------------------------- the base
BASE="$OUT/base.qcow2"; WANT="$(basename "$WIN_ISO") $EDITION"
if [ "${REBASE:-0}" = 1 ] || [ ! -f "$BASE" ] || [ "$(cat "$OUT/base.txt" 2>/dev/null)" != "$WANT" ]; then
  say "the base: Windows setup from the disc, unattended"
  rm -rf "$OUT/library" "$BASE" "$OUT/base.txt"
  "$LX" --wizard-new win7 "Windows 7" 40 > /dev/null
  [ -f "$TOML" ] || { echo "the launcher made no $TOML"; exit 1; }
  UA="$OUT/unattend.img"; rm -f "$UA"
  sed "s|@EDITION@|$EDITION|" "$ROOT/tools/win7-aero-test/autounattend.xml" > "$OUT/autounattend.xml"
  mformat -C -f 1440 -i "$UA" ::
  mcopy -i "$UA" "$OUT/autounattend.xml" ::/autounattend.xml
  t0=$(date +%s)
  boot install "$(gw_path "$OUT/library/windows-7/disk.qcow2")" "$WIN_ISO" \
    -drive "file=$UA,if=none,id=ua,format=raw" -device usb-storage,drive=ua,removable=on
  if ! gw_wait_log "$SER" W7-DESKTOP "${INSTALL_WAIT:-5400}"; then
    Q screendump "$OUT/install-stuck.png" || true
    power_off; echo "Windows setup never reached its desktop (OUT/install-stuck.png)"; exit 1
  fi
  say "Windows setup: its first desktop after $(( $(date +%s) - t0 )) s"
  sleep 60                                         # the first logon's own work
  power_off
  mv "$OUT/library/windows-7/disk.qcow2" "$BASE"
  echo "$WANT" > "$OUT/base.txt"
fi

# ----------------------------------------------------------------- the run
RUN="$OUT/run.qcow2"; rm -f "$RUN"
"$QIMG" create -q -f qcow2 -b "$(gw_path "$BASE")" -F qcow2 "$(gw_path "$RUN")"
# a FAT32 scratch disk for D3DGAME9's frame (tools/xp-driver-test.sh's
# recipe), tagged so the guest finds it whatever letter Windows gives it
SCRATCH="$OUT/scratch.img"; rm -f "$SCRATCH"
dd if=/dev/null of="$SCRATCH" bs=1 seek=$((64 * 1048576)) 2>/dev/null
python3 - "$SCRATCH" <<'MBR'
import struct, sys
start, total = 2048, 64 * 2048
mbr = bytearray(512)
mbr[0x1be:0x1be + 16] = struct.pack('<B3sB3sII', 0x00, b'\xfe\xff\xff', 0x0c, b'\xfe\xff\xff', start, total - start)
mbr[510:512] = b'\x55\xaa'
with open(sys.argv[1], 'r+b') as f:
    f.write(bytes(mbr))
MBR
mformat -i "$SCRATCH@@1048576" -F -H 2048 -T $((64 * 2048 - 2048)) ::
printf '%s\r\n' '@echo off' 'set S=%1' \
  'C:\2KSBOX\D3DGAME9.EXE -frames 600 -dump 300 %S%\G9.BMP' 'copy C:\2KSBOX\D3DGAME9.LOG %S%\G9.LOG > nul' \
  'echo G9DONE > COM1' > "$OUT/RUN.BAT"
echo tag > "$OUT/W7SCR.TAG"
mcopy -o -i "$SCRATCH@@1048576" "$OUT/RUN.BAT" "$OUT/W7SCR.TAG" ::/

boot run "$(gw_path "$RUN")" "$GUEST_ISO" -drive "file=$SCRATCH,format=raw,if=ide,index=1"
run() { gw_run_dialog "$SOCK" xp; Q type "$1"; Q keys ret; }
elevated() {  # an administrator's console: the Start menu's search, Ctrl+Shift+Enter, UAC's Alt+Y; then the line
  # slow on purpose: under TCG the search and UAC's secure desktop each take seconds, and a key sent early is lost
  Q keys esc; sleep 3
  Q keys meta_l; sleep 10; Q type cmd; sleep 10
  Q keys ctrl+shift+ret; sleep 20
  Q keys alt+y; sleep 15
  Q type "$1"; Q keys ret
}
gw_poke_until "$SOCK" xp 'cmd /c echo DESK1 > COM1' "${BOOT_WAIT:-900}" grep -q DESK1 "$SER" \
  || { Q screendump "$OUT/nodesk1.png" || true; power_off; echo "no desktop (OUT/nodesk1.png)"; exit 1; }
say "the desktop, before SETUP"
elevated 'mkdir C:\2KSBOX & D:\SETUP.EXE /ALL & type C:\2KSBOX\SETUP.LOG > COM1 & echo SETUPDONE > COM1'
# the unsigned-driver prompt never has the keyboard: "Install this driver
# software anyway" is 18 px right of and 48 px below the centre of the screen
for k in $(seq 1 30); do
  sleep 15
  grep -aq SETUPDONE "$SER" && break
  geom=$(Q screendump "$OUT/setup.png" | sed -n 's/^screendump \([0-9]*\)x\([0-9]*\) .*/\1 \2/p' || true)
  [ -n "$geom" ] && Q click $(( ${geom% *} / 2 + 18 )) $(( ${geom#* } / 2 + 48 )) ${geom% *} ${geom#* } > /dev/null || true
done
gw_wait_log "$SER" SETUPDONE 300 || { Q screendump "$OUT/nosetup.png" || true; power_off; echo "SETUP never finished"; exit 1; }
tr -d '\r' < "$SER" | sed -n '/^Display adapter driver/,/^The device mapper/p' | sed '$d'
run 'shutdown -r -t 0'
gw_wait_log "$LOG" "d3dptkmd: StartDevice" "${BOOT_WAIT:-900}" || true
gw_poke_until "$SOCK" xp 'cmd /c echo DESK2 > COM1' "${BOOT_WAIT:-900}" grep -q DESK2 "$SER" \
  || { Q screendump "$OUT/nodesk2.png" || true; power_off; echo "no desktop after the restart (OUT/nodesk2.png)"; exit 1; }
say "the desktop, after the restart"
sleep 60                                           # DWM's start
run 'cmd /c (tasklist /m d3dptumd.dll & echo UMDDONE) > COM1'
gw_wait_log "$SER" UMDDONE 120 || true
Q keys meta_l; sleep 10
Q screendump "$OUT/startmenu.png" > /dev/null || true
Q keys esc; sleep 3
run 'cmd /c for %d in (E F G H I J K) do @if exist %d:\W7SCR.TAG %d:\RUN.BAT %d:'
gw_wait_log "$SER" G9DONE 600 || true
sleep 20                                           # the lazy writer, for the scratch disk
if [ "${KEEP:-0}" = 1 ]; then
  echo "KEEP=1: the machine is still running (pid $GW_PID, QMP $SOCK)"
else
  power_off
fi

# ---------------------------------------------------------------- verdicts
fail=0
ok() { echo "-- $1: PASS ($2)"; }
no() { echo "-- $1: FAIL ($2)"; fail=1; }
if grep -aq "Windows 7: the WDDM driver" "$SER"; then ok setup "the WDDM driver, $(tr -d '\r' < "$SER" | grep -o "the adapter's interrupt: .*")"
else no setup "SETUP did not choose the WDDM driver"; fi
if grep -aq "d3dptkmd: StartDevice" "$LOG"; then ok driver "StartDevice"; else no driver "no StartDevice in $LOG"; fi
if tr -d '\r' < "$SER" | grep -q "^dwm.exe .*d3dptumd.dll"; then ok dwm "dwm.exe has d3dptumd.dll loaded"
else no dwm "no dwm.exe with d3dptumd.dll"; fi
rm -f "$OUT/G9.BMP" "$OUT/G9.LOG"
mcopy -n -i "$SCRATCH@@1048576" ::/G9.BMP ::/G9.LOG "$OUT/" 2>/dev/null || true
if [ ! -f "$OUT/G9.BMP" ]; then no d3dgame9 "no frame on the scratch disk"
elif [ ! -f "$ROOT/build/test/g9-native.bmp" ]; then echo "-- d3dgame9: frame taken, no native frame to compare (scripts/test.sh host)"
elif python3 "$ROOT/tools/bmpdiff.py" "$ROOT/build/test/g9-native.bmp" "$OUT/G9.BMP" --mask 0,368,270,112 --tolerance 8 \
     --max-over 1200 -o "$OUT/g9-diff.bmp"; then ok d3dgame9 "frame 300 within budget of the native d3d9 frame"
else no d3dgame9 "frame 300 differs from the native frame ($OUT/g9-diff.bmp)"; fi
exit $fail
