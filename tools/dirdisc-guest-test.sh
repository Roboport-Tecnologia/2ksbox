#!/usr/bin/env bash
# A host folder in a guest's CD-ROM drive (M5g,
# docs/tracks/m5-dirdisc.md), on the families xp-cdimage-test.sh does not
# cover:
#
#   tools/dirdisc-guest-test.sh ~/vms/win98.qcow2 win98
#   tools/dirdisc-guest-test.sh ~/vms/winxp.qcow2 xp
#
# The machine boots with `-cdrom isodir:<dir>` (no image anywhere) and a
# floppy carrying RUN.BAT, which lists the disc and reads files off it
# through the guest's own file system driver, over COM1. That is the
# proof: the names in the listing and the bytes of the files are what
# libdisc generated from the tree as the guest asked for them.
#
# The folder is deliberately awkward in the ways a user's own folder is:
# a long name, a space in a directory name, a file of exactly one sector
# and one a byte over, an empty file, and a nested directory.
#
# The image is never written: everything goes to a qcow2 overlay under
# build/dirdisc-guest. Local only (needs a guest image), so not in
# scripts/test.sh. The boot is not slept out: the run knocks on the Run
# dialog until the guest answers over COM1 (tools/guestwait.sh).
#
# BIG=1 asks a different question: where does *this guest's* file system
# stop? The folder is then sparse filler with a small marker file after
# each interesting offset: an 80-minute CD (703 MiB, where the drive
# starts reporting a DVD-ROM profile), a 99-minute one (878 MiB, past
# every MSF address), 2 GiB, 4 GiB and a nearly full DVD-9. Each
# marker the guest can `type` back is proof its driver reached that
# offset. Costs no host disk: the filler is never written.
#
# Env: OUT=dir, BOOT_WAIT=s (the cap on that wait, 300), NO_KVM=1, KEEP=1,
#      BIG=1, MARKS="703 878 2048 4096 8000" (MiB, overrides the offsets).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/guestwait.sh"
IMG="${1:?image.qcow2}"; FAMILY="${2:-win98}"
OUT="${OUT:-$ROOT/build/dirdisc-guest}"; mkdir -p "$OUT"
OVL="$OUT/overlay.qcow2"; FLOPPY="$OUT/tools.img"; SRC="$OUT/shared"
LOG="$OUT/serial-$FAMILY.log"; QLOG="$OUT/qemu-$FAMILY.log"; SOCK="$OUT/q.sock"
QEMU="$ROOT/build/qemu/qemu-system-i386"
[ -x "$QEMU" ] || { echo "no $QEMU: build QEMU first"; exit 1; }

# The shared folder itself.
rm -rf "$SRC"; mkdir -p "$SRC"
printf 'a folder is a disc\r\n' > "$SRC/FOLDER.TXT"
: > "$SRC/EMPTY.BIN"
head -c 2048 /dev/zero | tr '\0' 'A' > "$SRC/EXACT.BIN"
head -c 2049 /dev/zero | tr '\0' 'B' > "$SRC/ODD.BIN"

# The offsets a BIG run plants a marker after, and the files that carry
# them. `isodir` lays a directory's files out in identifier order, so one
# rising index over filler and markers alike puts each marker exactly
# where its name says. Filler is capped at 1000 MiB a file: two ISO 9660
# limits (4 GiB an extent) and one Win98 warning (2 GiB) live above that,
# and neither is what this run is asking about.
MARK_FILES=(); MARK_TAGS=(); MARK_MIB=()
if [ -n "${BIG:-}" ]; then
  at=0; idx=0
  for mib in ${MARKS:-703 878 2048 4096 8000}; do
    while [ "$at" -lt "$mib" ]; do
      chunk=$((mib - at)); [ "$chunk" -gt 1000 ] && chunk=1000
      truncate -s "${chunk}M" "$(printf '%s/X%04d.BIN' "$SRC" "$idx")"
      at=$((at + chunk)); idx=$((idx + 1))
    done
    name="$(printf 'X%04d.TXT' "$idx")"; idx=$((idx + 1))
    printf 'MARK%s ok\r\n' "$mib" > "$SRC/$name"
    MARK_FILES+=("$name"); MARK_TAGS+=("MARK$mib ok"); MARK_MIB+=("$mib")
    echo "    marker at $mib MiB: $name"
  done
else
  mkdir -p "$SRC/Patch Notes"
  printf 'long names survive\r\n' > "$SRC/Patch Notes/Read Me First.txt"
fi

# One batch for both families: COMMAND.COM (98) and CMD.EXE (XP) both take
# it, and every line goes to COM1, which needs no writable disk in the
# guest. The CD's letter differs per image, so both D: and E: are tried
# and one of them prints a "not found", which is cheaper than teaching
# this script every image's drive letters.
{
  echo '@echo off'
  echo 'echo ==== the folder as a disc > COM1'
  echo 'dir /b D:\ > COM1'
  echo 'dir /b E:\ > COM1'
  echo 'type D:\FOLDER.TXT > COM1'
  echo 'type E:\FOLDER.TXT > COM1'
  if [ ${#MARK_FILES[@]} -gt 0 ]; then
    for f in "${MARK_FILES[@]}"; do
      echo "type D:\\$f > COM1"
      echo "type E:\\$f > COM1"
    done
  else
    echo 'dir /b "D:\Patch Notes" > COM1'
    echo 'dir /b "E:\Patch Notes" > COM1'
    echo 'type "D:\Patch Notes\Read Me First.txt" > COM1'
    echo 'type "E:\Patch Notes\Read Me First.txt" > COM1'
  fi
  echo 'echo DIRDISCDONE > COM1'
} > "$OUT/RUN.BAT"
python3 - "$OUT/RUN.BAT" <<'CRLF'
import sys
p = sys.argv[1]
text = open(p, newline="").read().replace("\r\n", "\n").replace("\n", "\r\n")
open(p, "w", newline="").write(text)
CRLF
rm -f "$FLOPPY"
if command -v mkfs.fat >/dev/null; then
  mkfs.fat -C -F 12 "$FLOPPY" 1440 >/dev/null
else
  mformat -C -f 1440 -i "$FLOPPY" ::
fi
mcopy -o -i "$FLOPPY" "$OUT/RUN.BAT" ::/RUN.BAT

rm -f "$OVL"; "$ROOT/build/qemu/qemu-img" create -q -f qcow2 -b "$IMG" -F qcow2 "$OVL"
rm -f "$SOCK" "$LOG"
ACCEL=(-cpu pentium3)
[ -e /dev/kvm ] && [ -z "${NO_KVM:-}" ] && ACCEL=(-accel kvm -cpu pentium3)
if [ "$FAMILY" = win98 ]; then
  HW=(-m 256 -vga cirrus -netdev user,id=n0 -device pcnet,netdev=n0)
  SHELL_CMD='command /c A:\RUN.BAT'
else
  HW=(-m 512 -vga std -net none)
  SHELL_CMD='cmd /k A:\RUN.BAT'
fi
"$QEMU" -L "$ROOT/qemu/pc-bios" "${ACCEL[@]}" -machine pc "${HW[@]}" \
  -hda "$OVL" -fda "$FLOPPY" -boot c \
  -cdrom "isodir:$SRC" \
  -usb -device usb-tablet -display none \
  -qmp "unix:$SOCK,server,nowait" -serial "file:$LOG" -monitor none > "$QLOG" 2>&1 &
QPID=$!
Q() { python3 "$ROOT/tools/qmpc.py" "$SOCK" "$@"; }

GW_PID=$QPID
gw_wait_sock "$SOCK" || exit 1
# knock on the Run dialog until the guest's own output turns up on COM1
gw_poke_until "$SOCK" "$FAMILY" "$SHELL_CMD" "${BOOT_WAIT:-300}" test -s "$LOG" || {
  Q screendump "$OUT/$FAMILY-noshell.png" || true
  echo "the guest never ran anything: see $OUT/$FAMILY-noshell.png and $QLOG"
}
gw_wait_log "$LOG" DIRDISCDONE "${WAIT_SECS:-120}" || true
Q screendump "$OUT/$FAMILY-end.png" || true
if [ "$FAMILY" = win98 ]; then
  # a Win98 run ends with a Start-menu shutdown, never a kill (CLAUDE.md)
  Q keys ctrl+esc || true; sleep 2; Q keys u || true; sleep 2; Q keys ret || true
  gw_wait_exit "$QPID" 90 || true
else
  Q json '{"execute":"system_powerdown"}' >/dev/null || true
  gw_wait_exit "$QPID" 60 || true
fi
kill -0 $QPID 2>/dev/null && { [ -n "${KEEP:-}" ] || kill $QPID 2>/dev/null || true; }
wait $QPID 2>/dev/null || true

echo "---- $LOG"; cat "$LOG" || true
fails=0
want() { if grep -qF "$1" "$LOG"; then echo "PASS  $2"; else echo "FAIL  $2 (missing: $1)"; fails=$((fails + 1)); fi; }
echo "----"
want "DIRDISCDONE" "the batch ran to the end"
want "FOLDER.TXT" "the guest listed the generated volume"
want "a folder is a disc" "the guest read a file out of the folder"
want "EMPTY.BIN" "the empty file is in the listing"
if [ ${#MARK_FILES[@]} -gt 0 ]; then
  # Each marker is one offset the guest's own file system driver reached.
  # These are a measurement, not a regression: the first one that fails is
  # this guest's ceiling, and the run says so rather than only failing.
  for i in "${!MARK_TAGS[@]}"; do
    want "${MARK_TAGS[$i]}" "the guest read a file past ${MARK_MIB[$i]} MiB (${MARK_FILES[$i]})"
  done
else
  want "Read Me First.txt" "long names survived (Joliet)"
  want "long names survive" "the guest read a file from a directory with a space in its name"
fi
if [ "$fails" = 0 ]; then
  echo "dirdisc guest test ($FAMILY): PASS"
else
  echo "dirdisc guest test ($FAMILY): FAIL ($fails checks), see $OUT/$FAMILY-*.png and $QLOG"
  exit 1
fi
